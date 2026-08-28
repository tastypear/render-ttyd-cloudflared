/*
 * memlimit — userspace memory cgroup via seccomp USER_NOTIF + LD_PRELOAD.
 *
 * Usage: memlimit --hard=MB --soft=MB -- command [args...]
 *
 * The supervisor (parent) holds the seccomp listener fd and enforces a global
 * hard cap on virtual memory commitments (mmap/brk/mremap) across the entire
 * process tree.  The child installs the filter, passes the listener fd back,
 * sets LD_PRELOAD for the soft-cap shim, and execs the workload.
 *
 * Build: gcc -O2 -o memlimit memlimit.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
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
#ifndef SYS_mremap
#define SYS_mremap 25
#endif

#define BRK_TABLE_SIZE 4096

/* ---- Pass a file descriptor over a Unix domain socket ---- */
static int send_fd(int sock, int fd) {
    char dummy = 'x';
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *(int *)CMSG_DATA(cmsg) = fd;
    return sendmsg(sock, &msg, 0);
}

static int recv_fd(int sock) {
    char dummy;
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    if (recvmsg(sock, &msg, 0) <= 0) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
        return *(int *)CMSG_DATA(cmsg);
    return -1;
}

/* ---- BPF filter: USER_NOTIF on mmap/brk/mremap, allow everything else ---- */
static struct sock_filter filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 0, 5),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mmap,   3, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_brk,    2, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mremap, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
};

int main(int argc, char *argv[]) {
    uint64_t hard_cap = 450ULL * 1024 * 1024;
    uint64_t soft_cap = 350ULL * 1024 * 1024;
    const char *shim_path = "/app/memshim.so";
    int cmd_start = -1;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--hard=", 7) == 0)
            hard_cap = (uint64_t)atol(argv[i] + 7) * 1024 * 1024;
        else if (strncmp(argv[i], "--soft=", 7) == 0)
            soft_cap = (uint64_t)atol(argv[i] + 7) * 1024 * 1024;
        else if (strncmp(argv[i], "--shim=", 7) == 0)
            shim_path = argv[i] + 7;
        else if (strcmp(argv[i], "--") == 0) { cmd_start = i + 1; break; }
    }
    if (cmd_start < 0 || cmd_start >= argc) {
        fprintf(stderr, "Usage: memlimit --hard=MB --soft=MB -- command...\n");
        return 1;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair"); return 1;
    }

    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }

    if (child == 0) {
        /* ---- Child: install filter, pass listener, exec workload ---- */
        close(sv[0]);

        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
            fprintf(stderr, "[memlimit] prctl NO_NEW_PRIVS: %s\n", strerror(errno));
            _exit(2);
        }

        struct sock_fprog prog = {
            .len = sizeof(filter) / sizeof(filter[0]),
            .filter = filter,
        };
        int listener = (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                                     SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
        if (listener < 0) {
            fprintf(stderr, "[memlimit] seccomp NEW_LISTENER: %s\n", strerror(errno));
            _exit(3);
        }

        send_fd(sv[1], listener);
        close(sv[1]);
        close(listener);

        setenv("LD_PRELOAD", shim_path, 1);
        char buf[32];
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)soft_cap);
        setenv("MEMLIMIT_SOFT_CAP", buf, 1);

        execvp(argv[cmd_start], &argv[cmd_start]);
        fprintf(stderr, "[memlimit] exec %s: %s\n", argv[cmd_start], strerror(errno));
        _exit(127);
    }

    /* ---- Supervisor: receive listener, run notification loop ---- */
    close(sv[1]);
    int listener = recv_fd(sv[0]);
    close(sv[0]);

    if (listener < 0) {
        fprintf(stderr, "[memlimit] failed to receive listener fd\n");
        waitpid(child, NULL, 0);
        return 4;
    }

    fprintf(stderr, "[memlimit] started: hard_cap=%lluMB soft_cap=%lluMB pid=%d\n",
            (unsigned long long)hard_cap / (1024 * 1024),
            (unsigned long long)soft_cap / (1024 * 1024), child);

    uint64_t total = 0;
    uint64_t brk_table[BRK_TABLE_SIZE];
    memset(brk_table, 0, sizeof(brk_table));
    int notif_count = 0, deny_count = 0;
    time_t last_log = 0;

    for (;;) {
        int status;
        if (waitpid(child, &status, WNOHANG) > 0) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            fprintf(stderr, "[memlimit] child exited (status=%d), total=%lluMB, notifs=%d, denies=%d\n",
                    code, (unsigned long long)total / (1024 * 1024), notif_count, deny_count);
            return code;
        }

        struct pollfd pfd = { .fd = listener, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0) {
            time_t now = time(NULL);
            if (now != last_log) {
                last_log = now;
                fprintf(stderr, "[memlimit] idle: total=%lluMB notifs=%d denies=%d\n",
                        (unsigned long long)total / (1024 * 1024), notif_count, deny_count);
            }
            continue;
        }

        struct seccomp_notif req;
        memset(&req, 0, sizeof(req));
        if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req) < 0)
            continue;

        notif_count++;

        int64_t delta = 0;
        int deny = 0;
        int nr = req.data.nr;

        if (nr == SYS_mmap) {
            delta = (int64_t)req.data.args[1];
        } else if (nr == SYS_brk) {
            uint64_t new_brk = req.data.args[0];
            int idx = req.pid % BRK_TABLE_SIZE;
            if (new_brk != 0) {
                uint64_t old_brk = brk_table[idx];
                if (old_brk == 0) {
                    brk_table[idx] = new_brk;
                    delta = 0;
                } else {
                    delta = (int64_t)(new_brk - old_brk);
                    if (delta > (1024LL * 1024 * 1024) || delta < -(1024LL * 1024 * 1024)) {
                        /* exec replaced the address space; reset baseline. */
                        brk_table[idx] = new_brk;
                        delta = 0;
                    } else {
                        brk_table[idx] = new_brk;
                    }
                }
            }
        } else if (nr == SYS_mremap) {
            delta = (int64_t)(req.data.args[2] - req.data.args[1]);
        }

        if (delta > 0 && total + (uint64_t)delta > hard_cap) {
            deny = 1;
            deny_count++;
            fprintf(stderr, "[memlimit] DENY pid=%d nr=%d delta=%lldKB total=%lluMB cap=%lluMB\n",
                    req.pid, nr, (long long)delta / 1024,
                    (unsigned long long)total / (1024 * 1024),
                    (unsigned long long)hard_cap / (1024 * 1024));
        } else if (delta > 0) {
            total += (uint64_t)delta;
        } else if (delta < 0) {
            uint64_t abs_delta = (uint64_t)(-delta);
            total = total > abs_delta ? total - abs_delta : 0;
        }

        struct seccomp_notif_resp resp;
        memset(&resp, 0, sizeof(resp));
        resp.id = req.id;
        if (deny) {
            resp.val = -ENOMEM;
        } else {
            resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
        }
        ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp);

        if (notif_count % 500 == 0) {
            fprintf(stderr, "[memlimit] total=%lluMB notifs=%d denies=%d\n",
                    (unsigned long long)total / (1024 * 1024), notif_count, deny_count);
        }
    }
}
