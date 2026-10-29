/* boardstep.c - board-level lockstep: two complete C64 boards from the
 * harness sources, one with the netlist 6510 (harness c64.c/port6510.c as
 * they are), one with fastcpu (the same c64.c/port6510.c compiled a second
 * time with their symbols renamed fc64_* / fm6510_* and netcpu_init/tick/free
 * bound to fastcpu by fastshim.c; see the Makefile).  This is exactly the
 * "--cpu fast" integration path: the real VIC (BA/AEC, badlines, sprites),
 * CIAs, PLA, SID and keyboard drive both CPUs.
 *
 *   boardstep [ROMDIR] [CYCLES]     default ../harness/roms 4000000
 *
 * Each cycle the CPU pin masks of both boards (address, R/W, SYNC, data) are
 * compared; once READY. is on screen, "PRINT 2+2" and a small BASIC program
 * are typed through the keyboard matrix. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "c64.h"
#include "fastcpu.h"

/* the second board (renamed copy of harness c64.c) */
int  fc64_init(c64_t *m, sid_model model);
void fc64_free(c64_t *m);
int  fc64_load_roms(c64_t *m, const char *dir, char *err, size_t errlen);
void fc64_tick(c64_t *m);
void fc64_type(c64_t *m, const char *text);
bool fc64_typing(const c64_t *m);
bool fc64_screen_contains(c64_t *m, const char *s);
void fc64_screen_text(c64_t *m, char out[25][41]);
fastcpu_t *fastshim_cpu(void);

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define HIST 24
int main(int argc, char **argv) {
    const char *romdir = argc > 1 ? argv[1] : "../harness/roms";
    uint64_t cycles = argc > 2 ? strtoull(argv[2], NULL, 0) : 4000000;
    static c64_t A, B;
    char err[2048];
    if (c64_init(&A, SID_6581) || fc64_init(&B, SID_6581)) { fprintf(stderr, "init failed\n"); return 1; }
    if (c64_load_roms(&A, romdir, err, sizeof err) || fc64_load_roms(&B, romdir, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    /* fastcpu_init's power-up registers are the netlist's settle values */
    const netcpu_t *nc = &A.cpu.cpu;
    fastcpu_regs_t fr = fastcpu_regs(fastshim_cpu());
    if (fr.pc != netcpu_pc(nc) || fr.a != netcpu_a(nc) || fr.x != netcpu_x(nc) || fr.y != netcpu_y(nc) ||
        fr.s != netcpu_s(nc) || fr.p != netcpu_p(nc))
        printf("note: power-up registers differ (netlist PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X)\n",
               netcpu_pc(nc), netcpu_a(nc), netcpu_x(nc), netcpu_y(nc), netcpu_s(nc), netcpu_p(nc));

    static const char *const script[] = { "PRINT 2+2\\n", "10 FORI=1TO30:POKE53280,I:NEXT\\n", "RUN\\n", NULL };
    int line = 0;
    uint64_t hist[HIST][2], last_frame = 0, stalled = 0;
    const uint64_t mask = CPU_A_MASK | CPU_RW | CPU_SYNC | CPU_D_MASK;
    double t0 = now();
    uint64_t n;
    int bad = 0;
    for (n = 0; n < cycles; n++) {
        c64_tick(&A);
        fc64_tick(&B);
        hist[n % HIST][0] = A.cpu_pins; hist[n % HIST][1] = B.cpu_pins;
        if (!(A.cpu_pins & CPU_RDY) && (A.cpu_pins & CPU_RW)) stalled++;
        if ((A.cpu_pins & mask) != (B.cpu_pins & mask)) { bad = 1; break; }
        if (A.vic.frame != last_frame) {
            last_frame = A.vic.frame;
            if (script[line] && !c64_typing(&A) && c64_screen_contains(&A, "READY.") && A.vic.frame > (uint64_t)(120 + 60 * line)) {
                c64_type(&A, script[line]);
                fc64_type(&B, script[line]);
                line++;
            }
        }
    }
    double dt = now() - t0;
    if (bad) {
        printf("*** MISMATCH at board cycle %llu (raster line %d, cycle %d)\n", (unsigned long long)n,
               A.vic.line, A.vic.cycle);
        for (int k = HIST - 1; k >= 0; k--) {
            if ((int64_t)n - k < 0) continue;
            uint64_t a = hist[(n - k) % HIST][0], b = hist[(n - k) % HIST][1];
            printf("  %s %8lld  netlist %04X %c %02X %s %s | fast %04X %c %02X %s\n",
                   (a & mask) != (b & mask) ? ">>" : "  ", (long long)(n - k), CPU_GET_ADDR(a), (a & CPU_RW) ? 'R' : 'W',
                   CPU_GET_DATA(a), (a & CPU_SYNC) ? "S" : " ", (a & CPU_RDY) ? "  " : "BA",
                   CPU_GET_ADDR(b), (b & CPU_RW) ? 'R' : 'W', CPU_GET_DATA(b), (b & CPU_SYNC) ? "S" : " ");
        }
    }
    char t[25][41];
    c64_screen_text(&A, t);
    printf("netlist board screen:\n");
    for (int r = 0; r < 25; r++) {
        int blank = 1;
        for (int c = 0; c < 40; c++) if (t[r][c] != ' ') blank = 0;
        if (!blank) printf("  |%s|\n", t[r]);
    }
    char u[25][41];
    fc64_screen_text(&B, u);
    printf("fast board screen %s the netlist board's\n", memcmp(t, u, sizeof t) ? "DIFFERS from" : "is identical to");
    printf("%llu cycles (%llu frames), CPU stalled by BA on %llu read cycles, %llu IRQs / %llu NMIs taken\n",
           (unsigned long long)n, (unsigned long long)A.vic.frame, (unsigned long long)stalled,
           (unsigned long long)A.irqs_taken, (unsigned long long)A.nmis_taken);
    printf("RESULT board cycles=%llu mismatches=%d  (%.1f s)\n", (unsigned long long)n, bad, dt);
    c64_free(&A);
    fc64_free(&B);
    return bad;
}
