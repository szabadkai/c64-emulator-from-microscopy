/* cputest.c - validation of the netlist CPU alone on a flat 64 KB memory:
 * runs the hand-written programs from asm/cputests/ (assembled into
 * gen/cputests.h) and checks memory, registers and cycle counts.  Also a
 * raw speed benchmark of the netlist CPU without the rest of the machine. */
#include "cputest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../gen/cputests.h"
#include "netcpu.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static double cpu_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef struct {
    netcpu_t cpu;
    uint8_t mem[65536];
    int64_t first_sync[65536];
    uint64_t pins;
    uint64_t cycle;
} flat_t;

static void flat_setup(flat_t *f, const cputest_t *t) {
    netcpu_init(&f->cpu);
    memset(f->mem, 0, sizeof f->mem);
    memcpy(f->mem + t->org, t->img, t->len);
    f->mem[0xFFFC] = t->start & 0xFF;
    f->mem[0xFFFD] = t->start >> 8;
    for (int i = 0; i < 65536; i++) f->first_sync[i] = -1;
    f->pins = CPU_RDY | CPU_RES;
    f->cycle = 0;
}

static void flat_tick(flat_t *f) {
    if (f->cycle == 4) f->pins &= ~CPU_RES;
    f->pins = netcpu_tick(&f->cpu, f->pins);
    uint16_t a = CPU_GET_ADDR(f->pins);
    if (f->pins & CPU_RW) f->pins = CPU_SET_DATA(f->pins, f->mem[a]);
    else f->mem[a] = CPU_GET_DATA(f->pins);
    if ((f->pins & CPU_SYNC) && f->first_sync[a] < 0) f->first_sync[a] = (int64_t)f->cycle;
    f->cycle++;
}

int cputest_run(int verbose) {
    int failed_total = 0, checks_total = 0;
    flat_t *f = calloc(1, sizeof(flat_t));
    for (int i = 0; i < NUM_CPUTESTS; i++) {
        const cputest_t *t = &cputests[i];
        flat_setup(f, t);
        bool reached = false;
        while (f->cycle < 200000) {
            flat_tick(f);
            if ((f->pins & CPU_SYNC) && CPU_GET_ADDR(f->pins) == t->done) { reached = true; break; }
        }
        int fails = 0;
        printf("[%s] %s: %s after %llu cycles\n", reached ? "run " : "FAIL", t->name,
               reached ? "reached done" : "did NOT reach done", (unsigned long long)f->cycle);
        if (!reached) {
            fails++;
            printf("    PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X\n", netcpu_pc(&f->cpu), netcpu_a(&f->cpu),
                   netcpu_x(&f->cpu), netcpu_y(&f->cpu), netcpu_s(&f->cpu), netcpu_p(&f->cpu));
        }
        for (int k = 0; k < t->nchecks; k++) {
            const cpucheck_t *c = &t->checks[k];
            long got = 0;
            char what[64];
            switch (c->kind) {
            case CHK_MEM: got = f->mem[c->addr]; snprintf(what, sizeof what, "mem[$%04X]", c->addr); break;
            case CHK_A: got = netcpu_a(&f->cpu); snprintf(what, sizeof what, "A"); break;
            case CHK_X: got = netcpu_x(&f->cpu); snprintf(what, sizeof what, "X"); break;
            case CHK_Y: got = netcpu_y(&f->cpu); snprintf(what, sizeof what, "Y"); break;
            case CHK_S: got = netcpu_s(&f->cpu); snprintf(what, sizeof what, "S"); break;
            case CHK_P: got = netcpu_p(&f->cpu); snprintf(what, sizeof what, "P"); break;
            default:
                got = (f->first_sync[c->addr] < 0 || f->first_sync[c->addr2] < 0) ? -1
                      : (long)(f->first_sync[c->addr2] - f->first_sync[c->addr]);
                snprintf(what, sizeof what, "cycles $%04X->$%04X", c->addr, c->addr2);
                break;
            }
            bool ok = got == (long)c->value;
            checks_total++;
            if (!ok) fails++;
            if (verbose || !ok)
                printf("    %s %-22s expected $%02X (%u) got $%02lX (%ld)\n", ok ? "ok  " : "FAIL", what,
                       c->value, c->value, got < 0 ? 0 : got, got);
        }
        printf("    %d/%d checks passed\n", t->nchecks - (fails > t->nchecks ? t->nchecks : fails), t->nchecks);
        failed_total += fails;
        netcpu_free(&f->cpu);
    }
    printf("CPU tests: %d checks, %d failed\n", checks_total, failed_total);
    free(f);
    return failed_total ? 1 : 0;
}

double cputest_bench(uint64_t cycles) {
    flat_t *f = calloc(1, sizeof(flat_t));
    const cputest_t *t = &cputests[2];   /* loops/multiply program, restarted forever */
    flat_setup(f, t);
    f->mem[t->done] = 0x4C;              /* done: JMP start */
    f->mem[t->done + 1] = t->start & 0xFF;
    f->mem[t->done + 2] = t->start >> 8;
    for (int i = 0; i < 20; i++) flat_tick(f);
    double t0 = now(), c0 = cpu_now();
    for (uint64_t i = 0; i < cycles; i++) flat_tick(f);
    double dt = now() - t0, ct = cpu_now() - c0;
    const netsim_stats_t *st = netsim_stats(f->cpu.sim);
    printf("netlist CPU alone: %llu cycles in %.3f s wall = %.0f cycles/s (%.2f%% of 0.985 MHz)\n",
           (unsigned long long)cycles, dt, cycles / dt, 100.0 * cycles / dt / 985248.0);
    printf("                   %.3f s process CPU time = %.0f cycles/s (%.2f%% of 0.985 MHz, 1/%.1f)\n",
           ct, cycles / ct, 100.0 * cycles / ct / 985248.0, 985248.0 / (cycles / ct));
    printf("  %d transistors after de-duplication; per cycle: %.1f group evaluations, "
           "%.1f nodes/group, %.1f passes; loop-limit hits %llu\n",
           netcpu_transistors(&f->cpu), (double)st->groups / f->cpu.cycles,
           (double)st->group_nodes / (st->groups ? st->groups : 1), (double)st->passes / f->cpu.cycles,
           (unsigned long long)st->loop_limit_hits);
    netcpu_free(&f->cpu);
    free(f);
    return cycles / ct;
}
