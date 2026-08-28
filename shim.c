#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void *(*real_malloc)(size_t) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void (*real_free)(void *) = NULL;
static int initialized = 0;
static int initializing = 0;

/* static buffer for allocations during dlsym (which itself may call malloc) */
static char tmpbuf[65536];
static size_t tmpbuf_off = 0;

static void init(void) {
    if (initialized || initializing) return;
    initializing = 1;
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_free = dlsym(RTLD_NEXT, "free");
    initializing = 0;
    initialized = 1;
    fprintf(stderr, "SHIM_MALLOC_CALLED: shim loaded, real_malloc=%p\n", (void *)real_malloc);
}

void *malloc(size_t size) {
    if (!initialized) init();
    if (!real_malloc || initializing) {
        /* bootstrap: serve from static buffer */
        size_t aligned = (size + 15) & ~((size_t)15);
        if (tmpbuf_off + aligned > sizeof(tmpbuf)) return NULL;
        void *p = tmpbuf + tmpbuf_off;
        tmpbuf_off += aligned;
        return p;
    }
    return real_malloc(size);
}

void *calloc(size_t nmemb, size_t size) {
    if (!initialized) init();
    if (!real_calloc || initializing) {
        /* bootstrap */
        size_t total = nmemb * size;
        void *p = malloc(total);
        if (p) memset(p, 0, total);
        return p;
    }
    return real_calloc(nmemb, size);
}

void free(void *ptr) {
    if (!initialized) init();
    /* never free bootstrap allocations (in tmpbuf) */
    if (ptr && (char *)ptr >= tmpbuf && (char *)ptr < tmpbuf + sizeof(tmpbuf))
        return;
    if (real_free) real_free(ptr);
}
