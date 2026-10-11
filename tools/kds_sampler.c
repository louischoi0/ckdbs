// kds_sampler - a CPU-time sampling profiler loaded with LD_PRELOAD.
//
// BK-S1 (instructions/v3.0.0/workorder-bk-opt-read-update-path.md) needs a
// sampled split of where a statement's server CPU goes, and `perf` is
// refused on the measuring host (perf_event_paranoid = 4, no root). This is
// the smallest thing that answers it: ITIMER_PROF delivers SIGPROF per slice
// of process CPU time to the thread that spent it, the handler records the
// interrupted PC and the frame-pointer chain above it, and the samples are
// written out as raw addresses for `tools/kds_sampler_report.py` to
// symbolize against /proc/<pid>/maps.
//
// The chain is only as good as the frame pointers: build the profiled
// binary with `-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer`. A frame
// without one truncates the chain there, never corrupts it - every step is
// bounds-checked against the interrupted stack pointer.
//
// Control, all by signal so a driver can bracket exactly the window it
// measures:
//   SIGUSR1  start sampling (discarding nothing already kept)
//   SIGUSR2  stop sampling and write every kept sample to
//            $KDS_SAMPLER_OUT.<n> (n counts the dumps), then clear
// Environment: KDS_SAMPLER_OUT (required; no file is written without it),
// KDS_SAMPLER_HZ (default 1000; the kernel's tick bounds the real rate).
//
// Not a tool the engine ships: it lives in tools/ beside the drivers that
// use it, and nothing under src/ knows it exists.
#define _GNU_SOURCE
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

enum { kMaxDepth = 48, kMaxSamples = 1 << 20 };

typedef struct {
    uint32_t depth;
    uint64_t pc[kMaxDepth];
} Sample;

static Sample* g_samples;
static atomic_uint g_next;
static atomic_int g_on;
static unsigned g_dumps;
static int g_hz = 1000;

static void on_prof(int sig, siginfo_t* info, void* uctx) {
    (void)sig;
    (void)info;
    if (!atomic_load_explicit(&g_on, memory_order_relaxed)) return;
    unsigned i = atomic_fetch_add_explicit(&g_next, 1, memory_order_relaxed);
    if (i >= kMaxSamples) return;
    const ucontext_t* uc = (const ucontext_t*)uctx;
    Sample* s = &g_samples[i];
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    uint64_t sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP];
    uint64_t fp = (uint64_t)uc->uc_mcontext.gregs[REG_RBP];
    uint32_t d = 0;
    s->pc[d++] = pc;
    // A frame record is [saved fp, return address] at fp. Each must sit
    // above the last and within 8 MiB of the interrupted stack pointer.
    uint64_t lo = sp;
    while (d < kMaxDepth && fp >= lo && fp < sp + (8u << 20) && (fp & 7) == 0) {
        const uint64_t* rec = (const uint64_t*)fp;
        uint64_t ret = rec[1];
        if (ret < 4096) break;
        s->pc[d++] = ret - 1;  // inside the call instruction, for line lookup
        lo = fp + 16;
        fp = rec[0];
    }
    s->depth = d;
}

static void arm(int on) {
    struct itimerval t;
    memset(&t, 0, sizeof t);
    if (on) {
        t.it_interval.tv_usec = 1000000 / g_hz;
        t.it_value.tv_usec = 1000000 / g_hz;
    }
    setitimer(ITIMER_PROF, &t, NULL);
}

static void dump(void) {
    const char* base = getenv("KDS_SAMPLER_OUT");
    if (base == NULL) return;
    char path[4096];
    snprintf(path, sizeof path, "%s.%u", base, g_dumps++);
    FILE* f = fopen(path, "w");
    if (f == NULL) return;
    unsigned n = atomic_load(&g_next);
    if (n > kMaxSamples) n = kMaxSamples;
    fprintf(f, "# hz %d samples %u\n", g_hz, n);
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps != NULL) {
        char line[4096];
        while (fgets(line, sizeof line, maps) != NULL) fprintf(f, "M %s", line);
        fclose(maps);
    }
    for (unsigned i = 0; i < n; ++i) {
        fputc('S', f);
        for (uint32_t d = 0; d < g_samples[i].depth; ++d) fprintf(f, " %lx", g_samples[i].pc[d]);
        fputc('\n', f);
    }
    fclose(f);
    atomic_store(&g_next, 0);
}

// The control signals run their work on a thread of the kernel's choosing;
// stdio there is not async-signal-safe, which is acceptable for a profiling
// harness whose driver sends one signal and waits.
static void on_ctl(int sig) {
    if (sig == SIGUSR1) {
        atomic_store(&g_on, 1);
        arm(1);
    } else {
        atomic_store(&g_on, 0);
        arm(0);
        dump();
    }
}

__attribute__((constructor)) static void init(void) {
    if (getenv("KDS_SAMPLER_OUT") == NULL) return;
    const char* hz = getenv("KDS_SAMPLER_HZ");
    if (hz != NULL && atoi(hz) > 0) g_hz = atoi(hz);
    g_samples = calloc(kMaxSamples, sizeof(Sample));
    if (g_samples == NULL) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct sigaction ctl;
    memset(&ctl, 0, sizeof ctl);
    ctl.sa_handler = on_ctl;
    ctl.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &ctl, NULL);
    sigaction(SIGUSR2, &ctl, NULL);
}
