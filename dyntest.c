#include <stdlib.h>
#include <stdio.h>
int main(void) {
    void *p = malloc(128);
    void *q = calloc(16, 8);
    printf("DYNTEST_DONE: malloc=%p calloc=%p\n", p, q);
    free(p);
    free(q);
    return 0;
}
