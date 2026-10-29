/* bench.c - speed of the compiled core alone (and of harness netcpu for
 * comparison) on a flat 64 KB bus.
 *
 *   bench_ccpu [CYCLES] [ROMDIR] [netsim]
 *
 * Workloads:
 *   boot  KERNAL 901227-03 + BASIC 901226-01 on flat RAM with the I/O stubs
 *         of cpu6510/lockstep.c (raster counter, CIA1 timer IRQ every 16421
 *         cycles); from power-up through the RAM test to READY.
 *   loop  the mixed-addressing-mode loop of cpu6510's bench.
 * Built with -DCCPU_STATS (bench_stats) it also prints passes, evaluations
 * and node changes per cycle. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ccpu.h"
#include "lcpu.h"
#include "netcpu.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint8_t mem[65536], kernal[8192], basic[8192];
static int have_rom;
static uint64_t cyc, next_timer;
static int irq_line, timer_on;

static uint8_t rd(uint16_t a) {
    if (have_rom) {
        if (a >= 0xE000) return kernal[a - 0xE000];
        if (a >= 0xA000 && a < 0xC000) return basic[a - 0xA000];
        if (a >= 0xD000 && a < 0xE000) {
            if (a == 0xD012) return (uint8_t)((cyc / 63) % 312);
            if (a == 0xD011) return (uint8_t)((((cyc / 63) % 312) >> 1) & 0x80);
            if (a == 0xD019) return 0x71;
            if (a == 0xDC00 || a == 0xDC01) return 0xFF;
            if (a == 0xDC0D) { uint8_t v = irq_line ? 0x81 : 0; irq_line = 0; return v; }
            return 0;
        }
    }
    return mem[a];
}
static void wr(uint16_t a, uint8_t v) {
    if (have_rom && a == 0xDC0E) timer_on = v & 1;
    if (have_rom && a >= 0xD000 && a < 0xE000) return;
    mem[a] = v;
}

static void setup(int boot) {
    have_rom = boot;
    cyc = 0; irq_line = 0; timer_on = 0; next_timer = 17045;
    if (boot) {
        for (int i = 0; i < 65536; i++) mem[i] = (i & 0x40) ? 0xFF : 0x00;
    } else {
        static const uint8_t prog[] = {
            0xA2, 0x00, 0xA0, 0x10, 0xBD, 0x00, 0x30, 0x7D, 0x00, 0x31, 0x9D, 0x00, 0x32, 0xFE, 0x00, 0x33,
            0xB1, 0x80, 0x91, 0x82, 0x48, 0x68, 0x20, 0x40, 0x10, 0xE8, 0xD0, 0xE8, 0x88, 0xD0, 0xE5, 0x4C,
            0x00, 0x10 };
        memset(mem, 0, sizeof mem);
        memcpy(mem + 0x1000, prog, sizeof prog);
        mem[0x1040] = 0x0A; mem[0x1041] = 0x2A; mem[0x1042] = 0x60;
        mem[0x80] = 0x00; mem[0x81] = 0x34; mem[0x82] = 0x00; mem[0x83] = 0x35;
        mem[0xFFFC] = 0x00; mem[0xFFFD] = 0x10;
    }
}

static uint64_t ctl(void) {
    uint64_t c = CPU_RDY;
    if (cyc < 8) c |= CPU_RES;
    if (have_rom && timer_on && cyc >= next_timer) { irq_line = 1; next_timer = cyc + 16421; }
    if (irq_line) c |= CPU_IRQ;
    return c;
}

static double run_ccpu(uint64_t cycles, int boot, ccpu_t *c, uint16_t *pc) {
    setup(boot);
    ccpu_init(c);
    uint64_t pins = CPU_RW | CPU_RDY;
    double t0 = now();
    for (cyc = 0; cyc < cycles; cyc++) {
        pins = (pins & ~(CPU_IRQ | CPU_NMI | CPU_RES | CPU_RDY)) | ctl();
        pins = ccpu_tick(c, pins);
        uint16_t a = CPU_GET_ADDR(pins);
        if (pins & CPU_RW) pins = CPU_SET_DATA(pins, rd(a)); else wr(a, CPU_GET_DATA(pins));
    }
    double dt = now() - t0;
    *pc = ccpu_pc(c);
    return cycles / dt;
}

static double run_lcpu(uint64_t cycles, int boot, lcpu_t *c, uint16_t *pc) {
    setup(boot);
    lcpu_init(c);
    uint64_t pins = CPU_RW | CPU_RDY;
    double t0 = now();
    for (cyc = 0; cyc < cycles; cyc++) {
        pins = (pins & ~(CPU_IRQ | CPU_NMI | CPU_RES | CPU_RDY)) | ctl();
        pins = lcpu_tick(c, pins);
        uint16_t a = CPU_GET_ADDR(pins);
        if (pins & CPU_RW) pins = CPU_SET_DATA(pins, rd(a)); else wr(a, CPU_GET_DATA(pins));
    }
    double dt = now() - t0;
    *pc = lcpu_pc(c);
    return cycles / dt;
}

static double run_netcpu(uint64_t cycles, int boot, uint16_t *pc, double *evals) {
    setup(boot);
    netcpu_t c;
    netcpu_init(&c);
    uint64_t g0 = netsim_stats(c.sim)->groups;
    uint64_t pins = CPU_RW | CPU_RDY;
    double t0 = now();
    for (cyc = 0; cyc < cycles; cyc++) {
        pins = (pins & ~(CPU_IRQ | CPU_NMI | CPU_RES | CPU_RDY)) | ctl();
        pins = netcpu_tick(&c, pins);
        uint16_t a = CPU_GET_ADDR(pins);
        if (pins & CPU_RW) pins = CPU_SET_DATA(pins, rd(a)); else wr(a, CPU_GET_DATA(pins));
    }
    double dt = now() - t0;
    *pc = netcpu_pc(&c);
    *evals = (double)(netsim_stats(c.sim)->groups - g0) / cycles;
    netcpu_free(&c);
    return cycles / dt;
}

int main(int argc, char **argv) {
    uint64_t cycles = argc > 1 ? strtoull(argv[1], NULL, 0) : 2000000;
    const char *dir = argc > 2 ? argv[2] : "../harness/roms";
    int with_net = argc > 3 && !strcmp(argv[3], "netsim");
    char path[1024];
    snprintf(path, sizeof path, "%s/kernal.901227-03.bin", dir);
    FILE *f = fopen(path, "rb");
    if (!f || fread(kernal, 1, 8192, f) != 8192) { fprintf(stderr, "no %s\n", path); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/basic.901226-01.bin", dir);
    f = fopen(path, "rb");
    if (!f || fread(basic, 1, 8192, f) != 8192) { fprintf(stderr, "no %s\n", path); return 1; }
    fclose(f);
    static ccpu_t c;
    const char *wl[2] = { "loop", "boot" };
    for (int boot = 1; boot >= 0; boot--) {
        double best = 0;
        uint16_t pc = 0;
        for (int rep = 0; rep < 3; rep++) {
            double r = run_ccpu(cycles, boot, &c, &pc);
            if (r > best) best = r;
        }
        printf("ccpu   %-4s %llu cycles: %9.0f cycles/s (%.3fx real time PAL C64)  PC=%04X\n", wl[boot],
               (unsigned long long)cycles, best, best / 985248.0, pc);
#ifdef CCPU_STATS
        const ccpu_stats_t *st = ccpu_stats(&c);
        printf("       per cycle: %.2f passes, %.1f queue entries (netsim's evaluations); %.1f change a lone node, "
               "%.1f are group walks (%.1f nodes), the rest are skipped branch-free; %.1f node changes\n",
               (double)st->passes / cycles, (double)st->evals / cycles, (double)st->lone / cycles,
               (double)st->walks / cycles, (double)st->group_nodes / cycles, (double)st->changes / cycles);
#endif
        {
            static lcpu_t l;
            double lbest = 0;
            uint16_t lpc = 0;
            for (int rep = 0; rep < 3; rep++) {
                double r = run_lcpu(cycles, boot, &l, &lpc);
                if (r > lbest) lbest = r;
            }
            const lcpu_stats_t *ls = lcpu_stats(&l);
            double nc = (double)cycles;
            printf("lcpu   %-4s %llu cycles: %9.0f cycles/s (%.3fx real time PAL C64)  PC=%04X\n",
                   wl[boot], (unsigned long long)cycles, lbest, lbest / 985248.0, lpc);
            printf("       per cycle: %.0f phase-component evaluations in the two sweeps + %.2f cones, %.2f extra SCC iterations, %.2f ALU rounds\n",
                   (double)(LC_COMP_PHI1 + LC_COMP_PHI2) * ls->sweeps / 2 / nc, (double)ls->cones / nc,
                   (double)ls->scc_iters / nc, (double)ls->sg_rounds / nc);
        }
        if (with_net) {
            double ev;
            double r = run_netcpu(cycles, boot, &pc, &ev);
            printf("netcpu %-4s %llu cycles: %9.0f cycles/s (%.3fx real time)  PC=%04X  %.1f group evaluations/cycle\n",
                   wl[boot], (unsigned long long)cycles, r, r / 985248.0, pc, ev);
        }
    }
    return 0;
}
