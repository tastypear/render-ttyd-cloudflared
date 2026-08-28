#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/utsname.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <poll.h>
#include <stddef.h>
#include <time.h>

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif
#ifndef SECCOMP_IOCTL_NOTIF_RECV
#define SECCOMP_IOCTL_NOTIF_RECV _IOWR('S', 0x80, struct seccomp_notif)
#endif
#ifndef SECCOMP_IOCTL_NOTIF_SEND
#define SECCOMP_IOCTL_NOTIF_SEND _IOWR('S', 0x81, struct seccomp_notif_resp)
#endif
#ifndef SYS_mmap
#define SYS_mmap 9
#endif
#ifndef SYS_brk
#define SYS_brk 12
#endif

static char outbuf[65536];
static size_t outlen = 0;
#define OUTF(...) do { \
    outlen += snprintf(outbuf + outlen, sizeof(outbuf) > outlen ? sizeof(outbuf) - outlen : 0, __VA_ARGS__); \
    if (outlen >= sizeof(outbuf)) outlen = sizeof(outbuf) - 1; \
} while (0)

/* write-formatted helper that never calls malloc/stdio (safe under seccomp filter) */
static void wfmt(int fd, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) { if ((size_t)n > sizeof(buf)) n = sizeof(buf); write(fd, buf, n); }
}

/*
 * seccomp USER_NOTIF test:
 *   mid child: prctl(NO_NEW_PRIVS) + install filter (USER_NOTIF on mmap) + get listener fd
 *   mid forks grandchild: grandchild calls mmap -> triggers notification
 *   mid (supervisor): recv notification, respond CONTINUE, reap grandchild
 *   Main process stays filter-free so it can do stdio/HTTP freely.
 */
static void test_user_notif(void) {
    int pipefd[2];
    if (pipe(pipefd) < 0) { OUTF("  pipe fail: %s\n", strerror(errno)); return; }

    pid_t mid = fork();
    if (mid < 0) { OUTF("  fork fail: %s\n", strerror(errno)); return; }

    if (mid == 0) {
        /* Child A (supervisor): install filter, fork grandchild, handle notifications */
        close(pipefd[0]);

        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
            wfmt(pipefd[1], "prctl NO_NEW_PRIVS fail: %s\n", strerror(errno));
            _exit(1);
        }

        struct sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 0, 3),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mmap, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog = {
            .len = sizeof(filter) / sizeof(filter[0]),
            .filter = filter,
        };

        int listener = (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                                     SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
        if (listener < 0) {
            wfmt(pipefd[1], "seccomp NEW_LISTENER fail: %s (errno %d)\n", strerror(errno), errno);
            _exit(2);
        }
        wfmt(pipefd[1], "listener fd acquired: %d\n", listener);

        pid_t grand = fork();
        if (grand < 0) { wfmt(pipefd[1], "fork2 fail\n"); _exit(3); }

        if (grand == 0) {
            /* Grandchild: mmap triggers notification, then we report success */
            close(pipefd[1]);
            void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) _exit(0x80 | (errno & 0x7f));
            *(volatile char *)p = 42;          /* page fault (not a syscall) */
            if (munmap(p, 4096) < 0) _exit(0x90);
            _exit(0);
        }

        /* Supervisor: wait for notification */
        struct seccomp_notif req;
        memset(&req, 0, sizeof(req));
        int r = ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req);
        if (r < 0) {
            wfmt(pipefd[1], "NOTIF_RECV fail: %s (errno %d)\n", strerror(errno), errno);
            waitpid(grand, NULL, 0);
            _exit(4);
        }
        wfmt(pipefd[1], "notification received: pid=%d nr=%d (mmap=%d) len=%llu\n",
             req.pid, req.data.nr, SYS_mmap,
             (unsigned long long)req.data.args[1]);

        struct seccomp_notif_resp resp;
        memset(&resp, 0, sizeof(resp));
        resp.id = req.id;
        resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
        r = ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp);
        if (r < 0) {
            wfmt(pipefd[1], "NOTIF_SEND (CONTINUE) fail: %s (errno %d)\n", strerror(errno), errno);
            /* Try without CONTINUE: respond error=0 val=0 (won't really help mmap, but test ioctl) */
            resp.flags = 0; resp.error = 0; resp.val = 0;
            r = ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp);
            wfmt(pipefd[1], "NOTIF_SEND (plain) ret=%d errno=%d\n", r, errno);
        } else {
            wfmt(pipefd[1], "NOTIF_SEND (CONTINUE) OK\n");
        }

        int status;
        waitpid(grand, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            wfmt(pipefd[1], "RESULT: USER_NOTIF_OK - grandchild mmap succeeded after CONTINUE\n");
        else
            wfmt(pipefd[1], "RESULT: USER_NOTIF_FAIL - grandchild exit=0x%x\n", WEXITSTATUS(status));
        close(pipefd[1]);
        _exit(0);
    }

    /* Main: wait for supervisor to finish, then collect all output */
    close(pipefd[1]);
    waitpid(mid, NULL, 0);
    char tmp[8192];
    ssize_t total = 0, n;
    while ((n = read(pipefd[0], tmp + total, sizeof(tmp) - 1 - total)) > 0)
        total += n;
    if (total > 0) { tmp[total] = 0; OUTF("  %s", tmp); }
    else OUTF("  (no output from supervisor)\n");
    close(pipefd[0]);
}

static void test_ld_preload(void) {
    int pipefd[2];
    pipe(pipefd);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[1]);
        setenv("LD_PRELOAD", "/app/shim.so", 1);
        execl("/app/dyntest", "dyntest", NULL);
        _exit(127);
    }
    close(pipefd[1]);
    waitpid(pid, NULL, 0);
    char tmp[4096];
    ssize_t total = 0, n;
    while ((n = read(pipefd[0], tmp + total, sizeof(tmp) - 1 - total)) > 0)
        total += n;
    if (total > 0) tmp[total] = 0; else tmp[0] = 0;
    close(pipefd[0]);
    int has_shim = strstr(tmp, "SHIM_MALLOC_CALLED") != NULL;
    int has_done = strstr(tmp, "DYNTEST_DONE") != NULL;
    OUTF("  output: %s", tmp);
    OUTF("  shim intercepted malloc: %s\n", has_shim ? "YES" : "NO");
    OUTF("  dyntest ran to completion: %s\n", has_done ? "YES" : "NO");
    OUTF("  RESULT: LD_PRELOAD %s\n", has_shim ? "OK" : "FAIL");
}

static void test_rlimit_as(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl) == 0) {
        OUTF("  RLIMIT_AS cur=%lu max=%lu\n", (unsigned long)rl.rlim_cur, (unsigned long)rl.rlim_max);
        OUTF("  RESULT: RLIMIT_AS available (kernel-enforced per-process virtual cap)\n");
    } else {
        OUTF("  RESULT: RLIMIT_AS getrlimit fail: %s\n", strerror(errno));
    }
}

static void test_cgroup(void) {
    OUTF("  /sys/fs/cgroup/ exists: %s\n", access("/sys/fs/cgroup/", F_OK) == 0 ? "yes" : "no");
    OUTF("  /sys/fs/cgroup/memory.max (v2) exists: %s\n", access("/sys/fs/cgroup/memory.max", F_OK) == 0 ? "yes" : "no");
    OUTF("  /sys/fs/cgroup/memory.max writable: %s\n", access("/sys/fs/cgroup/memory.max", W_OK) == 0 ? "YES" : "no");
    OUTF("  /sys/fs/cgroup/memory/memory.limit_in_bytes (v1) exists: %s\n",
         access("/sys/fs/cgroup/memory/memory.limit_in_bytes", F_OK) == 0 ? "yes" : "no");
}

/* ---- Test: USER_NOTIF denial (enforce a cap by returning -ENOMEM) ---- */
static void test_denial(void) {
    int pipefd[2]; pipe(pipefd);
    pid_t mid = fork();
    if (mid == 0) {
        close(pipefd[0]);
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        struct sock_filter flt[] = {
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, arch)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 0, 3),
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_mmap, 0, 1),
            BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_USER_NOTIF),
            BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog = { 6, flt };
        int ln = (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
        if (ln < 0) { wfmt(pipefd[1], "NEW_LISTENER fail: %s\n", strerror(errno)); _exit(2); }
        pid_t g = fork();
        if (g == 0) {
            close(pipefd[1]);
            long ret = syscall(SYS_mmap, NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            int e = errno;
            wfmt(2, "denial child: ret=%ld errno=%d\n", ret, e);
            if (ret < 0 && e == ENOMEM) _exit(0);
            _exit(1);
        }
        struct seccomp_notif req; memset(&req, 0, sizeof(req));
        ioctl(ln, SECCOMP_IOCTL_NOTIF_RECV, &req);
        wfmt(pipefd[1], "denying mmap pid=%d nr=%d\n", req.pid, req.data.nr);
        struct seccomp_notif_resp resp; memset(&resp, 0, sizeof(resp));
        resp.id = req.id; resp.error = 0; resp.val = -ENOMEM;  /* raw return value */
        int r = ioctl(ln, SECCOMP_IOCTL_NOTIF_SEND, &resp);
        wfmt(pipefd[1], "SEND(deny) ret=%d errno=%d\n", r, errno);
        int st; waitpid(g, &st, 0);
        wfmt(pipefd[1], "RESULT: %s\n",
             WIFEXITED(st) && WEXITSTATUS(st)==0 ? "DENIAL_OK - child got ENOMEM" : "DENIAL_FAIL");
        close(pipefd[1]); _exit(0);
    }
    close(pipefd[1]); waitpid(mid, NULL, 0);
    char tmp[4096]; ssize_t tot=0, n;
    while ((n=read(pipefd[0], tmp+tot, sizeof(tmp)-1-tot))>0) tot+=n;
    if (tot>0) { tmp[tot]=0; OUTF("  %s", tmp); } else OUTF("  (no output)\n");
    close(pipefd[0]);
}

/* ---- Test: USER_NOTIF latency (1000 mmap cycles with/without filter) ---- */
static void test_latency(void) {
    /* baseline: no filter */
    int bp[2]; pipe(bp);
    pid_t b = fork();
    if (b==0) { close(bp[0]);
        struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<1000;i++) { void *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p!=MAP_FAILED) munmap(p,4096); }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        long ms=(t1.tv_sec-t0.tv_sec)*1000+(t1.tv_nsec-t0.tv_nsec)/1000000;
        wfmt(bp[1], "baseline: %ld ms / 1000 mmap+munmap\n", ms);
        close(bp[1]); _exit(0);
    }
    close(bp[1]); waitpid(b,NULL,0);
    char bb[256]; ssize_t bn=read(bp[0],bb,sizeof(bb)-1); if(bn>0) bb[bn]=0; else bb[0]=0; close(bp[0]);
    OUTF("  %s", bb);

    /* with USER_NOTIF filter */
    int mp[2], gp[2]; pipe(mp); pipe(gp);
    pid_t mid=fork();
    if (mid==0) {
        close(mp[0]);
        prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0);
        struct sock_filter flt[]={
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data,arch)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 0, 3),
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data,nr)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_mmap, 0, 1),
            BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_USER_NOTIF),
            BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog={6,flt};
        int ln=(int)syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_NEW_LISTENER,&prog);
        if(ln<0){ wfmt(mp[1],"NEW_LISTENER fail\n"); close(gp[0]); close(mp[1]); _exit(2); }
        pid_t g=fork();
        if(g==0){
            close(mp[1]); close(gp[0]);
            struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
            for(int i=0;i<1000;i++){ void *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p!=MAP_FAILED) munmap(p,4096); }
            clock_gettime(CLOCK_MONOTONIC,&t1);
            long ms=(t1.tv_sec-t0.tv_sec)*1000+(t1.tv_nsec-t0.tv_nsec)/1000000;
            wfmt(gp[1],"with_filter: %ld ms / 1000 mmap+munmap\n",ms);
            close(gp[1]); _exit(0);
        }
        close(gp[1]); /* mid doesn't write to gp; grandchild inherited it */
        /* supervisor: handle notifications until grandchild exits */
        int count=0;
        for(;;){
            int st; if(waitpid(g,&st,WNOHANG)>0) break;
            struct pollfd pfd={.fd=ln,.events=POLLIN};
            if(poll(&pfd,1,50)<=0) continue;
            struct seccomp_notif req; memset(&req,0,sizeof(req));
            if(ioctl(ln,SECCOMP_IOCTL_NOTIF_RECV,&req)<0) break;
            struct seccomp_notif_resp resp; memset(&resp,0,sizeof(resp));
            resp.id=req.id; resp.flags=SECCOMP_USER_NOTIF_FLAG_CONTINUE;
            ioctl(ln,SECCOMP_IOCTL_NOTIF_SEND,&resp); count++;
        }
        waitpid(g,NULL,0);
        char tb[256]; ssize_t tn=read(gp[0],tb,sizeof(tb)-1); if(tn>0) tb[tn]=0; else tb[0]=0;
        wfmt(mp[1],"%snotifications handled: %d\n",tb,count);
        close(gp[0]); close(mp[1]); _exit(0);
    }
    close(mp[1]); close(gp[0]); close(gp[1]); waitpid(mid,NULL,0);
    char tmp[4096]; ssize_t tot=0,n;
    while((n=read(mp[0],tmp+tot,sizeof(tmp)-1-tot))>0) tot+=n;
    if(tot>0){tmp[tot]=0; OUTF("  %s",tmp);} else OUTF("  (no output)\n");
    close(mp[0]);
}

/* ---- Test: brk interception via USER_NOTIF ---- */
static void test_brk_notif(void) {
    int pipefd[2]; pipe(pipefd);
    pid_t mid=fork();
    if(mid==0){
        close(pipefd[0]);
        prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0);
        struct sock_filter flt[]={
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data,arch)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 0, 3),
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data,nr)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_brk, 0, 1),
            BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_USER_NOTIF),
            BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog={6,flt};
        int ln=(int)syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_NEW_LISTENER,&prog);
        if(ln<0){ wfmt(pipefd[1],"NEW_LISTENER fail: %s\n",strerror(errno)); _exit(2); }
        pid_t g=fork();
        if(g==0){
            close(pipefd[1]);
            unsigned long cur=(unsigned long)syscall(SYS_brk,0);
            wfmt(2,"brk child: cur=%lu\n",cur);
            unsigned long nw=(unsigned long)syscall(SYS_brk,cur+4096);
            wfmt(2,"brk child: nw=%lu expected>=%lu\n",nw,cur+4096);
            if(nw>=cur+4096) _exit(0);
            _exit(1);
        }
        int count=0, all_brk=1, child_status=0, got_status=0;
        for(;;){
            int st; if(waitpid(g,&st,WNOHANG)>0) { child_status=st; got_status=1; break; }
            struct pollfd pfd={.fd=ln,.events=POLLIN};
            if(poll(&pfd,1,50)<=0) continue;
            struct seccomp_notif req; memset(&req,0,sizeof(req));
            if(ioctl(ln,SECCOMP_IOCTL_NOTIF_RECV,&req)<0) break;
            if(req.data.nr!=SYS_brk) all_brk=0;
            struct seccomp_notif_resp resp; memset(&resp,0,sizeof(resp));
            resp.id=req.id; resp.flags=SECCOMP_USER_NOTIF_FLAG_CONTINUE;
            ioctl(ln,SECCOMP_IOCTL_NOTIF_SEND,&resp); count++;
        }
        if(!got_status) { int st; waitpid(g,&st,0); child_status=st; }
        wfmt(pipefd[1],"notifications: %d, all_brk=%d, child_exit=%d, termsig=%d\n",
             count,all_brk,WIFEXITED(child_status)?WEXITSTATUS(child_status):-1,WIFSIGNALED(child_status)?WTERMSIG(child_status):-1);
        wfmt(pipefd[1],"RESULT: %s\n",
             WIFEXITED(child_status)&&WEXITSTATUS(child_status)==0&&all_brk ? "BRK_OK - brk intercepted and CONTINUE worked" : "BRK_FAIL");
        close(pipefd[1]); _exit(0);
    }
    close(pipefd[1]); waitpid(mid,NULL,0);
    char tmp[4096]; ssize_t tot=0,n;
    while((n=read(pipefd[0],tmp+tot,sizeof(tmp)-1-tot))>0) tot+=n;
    if(tot>0){tmp[tot]=0; OUTF("  %s",tmp);} else OUTF("  (no output)\n");
    close(pipefd[0]);
}

/* ---- Test: multi-process notifications on shared listener ---- */
static void test_multiproc(void) {
    int pipefd[2]; pipe(pipefd);
    pid_t mid=fork();
    if(mid==0){
        close(pipefd[0]);
        prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0);
        struct sock_filter flt[]={
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data,arch)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 0, 3),
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data,nr)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_mmap, 0, 1),
            BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_USER_NOTIF),
            BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog={6,flt};
        int ln=(int)syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_NEW_LISTENER,&prog);
        if(ln<0){ wfmt(pipefd[1],"NEW_LISTENER fail\n"); _exit(2); }
        pid_t kids[3]; int k;
        for(k=0;k<3;k++){
            kids[k]=fork();
            if(kids[k]==0){
                close(pipefd[1]);
                void *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                if(p!=MAP_FAILED){ *(volatile char*)p=1; munmap(p,4096); _exit(0); }
                _exit(1);
            }
        }
        int count=0, pids[3]={0,0,0}, unique=1;
        for(;;){
            int all_done=1;
            for(k=0;k<3;k++){ int st; if(waitpid(kids[k],&st,WNOHANG)==0) all_done=0; }
            if(all_done) break;
            struct pollfd pfd={.fd=ln,.events=POLLIN};
            if(poll(&pfd,1,50)<=0) continue;
            struct seccomp_notif req; memset(&req,0,sizeof(req));
            if(ioctl(ln,SECCOMP_IOCTL_NOTIF_RECV,&req)<0) break;
            if(count<3) pids[count]=req.pid;
            struct seccomp_notif_resp resp; memset(&resp,0,sizeof(resp));
            resp.id=req.id; resp.flags=SECCOMP_USER_NOTIF_FLAG_CONTINUE;
            ioctl(ln,SECCOMP_IOCTL_NOTIF_SEND,&resp); count++;
        }
        int ok=0;
        for(k=0;k<3;k++){ int st; waitpid(kids[k],&st,0); if(WIFEXITED(st)&&WEXITSTATUS(st)==0) ok++; }
        for(k=1;k<3;k++) if(pids[k]==pids[0]||pids[k]==0) unique=0;
        wfmt(pipefd[1],"notifications=%d, children_ok=%d/3, unique_pids=%d\n",count,ok,unique);
        wfmt(pipefd[1],"pids=%d,%d,%d\n",pids[0],pids[1],pids[2]);
        wfmt(pipefd[1],"RESULT: %s\n",
             ok==3&&unique ? "MULTIPROC_OK - 3 children, 3 distinct pids, all succeeded" : "MULTIPROC_FAIL");
        close(pipefd[1]); _exit(0);
    }
    close(pipefd[1]); waitpid(mid,NULL,0);
    char tmp[4096]; ssize_t tot=0,n;
    while((n=read(pipefd[0],tmp+tot,sizeof(tmp)-1-tot))>0) tot+=n;
    if(tot>0){tmp[tot]=0; OUTF("  %s",tmp);} else OUTF("  (no output)\n");
    close(pipefd[0]);
}

static void serve_http(int port, const char *body) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { fprintf(stderr, "socket fail: %s\n", strerror(errno)); return; }
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind fail: %s\n", strerror(errno)); return;
    }
    listen(s, 8);
    fprintf(stderr, "serving probe results on port %d\n", port);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        char hdr[256];
        size_t blen = strlen(body);
        int hl = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n", blen);
        write(c, hdr, hl);
        write(c, body, blen);
        close(c);
    }
}

int main(void) {
    struct utsname u;
    uname(&u);
    OUTF("=== MEMPROBE RESULTS ===\n\n");
    OUTF("kernel: %s %s %s\n", u.sysname, u.release, u.machine);
    OUTF("uid: %d  euid: %d\n\n", getuid(), geteuid());

    /* prctl NO_NEW_PRIVS */
    int ps = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    OUTF("[prctl NO_NEW_PRIVS]\n");
    OUTF("  set: %s (errno %d)\n\n", ps == 0 ? "OK" : "FAIL", errno);

    /* seccomp USER_NOTIF */
    OUTF("[seccomp USER_NOTIF]\n");
    test_user_notif();
    OUTF("\n");

    /* LD_PRELOAD */
    OUTF("[LD_PRELOAD]\n");
    test_ld_preload();
    OUTF("\n");

    /* RLIMIT_AS */
    OUTF("[RLIMIT_AS]\n");
    test_rlimit_as();
    OUTF("\n");

    /* cgroup */
    OUTF("[cgroup]\n");
    test_cgroup();
    OUTF("\n");

    /* USER_NOTIF denial (enforcement) */
    OUTF("[USER_NOTIF denial - can we ENFORCE a cap?]\n");
    test_denial();
    OUTF("\n");

    /* USER_NOTIF latency */
    OUTF("[USER_NOTIF latency - overhead per notification]\n");
    test_latency();
    OUTF("\n");

    /* brk interception */
    OUTF("[brk interception via USER_NOTIF]\n");
    test_brk_notif();
    OUTF("\n");

    /* multi-process shared listener */
    OUTF("[multi-process notifications on shared listener]\n");
    test_multiproc();
    OUTF("\n=== END ===\n");

    /* Print to stderr so it appears in container logs */
    fprintf(stderr, "%s", outbuf);
    fflush(stderr);

    int port = 7681;
    const char *p = getenv("PORT");
    if (p && atoi(p) > 0) port = atoi(p);
    serve_http(port, outbuf);
    return 0;
}
