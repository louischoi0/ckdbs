#include <stdio.h>
#include <time.h>
#include <stdint.h>
int main(void) {
    struct timespec t, s, e;
    uint64_t acc = 0;
    int n = 20000000;
    clock_gettime(CLOCK_MONOTONIC, &s);
    for (int i = 0; i < n; i++) { clock_gettime(CLOCK_MONOTONIC, &t); acc += t.tv_nsec; }
    clock_gettime(CLOCK_MONOTONIC, &e);
    double ns = ((e.tv_sec - s.tv_sec) * 1e9 + (e.tv_nsec - s.tv_nsec)) / n;
    printf("clock_gettime(CLOCK_MONOTONIC): %.1f ns per call (acc %llu)\n", ns, (unsigned long long)acc);
    return 0;
}
