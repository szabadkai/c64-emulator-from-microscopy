/* cboard.c - board-level lockstep: two complete C64 boards built from the
 * harness sources.  Board A is harness c64.c/port6510.c as they are (netlist
 * CPU, netsim); board B is the same c64.c/port6510.c compiled a second time
 * with renamed symbols (cc64_*, cm6510_*) and netcpu_init/tick/free bound to
 * the compiled core (test/ccpu_shim.c) -- the "--cpu compiled" integration
 * path.  VIC (badlines, BA/AEC), CIAs, PLA, SID and keyboard drive both.
 *
 *   cboard lockstep ROMDIR CYCLES [PRG]   compare CPU pins every cycle and all
 *                                         1725 nodes after every cycle; with
 *                                         PRG: inject at READY and type RUN
 *                                         (as harness --prg does), else type
 *                                         PRINT 2+2 and a BASIC loop
 *   cboard speed ROMDIR CYCLES [PRG]      board B alone (and board A alone):
 *                                         whole-machine cycles/s */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "c64.h"
#include "ccpu.h"

int  cc64_init(c64_t *m, sid_model model);
void cc64_free(c64_t *m);
int  cc64_load_roms(c64_t *m, const char *dir, char *err, size_t errlen);
void cc64_tick(c64_t *m);
void cc64_type(c64_t *m, const char *text);
bool cc64_typing(const c64_t *m);
bool cc64_screen_contains(c64_t *m, const char *s);
void cc64_screen_text(c64_t *m, char out[25][41]);
ccpu_t *ccpu_shim_cpu(void);

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

typedef struct { int line; bool injected; } script_t;
static const char *const basic_script[] = { "PRINT 2+2\\n", "10 FORI=1TO30:POKE53280,I:NEXT\\n", "RUN\\n", NULL };

/* drive one board's autostart / typing; tick fn and screen fns differ per board */
static void script_step(c64_t *m, script_t *s, const uint8_t *prg, size_t prg_len, bool (*contains)(c64_t *, const char *),
                        bool (*typing)(const c64_t *), void (*type)(c64_t *, const char *)) {
    if (prg) {
        if (!s->injected && contains(m, "READY.")) {
            uint16_t a, e; char msg[256];
            if (c64_inject_prg(m, prg, prg_len, &a, &e, msg, sizeof msg)) { fprintf(stderr, "%s\n", msg); exit(1); }
            type(m, "RUN\\n");
            s->injected = true;
        }
        return;
    }
    if (basic_script[s->line] && !typing(m) && contains(m, "READY.") && m->vic.frame > (uint64_t)(120 + 60 * s->line)) {
        type(m, basic_script[s->line]);
        s->line++;
    }
}

static void show_screen(const char *title, char t[25][41]) {
    printf("%s:\n", title);
    for (int r = 0; r < 25; r++) {
        int blank = 1;
        for (int c = 0; c < 40; c++) if (t[r][c] != ' ') blank = 0;
        if (!blank) printf("  |%s|\n", t[r]);
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: cboard lockstep|speed ROMDIR CYCLES [PRG]\n"); return 2; }
    int speed = !strcmp(argv[1], "speed");
    const char *romdir = argv[2];
    uint64_t cycles = strtoull(argv[3], NULL, 0);
    size_t prg_len = 0;
    uint8_t *prg = argc > 4 ? read_file(argv[4], &prg_len) : NULL;
    if (argc > 4 && !prg) { fprintf(stderr, "cannot read %s\n", argv[4]); return 1; }
    static c64_t A, B;
    char err[2048];
    if (cc64_init(&B, SID_6581) || cc64_load_roms(&B, romdir, err, sizeof err)) { fprintf(stderr, "board B: %s\n", err); return 1; }
    script_t sb = { 0, false };
    if (speed) {
        double t0 = now();
        for (uint64_t n = 0; n < cycles; n++) {
            cc64_tick(&B);
            if ((n & 1023) == 0) script_step(&B, &sb, prg, prg_len, cc64_screen_contains, cc64_typing, cc64_type);
        }
        double dt = now() - t0;
        printf("board with compiled core: %llu cycles (%llu frames) in %.1f s = %.0f cycles/s (%.3fx real time)\n",
               (unsigned long long)cycles, (unsigned long long)B.vic.frame, dt, cycles / dt, cycles / dt / 985248.0);
        if (c64_init(&A, SID_6581) || c64_load_roms(&A, romdir, err, sizeof err)) { fprintf(stderr, "board A: %s\n", err); return 1; }
        script_t sa = { 0, false };
        uint64_t na = cycles / 4;
        t0 = now();
        for (uint64_t n = 0; n < na; n++) {
            c64_tick(&A);
            if ((n & 1023) == 0) script_step(&A, &sa, prg, prg_len, c64_screen_contains, c64_typing, c64_type);
        }
        dt = now() - t0;
        printf("board with netlist core (netsim): %llu cycles in %.1f s = %.0f cycles/s (%.3fx real time)\n",
               (unsigned long long)na, dt, na / dt, na / dt / 985248.0);
        char t[25][41];
        cc64_screen_text(&B, t);
        show_screen("compiled board screen", t);
        return 0;
    }
    if (c64_init(&A, SID_6581) || c64_load_roms(&A, romdir, err, sizeof err)) { fprintf(stderr, "board A: %s\n", err); return 1; }
    script_t sa = { 0, false };
    const ccpu_t *cc = ccpu_shim_cpu();
    const netcpu_t *nc = &A.cpu.cpu;
    const uint64_t mask = CPU_A_MASK | CPU_RW | CPU_SYNC | CPU_D_MASK;
    uint64_t n, stalled = 0, node_mism = 0, node_cmp = 0;
    int bad = 0;
    for (int k = 0; k < CC_NN; k++) if (netcpu_node(nc, k) != ccpu_node(cc, k)) node_mism++;
    double t0 = now();
    for (n = 0; n < cycles && !bad; n++) {
        c64_tick(&A);
        cc64_tick(&B);
        if (!(A.cpu_pins & CPU_RDY) && (A.cpu_pins & CPU_RW)) stalled++;
        if ((A.cpu_pins & mask) != (B.cpu_pins & mask)) {
            printf("*** PIN MISMATCH at board cycle %llu: netlist %04X %c %02X  compiled %04X %c %02X\n", (unsigned long long)n,
                   CPU_GET_ADDR(A.cpu_pins), (A.cpu_pins & CPU_RW) ? 'R' : 'W', CPU_GET_DATA(A.cpu_pins),
                   CPU_GET_ADDR(B.cpu_pins), (B.cpu_pins & CPU_RW) ? 'R' : 'W', CPU_GET_DATA(B.cpu_pins));
            bad = 1;
        }
        int d = 0;
        for (int k = 0; k < CC_NN; k++) d += netcpu_node(nc, k) != ccpu_node(cc, k);
        node_cmp += CC_NN;
        if (d) {
            node_mism += (uint64_t)d;
            if (node_mism < 50) printf("*** %d NODE MISMATCHES at board cycle %llu\n", d, (unsigned long long)n);
            bad = 1;
        }
        if ((n & 1023) == 0) {
            script_step(&A, &sa, prg, prg_len, c64_screen_contains, c64_typing, c64_type);
            script_step(&B, &sb, prg, prg_len, cc64_screen_contains, cc64_typing, cc64_type);
        }
    }
    double dt = now() - t0;
    char t[25][41], u[25][41];
    c64_screen_text(&A, t);
    cc64_screen_text(&B, u);
    show_screen("netlist board screen", t);
    printf("compiled board screen %s the netlist board's\n", memcmp(t, u, sizeof t) ? "DIFFERS from" : "is identical to");
    printf("%llu cycles (%llu frames), CPU stalled by BA on %llu read cycles, %llu IRQs / %llu NMIs taken, RAM %s\n",
           (unsigned long long)n, (unsigned long long)A.vic.frame, (unsigned long long)stalled,
           (unsigned long long)A.irqs_taken, (unsigned long long)A.nmis_taken,
           memcmp(A.ram, B.ram, sizeof A.ram) ? "DIFFERS" : "identical");
    printf("RESULT board cycles=%llu pin_mismatches=%d nodes_compared=%llu node_mismatches=%llu  (%.1f s)\n",
           (unsigned long long)n, bad && !node_mism ? 1 : 0, (unsigned long long)node_cmp, (unsigned long long)node_mism, dt);
    return bad;
}
