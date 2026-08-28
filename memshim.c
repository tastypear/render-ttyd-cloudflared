/*
 * memshim.so — LD_PRELOAD allocator shim for memlimit.
 *
 * Intercepts malloc/calloc/realloc/free to provide graceful ENOMEM when
 * per-process logical allocations approach the soft cap (MEMLIMIT_SOFT_CAP
 * env, in bytes).  The hard cap is enforced by the seccomp layer; this shim
 * gives programs a chance to handle OOM gracefully before that happens.
 *
 * Build: gcc -O2 -shared -fPIC -o memshim.so memshim.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#define MAGIC 0x4D4C5353  /* "MLSS" — memlimit shim signature */
#define HEADER_SIZE sizeof(struct alloc_header)

struct alloc_header {
    uint32_t magic;
    uint32_t padding;
    uint64_t size;
};

static void *(*real_malloc)(size_t) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;
static void  (*real_free)(void *) = NULL;

static uint64_t total_allocated = 0;
static uint64_t soft_cap = 0;
static int initialized = 0;
static int initializing = 0;

/* Bootstrap buffer for allocations during dlsym (which may call malloc). */
static char bootstrap_buf[65536];
static size_t bootstrap_off = 0;

/* Simple spinlock for thread safety. */
static volatile int lock = 0;
static void spin_lock(void)   { while (__sync_lock_test_and_set(&lock, 1)) { /* spin */ } }
static void spin_unlock(void) { __sync_lock_release(&lock); }

static void init(void) {
    if (initialized) return;
    if (initializing) return;
    initializing = 1;

    real_malloc  = dlsym(RTLD_NEXT, "malloc");
    real_calloc  = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_free    = dlsym(RTLD_NEXT, "free");

    const char *cap_str = getenv("MEMLIMIT_SOFT_CAP");
    if (cap_str) soft_cap = strtoull(cap_str, NULL, 10);

    initializing = 0;
    initialized = 1;
}

static int is_bootstrap(void *p) {
    return (char *)p >= bootstrap_buf && (char *)p < bootstrap_buf + sizeof(bootstrap_buf);
}

void *malloc(size_t size) {
    if (!initialized) init();

    if (!real_malloc || initializing) {
        /* Bootstrap: serve from static buffer. */
        size_t aligned = (size + 15) & ~((size_t)15);
        if (bootstrap_off + aligned > sizeof(bootstrap_buf)) return NULL;
        void *p = bootstrap_buf + bootstrap_off;
        bootstrap_off += aligned;
        return p;
    }

    spin_lock();
    if (soft_cap > 0 && total_allocated + size > soft_cap) {
        spin_unlock();
        errno = ENOMEM;
        return NULL;
    }
    spin_unlock();

    void *raw = real_malloc(size + HEADER_SIZE);
    if (!raw) return NULL;

    struct alloc_header *h = (struct alloc_header *)raw;
    h->magic = MAGIC;
    h->size = size;

    spin_lock();
    total_allocated += size;
    spin_unlock();

    return (char *)raw + HEADER_SIZE;
}

void *calloc(size_t nmemb, size_t size) {
    if (!initialized) init();

    size_t total = nmemb * size;

    if (!real_calloc || initializing) {
        size_t aligned = (total + 15) & ~((size_t)15);
        if (bootstrap_off + aligned > sizeof(bootstrap_buf)) return NULL;
        void *p = bootstrap_buf + bootstrap_off;
        bootstrap_off += aligned;
        memset(p, 0, total);
        return p;
    }

    spin_lock();
    if (soft_cap > 0 && total_allocated + total > soft_cap) {
        spin_unlock();
        errno = ENOMEM;
        return NULL;
    }
    spin_unlock();

    void *raw = real_calloc(1, total + HEADER_SIZE);
    if (!raw) return NULL;

    struct alloc_header *h = (struct alloc_header *)raw;
    h->magic = MAGIC;
    h->size = total;

    spin_lock();
    total_allocated += total;
    spin_unlock();

    return (char *)raw + HEADER_SIZE;
}

void *realloc(void *ptr, size_t size) {
    if (!initialized) init();

    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    if (!real_realloc || initializing) return NULL;
    if (is_bootstrap(ptr)) {
        /* Can't realloc bootstrap allocations; do a manual copy. */
        void *new = malloc(size);
        if (new) memcpy(new, ptr, size);
        return new;
    }

    struct alloc_header *h = (struct alloc_header *)((char *)ptr - HEADER_SIZE);
    if (h->magic != MAGIC) {
        /* Not our allocation; fall through to real_realloc. */
        return real_realloc(ptr, size);
    }

    uint64_t old_size = h->size;

    spin_lock();
    if (soft_cap > 0 && total_allocated - old_size + size > soft_cap) {
        spin_unlock();
        errno = ENOMEM;
        return NULL;
    }
    spin_unlock();

    void *raw = real_realloc(h, size + HEADER_SIZE);
    if (!raw) return NULL;

    h = (struct alloc_header *)raw;
    h->magic = MAGIC;
    h->size = size;

    spin_lock();
    total_allocated = total_allocated - old_size + size;
    spin_unlock();

    return (char *)raw + HEADER_SIZE;
}

void free(void *ptr) {
    if (!initialized) init();
    if (!ptr) return;
    if (is_bootstrap(ptr)) return;
    if (!real_free) return;

    struct alloc_header *h = (struct alloc_header *)((char *)ptr - HEADER_SIZE);
    if (h->magic == MAGIC) {
        spin_lock();
        total_allocated -= h->size;
        spin_unlock();
        real_free(h);
    } else {
        /* Not our allocation; pass through. */
        real_free(ptr);
    }
}
