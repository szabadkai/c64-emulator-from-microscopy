/* lockstep.c - runs the netlist 6502 (harness/src/netcpu.c) and fastcpu on
 * the same simulated bus and compares them every cycle: address, R/W, SYNC,
 * data written, and A/X/Y/S/P at every opcode decode.
 *
 *   lockstep opcodes SEED N [FIRST LAST]   N random cases per opcode
 *   lockstep jam                           JAM opcodes, interrupts, RDY, reset out of JAM
 *   lockstep irq SEED [FIRST LAST]         IRQ/NMI swept over every cycle offset
 *   lockstep rdy SEED N [FIRST LAST]       RDY low at every cycle offset, several lengths
 *   lockstep alu adc|sbc C D                ADC/SBC for all A and M with carry C, decimal D
 *   lockstep reset                         power-on reset variants
 *   lockstep warmreset SEED N              /RES while running (approximate model: statistics only)
 *   lockstep random SEED CYCLES            random byte soup + random IRQ/NMI/RDY
 *   lockstep rom CYCLES ROMDIR             KERNAL/BASIC boot on flat RAM + I/O stubs
 *   lockstep bench CYCLES                  speed of fastcpu alone
 *
 * Every run ends with a line "RESULT <test> cycles=<compared> mismatches=<n>".
 * The netlist is the reference: a mismatch is reported with the last cycles
 * of both CPUs, then both CPUs are re-created and the run goes on. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "netcpu.h"
#include "fastcpu.h"

_Static_assert(CPU_RW == (1ULL << 24) && CPU_SYNC == (1ULL << 25) && CPU_RDY == (1ULL << 29),
               "pin layout");

/* ------------------------------------------------------------- utilities */
static uint64_t rng_state = 1;
static uint64_t rnd(void) {
    uint64_t x = rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static unsigned rn(unsigned n) { return (unsigned)(rnd() >> 33) % n; }
static uint8_t rb(void) { return (uint8_t)(rnd() >> 40); }
static void seed(uint64_t s) { rng_state = s * 0x9E3779B97F4A7C15ULL + 0x1234567; if (!rng_state) rng_state = 1; for (int i = 0; i < 4; i++) rnd(); }

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int is_jam(uint8_t op) { return (op & 0x1F) == 0x12 || op == 0x02 || op == 0x22 || op == 0x42 || op == 0x62; }

/* addressing modes, for building operands */
enum { IMP, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IZX, IZY, REL, IND, JAM };
static const char *const mode_str =
/*  0123456789ABCDEF */
   "IXJXZZZZIMIMAAAA"   /* 0 */
   "RYJYxxxxIbIbaaaa"   /* 1 */
   "AXJXZZZZIMIMAAAA"   /* 2 */
   "RYJYxxxxIbIbaaaa"   /* 3 */
   "IXJXZZZZIMIMAAAA"   /* 4 */
   "RYJYxxxxIbIbaaaa"   /* 5 */
   "IXJXZZZZIMIMNAAA"   /* 6 */
   "RYJYxxxxIbIbaaaa"   /* 7 */
   "MXMXZZZZIMIMAAAA"   /* 8 */
   "RYJYxxyyIbIbaabb"   /* 9 */
   "MXMXZZZZIMIMAAAA"   /* A */
   "RYJYxxyyIbIbaabb"   /* B */
   "MXMXZZZZIMIMAAAA"   /* C */
   "RYJYxxxxIbIbaaaa"   /* D */
   "MXMXZZZZIMIMAAAA"   /* E */
   "RYJYxxxxIbIbaaaa";  /* F */
static int op_mode(uint8_t op) {
    switch (mode_str[op]) {
    case 'I': return IMP; case 'M': return IMM; case 'Z': return ZP; case 'x': return ZPX;
    case 'y': return ZPY;
    case 'A': return ABS; case 'a': return ABX; case 'b': return ABY; case 'X': return IZX;
    case 'Y': return IZY; case 'R': return REL; case 'N': return IND; case 'J': return JAM;
    }
    return IMP;
}

/* --------------------------------------------------------------- the bus */
enum { INJ_NONE, INJ_WANT, INJ_SENT, INJ_ACCEPTED };

typedef struct {
    uint8_t mem[65536];
    int no_jam;                 /* opcode fetches of JAM bytes return NOP */
    /* JMP injection at an opcode fetch (bus override, memory untouched) */
    int inj;
    uint16_t inj_pc, inj_target;
    /* C64-like map for the ROM boot */
    const uint8_t *basic, *kernal;
    uint64_t cycle;
    int irq_line;               /* CIA1 timer IRQ (stub) */
    uint64_t next_timer;
    uint8_t vic_reg[64], cia_reg[2][16];
    int irq_acks;
} bus_t;

static uint8_t c64_io_read(bus_t *b, uint16_t a) {
    if (a >= 0xD000 && a < 0xD400) {
        int r = a & 0x3F;
        unsigned line = (unsigned)((b->cycle / 63) % 312);
        if (r == 0x12) return (uint8_t)line;
        if (r == 0x11) return (uint8_t)((b->vic_reg[0x11] & 0x7F) | ((line >> 1) & 0x80));
        if (r == 0x19) return 0x71;     /* raster IRQ latched: KERNAL decides PAL */
        return b->vic_reg[r];
    }
    if (a >= 0xD400 && a < 0xD800) return (a & 0x1F) == 0x1B ? (uint8_t)b->cycle : 0x00;
    if (a >= 0xD800 && a < 0xDC00) return b->mem[a] & 0x0F;
    if (a >= 0xDC00 && a < 0xDE00) {
        int n = (a >> 8) & 1, r = a & 0x0F;
        if (n == 0 && r == 0x01) return 0xFF;               /* keyboard rows: no key */
        if (n == 0 && r == 0x00) return 0xFF;
        if (n == 0 && r == 0x0D) {                           /* ICR: reading acks */
            uint8_t v = b->irq_line ? 0x81 : 0x00;
            if (b->irq_line) b->irq_acks++;
            b->irq_line = 0;
            return v;
        }
        if (r == 0x04) return (uint8_t)(b->cycle * 7);
        if (r == 0x05) return (uint8_t)(b->cycle >> 3);
        return b->cia_reg[n][r];
    }
    return 0xFF;
}

static uint8_t bus_read(bus_t *b, uint16_t a, int sync) {
    if (b->inj == INJ_SENT || b->inj == INJ_ACCEPTED) {
        if (a == b->inj_pc) return 0x4C;
        if (a == (uint16_t)(b->inj_pc + 1)) return (uint8_t)b->inj_target;
        if (a == (uint16_t)(b->inj_pc + 2)) return (uint8_t)(b->inj_target >> 8);
    }
    uint8_t v;
    if (b->kernal) {
        if (a >= 0xE000) v = b->kernal[a - 0xE000];
        else if (a >= 0xA000 && a < 0xC000) v = b->basic[a - 0xA000];
        else if (a >= 0xD000 && a < 0xE000) v = c64_io_read(b, a);
        else v = b->mem[a];
    } else {
        v = b->mem[a];
    }
    if (sync && b->no_jam && is_jam(v)) v = 0xEA;
    return v;
}

static void bus_write(bus_t *b, uint16_t a, uint8_t v) {
    if (b->kernal && a >= 0xD000 && a < 0xE000) {
        if (a < 0xD400) b->vic_reg[a & 0x3F] = v;
        else if (a >= 0xD800 && a < 0xDC00) b->mem[a] = v & 0x0F;
        else if (a >= 0xDC00 && a < 0xDE00) b->cia_reg[(a >> 8) & 1][a & 0x0F] = v;
        return;
    }
    b->mem[a] = v;
}

/* ------------------------------------------------------- lockstep engine */
#define HIST 40
typedef struct { uint64_t n, f, in; uint8_t nr[5], fr[5]; } hist_t;

typedef struct {
    netcpu_t net;
    fastcpu_t fast;
    bus_t *bus;
    uint64_t pins;          /* shared input pins (data of the last read) */
    int decode_next;        /* next tick decodes an opcode: compare registers */
    int failed;             /* mismatch in the current case */
    int tolerant;           /* count differences instead of failing (warm reset window) */
    int no_regs;            /* skip the register comparison */
    uint64_t diffs;
    uint64_t cycles, mismatches, reported;
    hist_t hist[HIST];
    unsigned hpos;
    char ctx[160];
    const char *test;
} ls_t;

static void regs_net(const netcpu_t *c, uint8_t r[5]) {
    r[0] = netcpu_a(c); r[1] = netcpu_x(c); r[2] = netcpu_y(c); r[3] = netcpu_s(c); r[4] = netcpu_p(c);
}
static void regs_fast(const fastcpu_t *c, uint8_t r[5]) {
    r[0] = fastcpu_a(c); r[1] = fastcpu_x(c); r[2] = fastcpu_y(c); r[3] = fastcpu_s(c); r[4] = fastcpu_p(c);
}

static void ls_power_on(ls_t *L) {
    netcpu_init(&L->net);
    fastcpu_init(&L->fast);
    /* undefined power-up contents: take the netlist's */
    fastcpu_regs_t r = { netcpu_pc(&L->net), netcpu_a(&L->net), netcpu_x(&L->net), netcpu_y(&L->net),
                         netcpu_s(&L->net), netcpu_p(&L->net) };
    fastcpu_set_regs(&L->fast, &r);
    L->pins = CPU_RW | CPU_RDY;
    L->decode_next = 0;
    L->hpos = 0;
    memset(L->hist, 0, sizeof L->hist);
}

static void ls_init(ls_t *L, bus_t *b, const char *test) {
    memset(L, 0, sizeof(*L));
    L->bus = b;
    L->test = test;
    ls_power_on(L);
}

static void pr_pins(char *s, uint64_t p) {
    sprintf(s, "%04X %c %02X %s", CPU_GET_ADDR(p), (p & CPU_RW) ? 'R' : 'W',
            CPU_GET_DATA(p), (p & CPU_SYNC) ? "S" : " ");
}

static void ls_report(ls_t *L, const char *why) {
    L->mismatches++;
    if (L->reported++ >= 12) return;
    printf("\n*** MISMATCH (%s) at compared cycle %llu: %s\n    case: %s\n", L->test,
           (unsigned long long)L->cycles, why, L->ctx);
    printf("    netlist IR=%02X PC=%04X | fast IR=%02X t=%d brk=%d intr=%d nmi_f=%d irq_s=%d\n",
           netcpu_ir(&L->net), netcpu_pc(&L->net), L->fast.ir, L->fast.t, L->fast.brk, L->fast.intr,
           L->fast.nmi_f, L->fast.irq_s);
    printf("      cyc  in(IRQ NMI RES RDY)  netlist            fast             regs net A X Y S P | fast\n");
    for (unsigned k = 0; k < HIST; k++) {
        const hist_t *h = &L->hist[(L->hpos + k) % HIST];
        if (!h->n && !h->f) continue;
        char a[40], b[40];
        pr_pins(a, h->n); pr_pins(b, h->f);
        int diff = (h->n & (CPU_A_MASK | CPU_RW | CPU_SYNC)) != (h->f & (CPU_A_MASK | CPU_RW | CPU_SYNC)) ||
                   (!(h->n & CPU_RW) && CPU_GET_DATA(h->n) != CPU_GET_DATA(h->f));
        printf("    %s %3d  %s %s %s %s      %s   %s   %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X\n",
               diff ? ">>" : "  ", (int)k - HIST + 1, (h->in & CPU_IRQ) ? "I" : ".", (h->in & CPU_NMI) ? "N" : ".",
               (h->in & CPU_RES) ? "R" : ".", (h->in & CPU_RDY) ? "." : "H", a, b,
               h->nr[0], h->nr[1], h->nr[2], h->nr[3], h->nr[4], h->fr[0], h->fr[1], h->fr[2], h->fr[3], h->fr[4]);
    }
    fflush(stdout);
}

/* recover after a mismatch: fresh CPUs, reset into whatever the vector says */
static void ls_recover(ls_t *L) {
    netcpu_free(&L->net);
    ls_power_on(L);
    L->bus->inj = INJ_NONE;
}

/* one cycle; ctl = IRQ/NMI/RES/RDY bits.  Returns 0, or -1 on mismatch
   (both CPUs have then been re-created). */
static uint64_t last_out;
static int ls_tick(ls_t *L, uint64_t ctl) {
    uint64_t in = (L->pins & ~(CPU_IRQ | CPU_NMI | CPU_RES | CPU_RDY)) | ctl;
    int decode = L->decode_next && !(ctl & CPU_RES);
    uint64_t no = netcpu_tick(&L->net, in);
    uint64_t fo = fastcpu_tick(&L->fast, in);
    L->cycles++;
    hist_t *h = &L->hist[L->hpos];
    L->hpos = (L->hpos + 1) % HIST;
    h->n = no; h->f = fo; h->in = in;
    regs_net(&L->net, h->nr);
    regs_fast(&L->fast, h->fr);
    const uint64_t m = CPU_A_MASK | CPU_RW | CPU_SYNC;
    if ((no & m) != (fo & m) || (!(no & CPU_RW) && CPU_GET_DATA(no) != CPU_GET_DATA(fo))) {
        if (L->tolerant) { L->diffs++; goto bus; }
        ls_report(L, "bus");
        L->failed = 1;
        ls_recover(L);
        return -1;
    }
    if (decode && !L->no_regs && memcmp(h->nr, h->fr, 5) != 0) {
        if (L->tolerant) { L->diffs++; goto bus; }
        ls_report(L, "registers at opcode decode");
        L->failed = 1;
        ls_recover(L);
        return -1;
    }
bus:;
    bus_t *b = L->bus;
    uint16_t a = CPU_GET_ADDR(no);
    int sync = (no & CPU_SYNC) != 0;
    /* injection bookkeeping (opcode fetch replaced by JMP target) */
    if (b->inj == INJ_SENT && !sync) {
        if (a == (uint16_t)(b->inj_pc + 1)) b->inj = INJ_ACCEPTED;
        else b->inj = INJ_WANT;            /* became an interrupt: try again */
    } else if (b->inj == INJ_SENT && sync && a != b->inj_pc) {
        b->inj = INJ_WANT;
    }
    if (sync && b->inj == INJ_ACCEPTED && a == b->inj_target) b->inj = INJ_NONE;
    if (sync && b->inj == INJ_WANT && (ctl & CPU_RDY)) { b->inj = INJ_SENT; b->inj_pc = a; }
    if (no & CPU_RW) L->pins = CPU_SET_DATA(no, bus_read(b, a, sync));
    else { bus_write(b, a, CPU_GET_DATA(no)); L->pins = no; }
    L->decode_next = sync && (ctl & CPU_RDY);
    b->cycle++;
    last_out = no;
    return 0;
}

/* ---------------------------------------------------------- scenarios */
typedef struct {
    uint16_t org;               /* first byte of the code under test */
    uint8_t code[24];
    int len;
    uint8_t a, x, y, s, p;
    struct { uint16_t a; uint8_t v; } poke[24];
    int npoke;
    int pad_nops;               /* fill +-0x90 around org with NOP first */
    /* waveforms, cycle offsets relative to the opcode fetch of code[0] */
    int irq_at, irq_len;        /* len < 0: until the CPU reads $FFFE */
    int nmi_at, nmi_len;
    int rdy_at, rdy_len;
    int end_syncs;              /* stop at this many SYNCs (incl. code[0]) ... */
    int max_cycles;             /* ... or at the first SYNC after this many cycles */
} scen_t;

#define SETUP_LEN 14            /* SEI LDX# TXS LDA# PHA LDA# LDX# LDY# PLP */
#define SETUP_CYCLES 21

static void scen_init(scen_t *sc) {
    memset(sc, 0, sizeof(*sc));
    sc->irq_at = sc->nmi_at = sc->rdy_at = -1000;
    sc->end_syncs = 2;
    sc->max_cycles = 1000;
}

static void scen_write(ls_t *L, const scen_t *sc) {
    uint8_t *m = L->bus->mem;
    if (sc->pad_nops)
        for (int i = -0x90; i < 0x90; i++) m[(uint16_t)(sc->org + i)] = 0xEA;
    uint16_t s0 = (uint16_t)(sc->org - SETUP_LEN);
    const uint8_t setup[SETUP_LEN] = { 0x78, 0xA2, sc->s, 0x9A, 0xA9, sc->p, 0x48, 0xA9, sc->a,
                                       0xA2, sc->x, 0xA0, sc->y, 0x28 };
    for (int i = 0; i < SETUP_LEN; i++) m[(uint16_t)(s0 + i)] = setup[i];
    for (int i = 0; i < sc->len; i++) m[(uint16_t)(sc->org + i)] = sc->code[i];
    for (int i = 0; i < sc->npoke; i++) m[sc->poke[i].a] = sc->poke[i].v;
}

static uint64_t idle_ctl(void) { return CPU_RDY; }

/* run one scenario; returns 0 or -1 (mismatch) */
static int run_scen(ls_t *L, const scen_t *sc) {
    bus_t *b = L->bus;
    uint16_t s0 = (uint16_t)(sc->org - SETUP_LEN);
    L->failed = 0;
    b->inj = INJ_WANT;
    b->inj_target = s0;
    int written = 0;
    /* phase 1: get the CPU to the setup code */
    for (int guard = 0;; guard++) {
        if (guard > 2000) { printf("    (case setup did not start: %s)\n", L->ctx); return 0; }
        if (b->inj == INJ_ACCEPTED && !written) { scen_write(L, sc); written = 1; }
        if (ls_tick(L, idle_ctl()) < 0) return -1;
        if ((last_out & CPU_SYNC) && CPU_GET_ADDR(last_out) == s0 && b->inj == INJ_NONE && written) break;
    }
    /* phase 2: waveforms relative to the opcode fetch of code[0] */
    int irq_acked = 0, syncs = 0;
    for (int r = -SETUP_CYCLES + 1;; r++) {
        uint64_t ctl = 0;
        int irq = r >= sc->irq_at && (sc->irq_len < 0 ? !irq_acked : r < sc->irq_at + sc->irq_len);
        int nmi = r >= sc->nmi_at && (sc->nmi_len < 0 || r < sc->nmi_at + sc->nmi_len);
        int rdy = !(r >= sc->rdy_at && r < sc->rdy_at + sc->rdy_len);
        if (irq) ctl |= CPU_IRQ;
        if (nmi) ctl |= CPU_NMI;
        if (rdy) ctl |= CPU_RDY;
        if (ls_tick(L, ctl) < 0) return -1;
        uint16_t a = CPU_GET_ADDR(last_out);
        if ((last_out & CPU_RW) && (a == 0xFFFE || a == 0xFFFF)) irq_acked = 1;
        if ((last_out & CPU_SYNC) && (ctl & CPU_RDY) && r >= 0) {
            syncs++;
            if (syncs >= sc->end_syncs || r >= sc->max_cycles) break;
        }
        if (r > sc->max_cycles + 200) break;   /* JAM or endless halt */
    }
    return 0;
}

/* random operands for opcode op placed at sc->org */
static void scen_random_op(scen_t *sc, uint8_t op) {
    int mode = op_mode(op);
    sc->code[0] = op;
    uint8_t o1 = rb(), o2 = rb();
    switch (mode) {
    case ZP: case ZPX: case ZPY: case IZX: case IZY:
        if (rn(6) == 0) o1 = 0xFF; else if (rn(8) == 0) o1 = 0x00;
        break;
    case ABS: case ABX: case ABY: case IND:
        if (rn(3) == 0) o1 = (uint8_t)(0xF0 | rn(16));
        if (mode == IND && rn(3) == 0) o1 = 0xFF;
        break;
    default: break;
    }
    sc->code[1] = o1; sc->code[2] = o2; sc->len = 3;
    if (mode == IZY || mode == IZX) {
        uint8_t zp = mode == IZX ? (uint8_t)(o1 + sc->x) : o1;
        uint8_t lo = rn(3) == 0 ? (uint8_t)(0xF0 | rn(16)) : rb();
        sc->poke[sc->npoke].a = zp; sc->poke[sc->npoke++].v = lo;
        sc->poke[sc->npoke].a = (uint8_t)(zp + 1); sc->poke[sc->npoke++].v = rb();
    }
    /* decimal mode: valid BCD operands half of the time */
    if ((sc->p & 0x08) && rn(2)) {
        sc->a = (uint8_t)((rn(10) << 4) | rn(10));
        if (mode == IMM) sc->code[1] = (uint8_t)((rn(10) << 4) | rn(10));
    }
}

static uint16_t random_org(void) {
    for (;;) {
        uint16_t org = (uint16_t)rnd();
        if (rn(4) == 0) org = (uint16_t)((org & 0xFF00) | (0xFD + rn(3)));
        /* keep the setup and the instruction out of the stack page and the vectors */
        uint16_t lo = (uint16_t)(org - SETUP_LEN), hi = (uint16_t)(org + 3);
        if (lo > org || hi < org) continue;          /* wraps around $0000 */
        if (hi >= 0xFFF8) continue;
        if (lo < 0x0200 && hi >= 0x0100) continue;
        return org;
    }
}

static void scen_random_regs(scen_t *sc) {
    sc->a = rb(); sc->x = rb(); sc->y = rb(); sc->s = rb(); sc->p = rb();
    if (rn(4) == 0) sc->x = (uint8_t)(0xF0 | rn(16));
    if (rn(4) == 0) sc->y = (uint8_t)(0xF0 | rn(16));
}

static const char *test_result(const char *name, ls_t *L, double t0) {
    printf("RESULT %s cycles=%llu mismatches=%llu  (%.1f s, %.0f lockstep cycles/s)\n", name,
           (unsigned long long)L->cycles, (unsigned long long)L->mismatches, now() - t0,
           L->cycles / (now() - t0));
    return "";
}

static void fill_random(bus_t *b, int valid_only) {
    for (int i = 0; i < 65536; i++) {
        uint8_t v = rb();
        if (valid_only) while (is_jam(v)) v = rb();
        b->mem[i] = v;
    }
}

static int parse_range(int argc, char **argv, int idx, int *first, int *last) {
    *first = 0; *last = 255;
    if (argc > idx) *first = (int)strtol(argv[idx], NULL, 0);
    if (argc > idx + 1) *last = (int)strtol(argv[idx + 1], NULL, 0);
    return 0;
}

/* ------------------------------------------------- 1: every opcode */
static int test_opcodes(uint64_t sd, int n, int first, int last) {
    static bus_t bus; ls_t L;
    seed(sd);
    memset(&bus, 0, sizeof bus);
    fill_random(&bus, 0);
    bus.no_jam = 1;
    ls_init(&L, &bus, "opcodes");
    double t0 = now();
    for (int op = first; op <= last; op++) {
        if (is_jam((uint8_t)op)) continue;
        for (int k = 0; k < n; k++) {
            scen_t sc; scen_init(&sc);
            scen_random_regs(&sc);
            sc.org = random_org();
            scen_random_op(&sc, (uint8_t)op);
            snprintf(L.ctx, sizeof L.ctx, "op %02X %02X %02X at %04X A=%02X X=%02X Y=%02X S=%02X P=%02X",
                     op, sc.code[1], sc.code[2], sc.org, sc.a, sc.x, sc.y, sc.s, sc.p);
            run_scen(&L, &sc);
        }
    }
    test_result("opcodes", &L, t0);
    netcpu_free(&L.net);
    return L.mismatches ? 1 : 0;
}

/* ------------------------------------------------- JAM */
static int test_jam(void) {
    static bus_t bus; ls_t L;
    seed(77);
    memset(&bus, 0, sizeof bus);
    fill_random(&bus, 0);
    bus.mem[0xFFFC] = 0x00; bus.mem[0xFFFD] = 0x02;
    double t0 = now();
    static const uint8_t jams[12] = { 0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xB2, 0xD2, 0xF2 };
    ls_init(&L, &bus, "jam");
    for (int j = 0; j < 12; j++) {
        for (int variant = 0; variant < 8; variant++) {
            snprintf(L.ctx, sizeof L.ctx, "JAM %02X variant %d", jams[j], variant);
            ls_recover(&L);   /* fresh power-on */
            bus.mem[0x0200] = 0xEA; bus.mem[0x0201] = 0x58; bus.mem[0x0202] = jams[j];
            int hold = 1 + variant;
            for (int cyc = 0; cyc < 120; cyc++) {
                uint64_t ctl = CPU_RDY;
                if (cyc < 2) ctl |= CPU_RES;
                if (variant & 1) { if (cyc >= 20 && cyc < 60) ctl |= CPU_IRQ; }
                if (variant & 2) { if (cyc >= 25 && cyc < 27) ctl |= CPU_NMI; }
                if ((variant & 4) && cyc >= 18 && cyc < 18 + 2 * variant) ctl &= ~CPU_RDY;
                /* reset out of the JAM */
                if (cyc >= 60 && cyc < 60 + hold * 3) ctl |= CPU_RES;
                if (ls_tick(&L, ctl) < 0) break;
            }
        }
    }
    test_result("jam", &L, t0);
    netcpu_free(&L.net);
    return L.mismatches ? 1 : 0;
}

/* ------------------------------------------------- 2: interrupt timing */
typedef struct { const char *name; uint8_t code[8]; int len; int x, y, p; int pk; } seq_t;

/* code is placed after two NOPs; the scenario pads with NOPs around */
static const seq_t seqs[] = {
    { "NOP",              { 0xEA }, 1, -1, -1, -1, 0 },
    { "LDA #",            { 0xA9, 0x55 }, 2, -1, -1, -1, 0 },
    { "LDA zp",           { 0xA5, 0x80 }, 2, -1, -1, -1, 0 },
    { "LDA abs",          { 0xAD, 0x34, 0x12 }, 3, -1, -1, -1, 0 },
    { "LDA abs,X",        { 0xBD, 0x10, 0x12 }, 3, 0x05, -1, -1, 0 },
    { "LDA abs,X cross",  { 0xBD, 0xF0, 0x12 }, 3, 0x35, -1, -1, 0 },
    { "STA abs,X",        { 0x9D, 0x10, 0x12 }, 3, 0x05, -1, -1, 0 },
    { "INC abs",          { 0xEE, 0x34, 0x12 }, 3, -1, -1, -1, 0 },
    { "INC abs,X",        { 0xFE, 0xF0, 0x12 }, 3, 0x35, -1, -1, 0 },
    { "INC zp",           { 0xE6, 0x80 }, 2, -1, -1, -1, 0 },
    { "LDA (zp),Y",       { 0xB1, 0x80 }, 2, -1, 0x05, -1, 1 },
    { "LDA (zp),Y cross", { 0xB1, 0x80 }, 2, -1, 0xF5, -1, 1 },
    { "STA (zp),Y",       { 0x91, 0x80 }, 2, -1, 0xF5, -1, 1 },
    { "LDA (zp,X)",       { 0xA1, 0x7E }, 2, 0x02, -1, -1, 1 },
    { "DCP (zp,X)",       { 0xC3, 0x7E }, 2, 0x02, -1, -1, 1 },
    { "JMP abs (next)",   { 0x4C, 0xFF, 0xFF }, 3, -1, -1, -1, 2 },
    { "JMP (ind)",        { 0x6C, 0x90, 0x00 }, 3, -1, -1, -1, 3 },
    { "JSR/RTS",          { 0x20, 0xFF, 0xFF }, 3, -1, -1, -1, 4 },
    { "RTS",              { 0x60 }, 1, -1, -1, -1, 5 },
    { "RTI I=0",          { 0x40 }, 1, -1, -1, 0x04, 6 },
    { "RTI I=1",          { 0x40 }, 1, -1, -1, 0x00, 7 },
    { "PHA",              { 0x48 }, 1, -1, -1, -1, 0 },
    { "PLA",              { 0x68 }, 1, -1, -1, -1, 0 },
    { "PHP",              { 0x08 }, 1, -1, -1, -1, 0 },
    { "PLP I=1->0",       { 0x28 }, 1, -1, -1, 0x04, 8 },
    { "PLP I=0->1",       { 0x28 }, 1, -1, -1, 0x00, 9 },
    { "PLP I=0->0",       { 0x28 }, 1, -1, -1, 0x00, 8 },
    { "CLI",              { 0x58 }, 1, -1, -1, 0x04, 0 },
    { "SEI",              { 0x78 }, 1, -1, -1, 0x00, 0 },
    { "CLI SEI",          { 0x58, 0x78 }, 2, -1, -1, 0x04, 0 },
    { "SEI CLI",          { 0x78, 0x58 }, 2, -1, -1, 0x00, 0 },
    { "BRK",              { 0x00, 0xEA }, 2, -1, -1, -1, 0 },
    { "BNE not taken",    { 0xD0, 0x10 }, 2, -1, -1, 0x02, 0 },
    { "BNE taken",        { 0xD0, 0x02 }, 2, -1, -1, 0x00, 0 },
    { "BNE taken back",   { 0xD0, 0xF0 }, 2, -1, -1, 0x00, 0 },
    { "BNE taken cross",  { 0xD0, 0x7F }, 2, -1, -1, 0x00, 0 },
    { "BNE taken cross back", { 0xD0, 0x80 }, 2, -1, -1, 0x00, 0 },
    { "BEQ taken to self-loop exit", { 0xF0, 0x00 }, 2, -1, -1, 0x02, 0 },
    { "ADC # decimal",    { 0x69, 0x19 }, 2, -1, -1, 0x08, 0 },
    { "TXS",              { 0x9A }, 1, -1, -1, -1, 0 },
};

static void scen_from_seq(scen_t *sc, const seq_t *q, uint16_t org) {
    scen_init(sc);
    sc->pad_nops = 1;
    sc->org = org;
    sc->a = rb(); sc->x = q->x >= 0 ? (uint8_t)q->x : rb(); sc->y = q->y >= 0 ? (uint8_t)q->y : rb();
    sc->s = (uint8_t)(0x80 + rn(0x70));
    sc->p = q->p >= 0 ? (uint8_t)q->p : (uint8_t)(rb() & ~0x04);
    sc->code[0] = 0xEA; sc->code[1] = 0xEA;
    memcpy(sc->code + 2, q->code, q->len);
    sc->len = 2 + q->len;
    for (int i = 0; i < 6; i++) sc->code[sc->len++] = 0xEA;
    uint16_t after = (uint16_t)(org + 2 + q->len);       /* first NOP after the sequence */
    #define POKE(addr, val) do { sc->poke[sc->npoke].a = (uint16_t)(addr); sc->poke[sc->npoke++].v = (uint8_t)(val); } while (0)
    switch (q->pk) {
    case 1:   /* pointer at $80/$81 */
        POKE(0x80, 0xF0); POKE(0x81, 0x12); break;
    case 2:   /* JMP to the next instruction */
        sc->code[3] = (uint8_t)after; sc->code[4] = (uint8_t)(after >> 8); break;
    case 3:   /* JMP ($0090) -> next */
        POKE(0x90, after); POKE(0x91, after >> 8); break;
    case 4: { /* JSR to an RTS 0x40 bytes ahead */
        uint16_t sub = (uint16_t)(org + 0x40);
        sc->code[3] = (uint8_t)sub; sc->code[4] = (uint8_t)(sub >> 8);
        POKE(sub, 0x60);
        break; }
    case 5:   /* RTS frame -> next */
        POKE(0x100 + (uint8_t)(sc->s + 1), (uint8_t)(after - 1));
        POKE(0x100 + (uint8_t)(sc->s + 2), (uint8_t)((after - 1) >> 8));
        break;
    case 6: case 7:   /* RTI frame: P (I clear / set), PCL, PCH */
        POKE(0x100 + (uint8_t)(sc->s + 1), q->pk == 6 ? 0x20 : 0x24);
        POKE(0x100 + (uint8_t)(sc->s + 2), (uint8_t)after);
        POKE(0x100 + (uint8_t)(sc->s + 3), (uint8_t)(after >> 8));
        break;
    case 8: POKE(0x100 + (uint8_t)(sc->s + 1), 0x20); break;   /* PLP pulls I=0 */
    case 9: POKE(0x100 + (uint8_t)(sc->s + 1), 0x24); break;   /* PLP pulls I=1 */
    }
    /* interrupt handlers: RTI */
    POKE(0xFFFA, 0x00); POKE(0xFFFB, 0xF8); POKE(0xFFFE, 0x00); POKE(0xFFFF, 0xF9);
    POKE(0xF800, 0x40); POKE(0xF900, 0xEA); POKE(0xF901, 0x40);
    #undef POKE
}

static int test_irq(uint64_t sd, int first, int last) {
    static bus_t bus; ls_t L;
    seed(sd);
    memset(&bus, 0, sizeof bus);
    fill_random(&bus, 0);
    bus.no_jam = 1;
    ls_init(&L, &bus, "irq");
    double t0 = now();
    const int nseq = (int)(sizeof seqs / sizeof seqs[0]);
    static const int irq_lens[] = { 1, 2, 3, -1 }, nmi_lens[] = { 1, 2, 40 };
    /* items: the fixed sequences, then every non-JAM opcode with random operands */
    for (int item = 0; item < nseq + 256; item++) {
        int opi = item - nseq;
        /* FIRST = -1: only the fixed sequences; otherwise only opcodes FIRST..LAST */
        if (opi >= 0 && (first < 0 || opi < first || opi > last || is_jam((uint8_t)opi))) continue;
        if (opi < 0 && first >= 0) continue;
        for (int w = 0; w < 7; w++) {
            for (int r = -6; r <= 14; r++) {
                scen_t sc;
                uint16_t org = (uint16_t)(0x0400 + rn(0xB000));
                if (rn(4) == 0) org = (uint16_t)((org & 0xFF00) | (0xF0 + rn(16)));
                if (opi < 0) {
                    const seq_t *q = &seqs[item];
                    /* branches that should cross: put them at a page end */
                    if (strstr(q->name, "cross")) org = (uint16_t)((org & 0xFF00) | (q->code[1] & 0x80 ? 0x10 : 0xF0));
                    scen_from_seq(&sc, q, org);
                    snprintf(L.ctx, sizeof L.ctx, "%s at %04X", q->name, org);
                } else {
                    seq_t q = { "op", { (uint8_t)opi }, 1, -1, -1, -1, 0 };
                    scen_from_seq(&sc, &q, org);
                    scen_t tmp; scen_init(&tmp); tmp.x = sc.x; tmp.p = sc.p;
                    scen_random_op(&tmp, (uint8_t)opi);
                    sc.code[2] = tmp.code[0]; sc.code[3] = tmp.code[1]; sc.code[4] = tmp.code[2];
                    sc.code[5] = 0xEA; sc.len = 11;
                    for (int i = 0; i < tmp.npoke; i++) sc.poke[sc.npoke++] = tmp.poke[i];
                    sc.p &= (uint8_t)~0x04;
                    snprintf(L.ctx, sizeof L.ctx, "op %02X %02X %02X at %04X", opi, tmp.code[1], tmp.code[2], org);
                }
                sc.end_syncs = 1000;
                sc.max_cycles = 40;
                size_t cl = strlen(L.ctx);
                if (w < 4) {
                    sc.irq_at = r; sc.irq_len = irq_lens[w];
                    snprintf(L.ctx + cl, sizeof L.ctx - cl, " IRQ at %d len %d", r, irq_lens[w]);
                } else {
                    sc.nmi_at = r; sc.nmi_len = nmi_lens[w - 4];
                    snprintf(L.ctx + cl, sizeof L.ctx - cl, " NMI at %d len %d", r, nmi_lens[w - 4]);
                }
                run_scen(&L, &sc);
            }
        }
        /* NMI swept across an IRQ sequence and a BRK */
        if (opi < 0 && (!strcmp(seqs[item].name, "NOP") || !strcmp(seqs[item].name, "BRK"))) {
            for (int r = 0; r <= 14; r++)
                for (int nl = 0; nl < 3; nl++) {
                    scen_t sc;
                    scen_from_seq(&sc, &seqs[item], (uint16_t)(0x0400 + rn(0xB000)));
                    sc.end_syncs = 1000; sc.max_cycles = 40;
                    if (!strcmp(seqs[item].name, "NOP")) { sc.irq_at = 0; sc.irq_len = -1; }
                    sc.nmi_at = r; sc.nmi_len = nmi_lens[nl];
                    snprintf(L.ctx, sizeof L.ctx, "%s + IRQ at 0, NMI at %d len %d", seqs[item].name, r, nmi_lens[nl]);
                    run_scen(&L, &sc);
                }
        }
    }
    test_result("irq", &L, t0);
    netcpu_free(&L.net);
    return L.mismatches ? 1 : 0;
}

/* ------------------------------------------------- 3: RDY */
static int test_rdy(uint64_t sd, int n, int first, int last) {
    static bus_t bus; ls_t L;
    seed(sd);
    memset(&bus, 0, sizeof bus);
    fill_random(&bus, 0);
    bus.no_jam = 1;
    ls_init(&L, &bus, "rdy");
    double t0 = now();
    static const int lens[] = { 1, 2, 3, 4, 7, 20 };
    for (int op = first; op <= last; op++) {
        if (is_jam((uint8_t)op)) continue;
        for (int k = 0; k < n; k++) {
            for (int li = 0; li < 6; li++)
                for (int r = -2; r <= 9; r++) {
                    scen_t sc; scen_init(&sc);
                    scen_random_regs(&sc);
                    sc.org = random_org();
                    scen_random_op(&sc, (uint8_t)op);
                    sc.rdy_at = r; sc.rdy_len = lens[li];
                    /* sometimes an IRQ or NMI in the same window */
                    int extra = rn(4);
                    if (extra == 1) { sc.irq_at = (int)rn(10) - 2; sc.irq_len = 1 + (int)rn(4); sc.p &= (uint8_t)~0x04; }
                    if (extra == 2) { sc.nmi_at = (int)rn(10) - 2; sc.nmi_len = 1 + (int)rn(3); }
                    sc.end_syncs = 2;
                    snprintf(L.ctx, sizeof L.ctx, "op %02X %02X %02X at %04X A=%02X X=%02X Y=%02X S=%02X P=%02X RDY low at %d len %d%s",
                             op, sc.code[1], sc.code[2], sc.org, sc.a, sc.x, sc.y, sc.s, sc.p, r, lens[li],
                             extra == 1 ? " +IRQ" : extra == 2 ? " +NMI" : "");
                    if (extra == 1) {
                        size_t cl = strlen(L.ctx);
                        snprintf(L.ctx + cl, sizeof L.ctx - cl, "(%d,%d)", sc.irq_at, sc.irq_len);
                    } else if (extra == 2) {
                        size_t cl = strlen(L.ctx);
                        snprintf(L.ctx + cl, sizeof L.ctx - cl, "(%d,%d)", sc.nmi_at, sc.nmi_len);
                    }
                    run_scen(&L, &sc);
                }
        }
    }
    test_result("rdy", &L, t0);
    netcpu_free(&L.net);
    return L.mismatches ? 1 : 0;
}

/* ------------------------------------------------- 4: reset */
static int test_reset(void) {
    static bus_t bus; ls_t L;
    seed(4);
    memset(&bus, 0, sizeof bus);
    fill_random(&bus, 0);
    bus.no_jam = 1;
    double t0 = now();
    ls_init(&L, &bus, "reset");
    /* RES held for h cycles from power-up, optional IRQ/NMI/RDY activity */
    for (int h = 0; h <= 12; h++)
        for (int v = 0; v < 9; v++) {
            ls_recover(&L);
            uint16_t vec = (uint16_t)(0x0400 + rn(0xA000));
            bus.mem[0xFFFC] = (uint8_t)vec; bus.mem[0xFFFD] = (uint8_t)(vec >> 8);
            snprintf(L.ctx, sizeof L.ctx, "power-on, /RES held %d cycles, variant %d", h, v);
            for (int cyc = 0; cyc < 60; cyc++) {
                uint64_t ctl = CPU_RDY;
                if (cyc < h) ctl |= CPU_RES;
                if (v == 1 && cyc >= 1 && cyc < 40) ctl |= CPU_IRQ;           /* masked by I after reset */
                if (v == 2 && cyc == h + 3) ctl |= CPU_NMI;                   /* NMI during the sequence */
                if (v == 3 && cyc >= h + 4 && cyc < h + 9) ctl &= ~CPU_RDY;  /* RDY low in the sequence */
                if (v == 4 && cyc >= h + 9 && cyc < h + 12) ctl &= ~CPU_RDY; /* ... on the first fetch */
                if (v == 5 && cyc == h + 10) ctl |= CPU_NMI;                  /* NMI right after */
                if (v == 6 && cyc >= h - 1 && cyc < h + 3) ctl &= ~CPU_RDY;   /* RDY low around release */
                if (v == 7 && cyc < h) ctl &= ~CPU_RDY;                       /* RDY low while held */
                if (v == 8 && cyc == h + 2) ctl &= ~CPU_RDY;                  /* ... on the reset SYNC */
                if (ls_tick(&L, ctl) < 0) break;
            }
        }
    test_result("reset", &L, t0);
    netcpu_free(&L.net);
    return L.mismatches ? 1 : 0;
}

/* ------------------------------------------------- 5: random programs */
static int test_random(uint64_t sd, uint64_t cycles) {
    static bus_t bus; ls_t L;
    seed(sd);
    memset(&bus, 0, sizeof bus);
    fill_random(&bus, 1);
    bus.no_jam = 1;
    ls_init(&L, &bus, "random");
    snprintf(L.ctx, sizeof L.ctx, "random soup seed %llu", (unsigned long long)sd);
    double t0 = now();
    int irq_left = 0, nmi_left = 0, rdy_left = 0;
    /* waveform mix changes every 20000 cycles */
    unsigned p_irq = 200, p_nmi = 800, p_rdy = 150;
    for (uint64_t cyc = 0; cyc < cycles; cyc++) {
        if (cyc % 20000 == 0) {
            p_irq = 20 + rn(600); p_nmi = 100 + rn(3000); p_rdy = 10 + rn(400);
            if (rn(4) == 0) p_rdy = 1u << 30;
            if (rn(4) == 0) p_irq = 1u << 30;
        }
        uint64_t ctl = 0;
        if (cyc < 3) ctl |= CPU_RES;
        if (irq_left > 0) { irq_left--; ctl |= CPU_IRQ; } else if (rn(p_irq) == 0) irq_left = 1 + (int)rn(rn(2) ? 4 : 60);
        if (nmi_left > 0) { nmi_left--; ctl |= CPU_NMI; } else if (rn(p_nmi) == 0) nmi_left = 1 + (int)rn(rn(2) ? 3 : 30);
        if (rdy_left > 0) rdy_left--; else { ctl |= CPU_RDY; if (rn(p_rdy) == 0) rdy_left = 1 + (int)rn(rn(2) ? 4 : 44); }
        /* now and then: jump somewhere else */
        if (bus.inj == INJ_NONE && rn(400) == 0) { bus.inj = INJ_WANT; bus.inj_target = (uint16_t)rnd(); }
        ls_tick(&L, ctl);
        if (L.failed) { L.failed = 0; }
    }
    test_result("random", &L, t0);
    netcpu_free(&L.net);
    return L.mismatches ? 1 : 0;
}

/* ------------------------------------------------- exhaustive ADC/SBC */
/* A 6502 loop runs OP (ADC/SBC zp) for every A and M with the given C and D
   flags; result and pushed P are written to memory, so every value and
   flag goes over the compared bus. */
static int test_alu(uint8_t op, int carry, int dec) {
    static bus_t bus; ls_t L;
    memset(&bus, 0, sizeof bus);
    static const uint8_t prog[] = {
        0xA9, 0x00, 0x85, 0x10,             /* 0400 LDA #0, STA $10 (M)          */
        0xA2, 0x00,                         /* 0404 LDX #0 (A value)             */
        0xA5, 0x11, 0x48, 0x28,             /* 0406 loop: LDA $11, PHA, PLP      */
        0x8A, 0x65, 0x10,                   /* 040A TXA, ADC $10 (patched)       */
        0x08, 0x9D, 0x00, 0x05,             /* 040D PHP, STA $0500,X             */
        0x68, 0x9D, 0x00, 0x06,             /* 0411 PLA, STA $0600,X             */
        0xE8, 0xD0, 0xEE,                   /* 0415 INX, BNE loop                */
        0xE6, 0x10, 0xD0, 0xEA,             /* 0418 INC $10, BNE loop            */
        0x4C, 0x1C, 0x04 };                 /* 041C JMP *                        */
    memcpy(bus.mem + 0x0400, prog, sizeof prog);
    bus.mem[0x040B] = op;
    bus.mem[0x11] = (uint8_t)(0x04 | (dec ? 0x08 : 0) | (carry ? 0x01 : 0));
    bus.mem[0xFFFC] = 0x00; bus.mem[0xFFFD] = 0x04;
    ls_init(&L, &bus, "alu");
    snprintf(L.ctx, sizeof L.ctx, "%s, all A and M, C=%d D=%d", op == 0x65 ? "ADC" : "SBC", carry, dec);
    double t0 = now();
    for (uint64_t cyc = 0; cyc < 4000000; cyc++) {
        if (ls_tick(&L, CPU_RDY) < 0) break;
        if ((last_out & CPU_SYNC) && CPU_GET_ADDR(last_out) == 0x041C) break;
    }
    int done = (last_out & CPU_SYNC) && CPU_GET_ADDR(last_out) == 0x041C;
    printf("%s: %s\n", L.ctx, done ? "all 65536 combinations compared" : "DID NOT FINISH");
    if (!done) L.mismatches++;
    test_result("alu", &L, t0);
    netcpu_free(&L.net);
    return L.mismatches ? 1 : 0;
}

/* ------------------------------------------------- warm reset (measurement) */
/* /RES asserted while a random program runs, for 1..12 or 20..420 cycles.
   fastcpu models this only approximately (see README), so this is a
   measurement, not a pass/fail test.  The reset vector points to a handler
   that re-initialises the registers like the KERNAL does (LDX #$FF, TXS,
   LDA #0, PHA, PLP, LDY #0, LDX #0) and then jumps back into the soup.
   Counted per event: the vector fetch happens in the same cycle on both
   CPUs; the bus is identical throughout; the bus is identical from the
   vector fetch on (200 cycles checked). */
static int test_warmreset(uint64_t sd, int events) {
    static bus_t bus; ls_t L;
    seed(sd);
    memset(&bus, 0, sizeof bus);
    fill_random(&bus, 1);
    bus.no_jam = 1;
    ls_init(&L, &bus, "warmreset");
    snprintf(L.ctx, sizeof L.ctx, "warm reset seed %llu", (unsigned long long)sd);
    double t0 = now();
    int vec_aligned = 0, exact = 0, resynced = 0, diverged = 0;
    for (int ev = 0; ev < events; ev++) {
        uint16_t back = (uint16_t)rnd();
        const uint8_t handler[] = { 0xA2, 0xFF, 0x9A, 0xA9, 0x00, 0x48, 0x28, 0xA0, 0x00, 0xA2, 0x00,
                                    0x4C, (uint8_t)back, (uint8_t)(back >> 8) };
        memcpy(bus.mem + 0x0300, handler, sizeof handler);
        bus.mem[0xFFFC] = 0x00; bus.mem[0xFFFD] = 0x03;
        int run = 20 + (int)rn(300);
        for (int i = 0; i < run; i++) ls_tick(&L, CPU_RDY);
        int len = rn(3) ? 1 + (int)rn(12) : 20 + (int)rn(400);
        L.tolerant = 1; L.diffs = 0;
        int nvec = -1, fvec = -1;
        for (int i = 0; i < len + 60 && (nvec < 0 || fvec < 0); i++) {
            ls_tick(&L, CPU_RDY | (i < len ? CPU_RES : 0));
            const hist_t *h = &L.hist[(L.hpos + HIST - 1) % HIST];
            if (nvec < 0 && CPU_GET_ADDR(h->n) == 0xFFFC && (h->n & CPU_RW)) nvec = i;
            if (fvec < 0 && CPU_GET_ADDR(h->f) == 0xFFFC && (h->f & CPU_RW)) fvec = i;
        }
        /* the handler (22 cycles) re-initialises the registers: bus only */
        uint64_t window_diffs = L.diffs;
        L.diffs = 0;
        L.no_regs = 1;
        for (int i = 0; i < 22; i++) ls_tick(&L, CPU_RDY);
        L.no_regs = 0;
        for (int i = 0; i < 200; i++) ls_tick(&L, CPU_RDY);
        L.tolerant = 0;
        if (nvec >= 0 && nvec == fvec) vec_aligned++;
        if (!L.diffs && !window_diffs) exact++;
        else if (!L.diffs) resynced++;
        else { diverged++; ls_recover(&L); }
    }
    printf("warm resets: %d events; reset vector fetched in the same cycle: %d; bus identical throughout: %d;"
           " identical from the vector fetch on: %d; diverged: %d\n", events, vec_aligned, exact, exact + resynced, diverged);
    printf("RESULT warmreset cycles=%llu events=%d vector_aligned=%d exact=%d resynced=%d diverged=%d  (%.1f s)\n",
           (unsigned long long)L.cycles, events, vec_aligned, exact, resynced, diverged, now() - t0);
    netcpu_free(&L.net);
    return 0;
}

/* ------------------------------------------------- ROM boot */
static int load(const char *dir, const char *name, uint8_t *dst, size_t n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(dst, 1, n, f);
    fclose(f);
    return r == n ? 0 : -1;
}

static void type_into_buffer(bus_t *b, const char *s) {
    /* KERNAL keyboard buffer: $0277.., count in $C6 (max 10) */
    int n = 0;
    while (s[n] && n < 10) { b->mem[0x0277 + n] = (uint8_t)(s[n] == '\n' ? 13 : s[n]); n++; }
    b->mem[0xC6] = (uint8_t)n;
}

static int test_rom(uint64_t cycles, const char *dir) {
    static bus_t bus; ls_t L;
    static uint8_t basic[8192], kernal[8192];
    if (load(dir, "kernal.901227-03.bin", kernal, 8192) || load(dir, "basic.901226-01.bin", basic, 8192)) {
        printf("ROMs not found in %s\n", dir);
        return 1;
    }
    memset(&bus, 0, sizeof bus);
    bus.basic = basic; bus.kernal = kernal;
    for (int i = 0; i < 65536; i++) bus.mem[i] = (i & 0x40) ? 0xFF : 0x00;
    bus.next_timer = 17045;
    ls_init(&L, &bus, "rom");
    double t0 = now();
    static const char *const lines[] = {
        "?2+2\n", "A=7\n", "?A*A\n", "10 FORI=1", "TO20\n", "20 B=SIN(", "I)*I+B\n", "30 NEXT\n",
        "40 ?B;I\n", "RUN\n", "?FRE(0)\n", NULL };
    int line = 0;
    uint64_t next_type = 2600000, irqs = 0;
    for (uint64_t cyc = 0; cyc < cycles; cyc++) {
        uint64_t ctl = CPU_RDY;
        if (cyc < 8) ctl |= CPU_RES;
        /* CIA1 timer A stub: underflow every 16421 cycles once started */
        if ((bus.cia_reg[0][0x0E] & 1) && cyc >= bus.next_timer) {
            bus.irq_line = 1; irqs++;
            bus.next_timer = cyc + 16421;
        }
        if (bus.irq_line) ctl |= CPU_IRQ;
        /* typing: once READY and the buffer is empty */
        if (cyc >= next_type && lines[line] && bus.mem[0xC6] == 0) {
            type_into_buffer(&bus, lines[line]);
            line++;
            next_type = cyc + 300000;
        }
        snprintf(L.ctx, sizeof L.ctx, "ROM boot, cycle %llu", (unsigned long long)cyc);
        if (ls_tick(&L, ctl) < 0) { printf("ROM run aborted after mismatch\n"); break; }
    }
    /* screen */
    printf("screen after %llu cycles (%llu timer IRQs, %d acknowledged):\n", (unsigned long long)L.cycles,
           (unsigned long long)irqs, bus.irq_acks);
    for (int r = 0; r < 25; r++) {
        char t[41];
        for (int c = 0; c < 40; c++) {
            uint8_t ch = bus.mem[0x0400 + r * 40 + c] & 0x7F;
            t[c] = ch == 0 ? '@' : ch < 27 ? (char)('A' + ch - 1) : (ch >= 32 && ch < 64) ? (char)ch : '#';
        }
        t[40] = 0;
        int blank = 1;
        for (int c = 0; c < 40; c++) if (t[c] != ' ') blank = 0;
        if (!blank) printf("  |%s|\n", t);
    }
    test_result("rom", &L, t0);
    netcpu_free(&L.net);
    return L.mismatches ? 1 : 0;
}

/* ------------------------------------------------- speed */
static int bench(uint64_t cycles) {
    static uint8_t mem[65536];
    /* a busy loop with a mix of addressing modes; the board work is minimal */
    static const uint8_t prog[] = {
        0xA2, 0x00, 0xA0, 0x10,                   /* LDX #0, LDY #$10 */
        0xBD, 0x00, 0x30, 0x7D, 0x00, 0x31,       /* L: LDA $3000,X  ADC $3100,X */
        0x9D, 0x00, 0x32, 0xFE, 0x00, 0x33,       /* STA $3200,X  INC $3300,X */
        0xB1, 0x80, 0x91, 0x82, 0x48, 0x68,       /* LDA ($80),Y  STA ($82),Y  PHA PLA */
        0x20, 0x40, 0x10, 0xE8, 0xD0, 0xE8,       /* JSR $1040  INX  BNE L */
        0x88, 0xD0, 0xE5, 0x4C, 0x00, 0x10 };     /* DEY  BNE L  JMP $1000 */
    memcpy(mem + 0x1000, prog, sizeof prog);
    mem[0x1040] = 0x0A; mem[0x1041] = 0x2A; mem[0x1042] = 0x60;   /* ASL ROL RTS */
    mem[0x80] = 0x00; mem[0x81] = 0x34; mem[0x82] = 0x00; mem[0x83] = 0x35;
    mem[0xFFFC] = 0x00; mem[0xFFFD] = 0x10;
    fastcpu_t c;
    fastcpu_init(&c);
    uint64_t pins = CPU_RW | CPU_RDY | CPU_RES;
    for (int i = 0; i < 4; i++) {
        pins = fastcpu_tick(&c, pins);
        pins = CPU_SET_DATA(pins, mem[CPU_GET_ADDR(pins)]);
    }
    pins &= ~CPU_RES;
    double best = 0;
    for (int rep = 0; rep < 5; rep++) {
        double t0 = now();
        for (uint64_t i = 0; i < cycles; i++) {
            pins = fastcpu_tick(&c, pins);
            uint16_t a = CPU_GET_ADDR(pins);
            if (pins & CPU_RW) pins = CPU_SET_DATA(pins, mem[a]);
            else mem[a] = CPU_GET_DATA(pins);
        }
        double dt = now() - t0, rate = cycles / dt;
        if (rate > best) best = rate;
        printf("fastcpu alone: %llu cycles in %.3f s = %.1f M cycles/s (%.0fx real time C64)\n",
               (unsigned long long)cycles, dt, rate / 1e6, rate / 985248.0);
    }
    /* random opcode soup (JAM opcodes fetched as NOP): poor branch prediction */
    seed(99);
    for (int i = 0; i < 65536; i++) mem[i] = rb();
    double best_soup = 0;
    for (int rep = 0; rep < 3; rep++) {
        double t0 = now();
        for (uint64_t i = 0; i < cycles; i++) {
            pins = fastcpu_tick(&c, pins | CPU_RDY);
            uint16_t a = CPU_GET_ADDR(pins);
            if (pins & CPU_RW) {
                uint8_t v = mem[a];
                if ((pins & CPU_SYNC) && is_jam(v)) v = 0xEA;
                pins = CPU_SET_DATA(pins, v);
            } else mem[a] = CPU_GET_DATA(pins);
        }
        double rate = cycles / (now() - t0);
        if (rate > best_soup) best_soup = rate;
        printf("fastcpu alone, random opcode soup: %.1f M cycles/s\n", rate / 1e6);
    }
    printf("RESULT bench loop=%.1f soup=%.1f Mcycles/s (PC=%04X)\n", best / 1e6, best_soup / 1e6, fastcpu_pc(&c));
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: lockstep opcodes|jam|irq|rdy|alu|reset|warmreset|random|rom|bench ...\n");
        return 2;
    }
    const char *t = argv[1];
    int first, last;
    if (!strcmp(t, "opcodes")) {
        parse_range(argc, argv, 4, &first, &last);
        return test_opcodes(argc > 2 ? strtoull(argv[2], NULL, 0) : 1, argc > 3 ? atoi(argv[3]) : 50, first, last);
    }
    if (!strcmp(t, "jam")) return test_jam();
    if (!strcmp(t, "irq")) {
        parse_range(argc, argv, 3, &first, &last);
        return test_irq(argc > 2 ? strtoull(argv[2], NULL, 0) : 1, first, last);
    }
    if (!strcmp(t, "rdy")) {
        parse_range(argc, argv, 4, &first, &last);
        return test_rdy(argc > 2 ? strtoull(argv[2], NULL, 0) : 1, argc > 3 ? atoi(argv[3]) : 1, first, last);
    }
    if (!strcmp(t, "reset")) return test_reset();
    if (!strcmp(t, "alu") && argc > 4)
        return test_alu(!strcmp(argv[2], "sbc") ? 0xE5 : 0x65, atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(t, "warmreset"))
        return test_warmreset(argc > 2 ? strtoull(argv[2], NULL, 0) : 1, argc > 3 ? atoi(argv[3]) : 300);
    if (!strcmp(t, "random"))
        return test_random(argc > 2 ? strtoull(argv[2], NULL, 0) : 1, argc > 3 ? strtoull(argv[3], NULL, 0) : 1000000);
    if (!strcmp(t, "rom"))
        return test_rom(argc > 2 ? strtoull(argv[2], NULL, 0) : 5000000, argc > 3 ? argv[3] : "../harness/roms");
    if (!strcmp(t, "bench")) return bench(argc > 2 ? strtoull(argv[2], NULL, 0) : 200000000ULL);
    fprintf(stderr, "unknown test %s\n", t);
    return 2;
}
