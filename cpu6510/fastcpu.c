/* fastcpu.c - fast behavioural NMOS 6502 core, bus-cycle-identical to the
 * visual6502 netlist CPU of this project (see fastcpu.h, README.md).
 *
 * Model.  One tick = one bus cycle.  A tick first finishes the previous
 * cycle (consumes its read data) and then sets up the next one, so every
 * instruction is a list of steps: step t runs in the tick that puts out bus
 * cycle C(t+2) and sees the data of C(t+1) (C1 = opcode fetch).  The last
 * step of every instruction puts out the next opcode fetch (SYNC); the tick
 * after a SYNC cycle decodes the opcode or, if an interrupt was polled,
 * forces BRK (ir = 0) without incrementing PC.
 *
 * Interrupt timing (all measured on the netlist):
 *  - At the end of every tick: irq_s = /IRQ asserted, nmi_f |= /NMI
 *    asserted now && not asserted last tick.
 *  - POLL: the step that puts out the LAST cycle of an instruction does
 *    intr |= (irq_s && I clear) | nmi_f, i.e. it sees the lines as they
 *    were in the penultimate cycle and I as it is now.
 *    Branches poll in their operand-fetch step; a taken branch polls again
 *    only if it crosses a page (so a taken branch without page cross does
 *    not see an interrupt that arrives in its last two cycles).  Polls OR
 *    into intr, which is cleared at every opcode decode.  BRK and the
 *    interrupt sequence do not poll, except on RDY repeats of their last
 *    cycle.
 *  - Interrupt sequence: the NMI vector is chosen if nmi_f is set when the
 *    P push is put out (step 3: NMI hijacks BRK/IRQ until then); nmi_f is
 *    cleared when the vector high byte is put out (step 5).  An NMI edge in
 *    the P-push or vector-low cycle of an IRQ/BRK sequence is therefore lost,
 *    unless /NMI is still low in the vector-high cycle (then it is taken
 *    after one handler instruction).
 *  - RDY low halts a read cycle: the next tick repeats it; IRQ/NMI sampling
 *    goes on while halted, and a halted polling cycle polls again on each
 *    repeat (with the I change of CLI/SEI already applied).
 * Reset, RDY and JAM details: see slow_tick().
 *
 * Where the visual6502 netlist differs from real NMOS silicon, this core
 * follows the netlist (ANC/ALR/ARR/LAS, XAA/LAX# "magic" = $00); see
 * FASTCPU_SILICON_IMM below and README.md.
 */
#include "fastcpu.h"
#include <string.h>

enum { FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08, FB = 0x10, FU = 0x20, FV = 0x40, FN = 0x80 };
enum { BRK_SW = 0, BRK_HW = 1, BRK_RES = 2 };

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ------------------------------------------------------------------ ALU */
#define NZ(v) (c->p = (uint8_t)((c->p & ~(FN | FZ)) | ((v) & FN) | ((v) ? 0 : FZ)))

static inline void op_adc(fastcpu_t *c, uint8_t v) {
    unsigned a = c->a, ci = c->p & FC;
    uint8_t p = c->p & (uint8_t)~(FN | FV | FZ | FC);
    if (c->p & FD) {
        /* NMOS decimal mode: Z from the binary sum, N/V from the
           half-adjusted high nibble, C from the fully adjusted one */
        unsigned lo = (a & 0x0F) + (v & 0x0F) + ci;
        if (lo > 9) lo += 6;
        unsigned hi = (a >> 4) + (v >> 4) + (lo > 0x0F);
        if (((a + v + ci) & 0xFF) == 0) p |= FZ;
        if (hi & 0x08) p |= FN;
        if (~(a ^ v) & (a ^ (hi << 4)) & 0x80) p |= FV;
        if (hi > 9) hi += 6;
        if (hi > 0x0F) p |= FC;
        c->a = (uint8_t)((hi << 4) | (lo & 0x0F));
    } else {
        unsigned sum = a + v + ci;
        if (sum > 0xFF) p |= FC;
        if (~(a ^ v) & (a ^ sum) & 0x80) p |= FV;
        c->a = (uint8_t)sum;
        p |= c->a & FN;
        if (!c->a) p |= FZ;
    }
    c->p = p;
}

static inline void op_sbc(fastcpu_t *c, uint8_t v) {
    unsigned a = c->a, borrow = (c->p & FC) ? 0 : 1;
    unsigned diff = a - v - borrow;
    uint8_t p = c->p & (uint8_t)~(FN | FV | FZ | FC);
    if (diff < 0x100) p |= FC;
    if ((a ^ v) & (a ^ diff) & 0x80) p |= FV;
    if (!(diff & 0xFF)) p |= FZ;
    p |= diff & FN;
    if (c->p & FD) {   /* NMOS: flags from the binary result */
        int lo = (int)(a & 0x0F) - (int)(v & 0x0F) - (int)borrow;
        int hi = (int)(a >> 4) - (int)(v >> 4);
        if (lo & 0x10) { lo -= 6; hi--; }
        if (hi & 0x10) hi -= 6;
        c->a = (uint8_t)((hi << 4) | (lo & 0x0F));
    } else {
        c->a = (uint8_t)diff;
    }
    c->p = p;
}

static inline void op_cmp(fastcpu_t *c, uint8_t r, uint8_t v) {
    unsigned t = (unsigned)r - v;
    c->p = (uint8_t)((c->p & ~(FN | FZ | FC)) | (t & FN) | ((t & 0xFF) ? 0 : FZ) | (r >= v ? FC : 0));
}

static inline uint8_t op_asl(fastcpu_t *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~FC) | (v >> 7));
    v = (uint8_t)(v << 1); NZ(v); return v;
}
static inline uint8_t op_lsr(fastcpu_t *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~FC) | (v & 1));
    v >>= 1; NZ(v); return v;
}
static inline uint8_t op_rol(fastcpu_t *c, uint8_t v) {
    uint8_t ci = c->p & FC;
    c->p = (uint8_t)((c->p & ~FC) | (v >> 7));
    v = (uint8_t)((v << 1) | ci); NZ(v); return v;
}
static inline uint8_t op_ror(fastcpu_t *c, uint8_t v) {
    uint8_t ci = c->p & FC;
    c->p = (uint8_t)((c->p & ~FC) | (v & 1));
    v = (uint8_t)((v >> 1) | (ci << 7)); NZ(v); return v;
}
static inline uint8_t op_inc(fastcpu_t *c, uint8_t v) { v++; NZ(v); return v; }
static inline uint8_t op_dec(fastcpu_t *c, uint8_t v) { v--; NZ(v); return v; }
/* undocumented read-modify-write combinations */
static inline uint8_t op_slo(fastcpu_t *c, uint8_t v) { v = op_asl(c, v); c->a |= v; NZ(c->a); return v; }
static inline uint8_t op_rla(fastcpu_t *c, uint8_t v) { v = op_rol(c, v); c->a &= v; NZ(c->a); return v; }
static inline uint8_t op_sre(fastcpu_t *c, uint8_t v) { v = op_lsr(c, v); c->a ^= v; NZ(c->a); return v; }
static inline uint8_t op_rra(fastcpu_t *c, uint8_t v) { v = op_ror(c, v); op_adc(c, v); return v; }
static inline uint8_t op_dcp(fastcpu_t *c, uint8_t v) { v--; op_cmp(c, c->a, v); return v; }
static inline uint8_t op_isc(fastcpu_t *c, uint8_t v) { v++; op_sbc(c, v); return v; }

/* ANC / ALR / ARR.  On real NMOS silicon the immediate is ANDed into A
 * first.  The visual6502 netlist (this project's reference) does not do
 * that: ANC leaves A alone (N/Z/C from A), ALR shifts A (C from bit 0 of
 * A & imm), ARR rotates A.  Measured on 610 random cases, then lockstep.
 * Build with -DFASTCPU_SILICON_IMM for the silicon behaviour instead. */
#ifdef FASTCPU_SILICON_IMM
#define IMM_AND(v) (c->a & (v))
#else
#define IMM_AND(v) ((void)(v), c->a)
#endif
static inline void op_arr(fastcpu_t *c, uint8_t v) {
    uint8_t t = IMM_AND(v), ci = c->p & FC;
    uint8_t r = (uint8_t)((t >> 1) | (ci << 7));
    uint8_t p = c->p & (uint8_t)~(FN | FV | FZ | FC);
    if (c->p & FD) {
        p |= r & FN;
        if (!r) p |= FZ;
        if ((t ^ r) & 0x40) p |= FV;
        if ((t & 0x0F) + (t & 0x01) > 5) r = (uint8_t)((r & 0xF0) | ((r + 6) & 0x0F));
        if ((t & 0xF0) + (t & 0x10) > 0x50) { r = (uint8_t)(r + 0x60); p |= FC; }
    } else {
        p |= r & FN;
        if (!r) p |= FZ;
        if (r & 0x40) p |= FC;
        if (((r >> 6) ^ (r >> 5)) & 1) p |= FV;
    }
    c->a = r;
    c->p = p;
}
static inline void op_anc(fastcpu_t *c, uint8_t v) {
    c->a = IMM_AND(v);
    NZ(c->a);
    c->p = (uint8_t)((c->p & ~FC) | (c->a >> 7));
}
static inline void op_alr(fastcpu_t *c, uint8_t v) {
    uint8_t cy = (uint8_t)(c->a & v & 1);
    c->a = (uint8_t)(IMM_AND(v) >> 1);
    NZ(c->a);
    c->p = (uint8_t)((c->p & ~FC) | cy);
}

/* LAS: silicon gives A = X = S = M & S.  The netlist leaves S alone, copies
   S to X and puts S & (M | $11) into A; N/Z come from M & S (measured). */
static inline void op_las(fastcpu_t *c, uint8_t v) {
    uint8_t r = c->s & v;
#ifdef FASTCPU_SILICON_IMM
    c->a = c->x = c->s = r;
#else
    c->a = (uint8_t)(c->s & (v | 0x11));
    c->x = c->s;
#endif
    NZ(r);
}

/* read operations: X(v) */
#define LDA(v) do { c->a = (v); NZ(c->a); } while (0)
#define LDX(v) do { c->x = (v); NZ(c->x); } while (0)
#define LDY(v) do { c->y = (v); NZ(c->y); } while (0)
#define LAX(v) do { c->a = c->x = (v); NZ(c->a); } while (0)
#define ORA(v) do { c->a |= (v); NZ(c->a); } while (0)
#define AND(v) do { c->a &= (v); NZ(c->a); } while (0)
#define EOR(v) do { c->a ^= (v); NZ(c->a); } while (0)
#define ADC(v) op_adc(c, (v))
#define SBC(v) op_sbc(c, (v))
#define CMP(v) op_cmp(c, c->a, (v))
#define CPX(v) op_cmp(c, c->x, (v))
#define CPY(v) op_cmp(c, c->y, (v))
#define BIT(v) do { uint8_t v_ = (v); \
    c->p = (uint8_t)((c->p & ~(FN | FV | FZ)) | (v_ & (FN | FV)) | ((c->a & v_) ? 0 : FZ)); } while (0)
#define NOP(v) do { (void)(v); } while (0)
#define LAS(v) op_las(c, (v))
#define ANC(v) op_anc(c, (v))
#define ALR(v) op_alr(c, (v))
#define ARR(v) op_arr(c, (v))
#define SBX(v) do { uint8_t ax_ = c->a & c->x, v_ = (v); \
    c->p = (uint8_t)((c->p & ~FC) | (ax_ >= v_ ? FC : 0)); c->x = (uint8_t)(ax_ - v_); NZ(c->x); } while (0)
/* unstable on real chips (analog "magic constant"); the netlist resolves the
   bus fight as a wired-AND, i.e. magic = $00 */
#define XAA(v) do { c->a = (uint8_t)(c->a & c->x & (v)); NZ(c->a); } while (0)
#define LXA(v) do { c->a = c->x = (uint8_t)(c->a & (v)); NZ(c->a); } while (0)
/* read-modify-write operations: X(v) -> new value */
#define ASL(v) op_asl(c, (v))
#define LSR(v) op_lsr(c, (v))
#define ROL(v) op_rol(c, (v))
#define ROR(v) op_ror(c, (v))
#define INC(v) op_inc(c, (v))
#define DEC(v) op_dec(c, (v))
#define SLO(v) op_slo(c, (v))
#define RLA(v) op_rla(c, (v))
#define SRE(v) op_sre(c, (v))
#define RRA(v) op_rra(c, (v))
#define DCP(v) op_dcp(c, (v))
#define ISC(v) op_isc(c, (v))

/* ------------------------------------------------------------ bus steps */
#define ST(op, t) (((op) << 3) | (t))
#define RD(adr) do { ab = (uint16_t)(adr); } while (0)
#define WR(adr, v) do { ab = (uint16_t)(adr); rw = 0; db = (uint8_t)(v); } while (0)
#define FETCH() do { ab = c->pc; sync = 1; } while (0)
#define POLL() (c->intr |= (c->irq_s & !(c->p & FI)) | c->nmi_f, c->pcyc = 1)
#define SKIP() (c->t++)   /* skip the next step (no page crossing) */

/* --- addressing modes; R = read op, W = store value, M = read-modify-write */
#define M_IMP(op, CODE) \
    case ST(op, 0): RD(c->pc); POLL(); break; \
    case ST(op, 1): CODE; FETCH(); break;
#define M_IMM(op, R) \
    case ST(op, 0): RD(c->pc++); POLL(); break; \
    case ST(op, 1): R(d); FETCH(); break;

#define M_ZP_R(op, R) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->ad); POLL(); break; \
    case ST(op, 2): R(d); FETCH(); break;
#define M_ZP_W(op, W) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; WR(c->ad, W); POLL(); break; \
    case ST(op, 2): FETCH(); break;
#define M_ZP_M(op, M) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->ad); break; \
    case ST(op, 2): c->dl = d; WR(c->ad, c->dl); break; \
    case ST(op, 3): c->dl = M(c->dl); WR(c->ad, c->dl); POLL(); break; \
    case ST(op, 4): FETCH(); break;

#define M_ZPI_R(op, IDX, R) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->ad); break; \
    case ST(op, 2): c->ad = (uint8_t)(c->ad + IDX); RD(c->ad); POLL(); break; \
    case ST(op, 3): R(d); FETCH(); break;
#define M_ZPI_W(op, IDX, W) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->ad); break; \
    case ST(op, 2): c->ad = (uint8_t)(c->ad + IDX); WR(c->ad, W); POLL(); break; \
    case ST(op, 3): FETCH(); break;
#define M_ZPI_M(op, IDX, M) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->ad); break; \
    case ST(op, 2): c->ad = (uint8_t)(c->ad + IDX); RD(c->ad); break; \
    case ST(op, 3): c->dl = d; WR(c->ad, c->dl); break; \
    case ST(op, 4): c->dl = M(c->dl); WR(c->ad, c->dl); POLL(); break; \
    case ST(op, 5): FETCH(); break;

#define M_ABS_R(op, R) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->pc++); break; \
    case ST(op, 2): c->ad |= (uint16_t)(d << 8); RD(c->ad); POLL(); break; \
    case ST(op, 3): R(d); FETCH(); break;
#define M_ABS_W(op, W) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->pc++); break; \
    case ST(op, 2): c->ad |= (uint16_t)(d << 8); WR(c->ad, W); POLL(); break; \
    case ST(op, 3): FETCH(); break;
#define M_ABS_M(op, M) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->pc++); break; \
    case ST(op, 2): c->ad |= (uint16_t)(d << 8); RD(c->ad); break; \
    case ST(op, 3): c->dl = d; WR(c->ad, c->dl); break; \
    case ST(op, 4): c->dl = M(c->dl); WR(c->ad, c->dl); POLL(); break; \
    case ST(op, 5): FETCH(); break;

/* absolute indexed: the first access goes to (base high, low + index) */
#define ABI_CROSSED() ((c->ad >> 8) != c->hb)
#define ABI_ADDR(IDX) \
    c->hb = d; c->ad = (uint16_t)(((d << 8) | c->ad) + IDX); \
    RD((d << 8) | (c->ad & 0xFF)); c->fix = ABI_CROSSED()
#define M_ABI_R(op, IDX, R) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->pc++); break; \
    case ST(op, 2): ABI_ADDR(IDX); if (!ABI_CROSSED()) { POLL(); SKIP(); } break; \
    case ST(op, 3): RD(c->ad); POLL(); break; \
    case ST(op, 4): R(d); FETCH(); break;
#define M_ABI_W(op, IDX, W) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->pc++); break; \
    case ST(op, 2): ABI_ADDR(IDX); break; \
    case ST(op, 3): WR(c->ad, W); POLL(); break; \
    case ST(op, 4): FETCH(); break;
#define M_ABI_M(op, IDX, M) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->pc++); break; \
    case ST(op, 2): ABI_ADDR(IDX); break; \
    case ST(op, 3): RD(c->ad); break; \
    case ST(op, 4): c->dl = d; WR(c->ad, c->dl); break; \
    case ST(op, 5): c->dl = M(c->dl); WR(c->ad, c->dl); POLL(); break; \
    case ST(op, 6): FETCH(); break;
/* SHA/SHX/SHY/TAS: value = reg & (base high + 1); on a page crossing the
   value also replaces the high byte of the address.  If RDY halted the dummy
   read before the write, the value is the plain register (the address is
   unchanged) - as on real chips and on the netlist. */
#define SH_WRITE(V) do { uint8_t r_ = (uint8_t)(V), v_ = r_ & (uint8_t)(c->hb + 1); \
    uint16_t a_ = ABI_CROSSED() ? (uint16_t)((v_ << 8) | (c->ad & 0xFF)) : c->ad; \
    if (c->sh_halt) { v_ = r_; c->sh_halt = 0; }   /* RDY: no "& (H+1)" */ \
    WR(a_, v_); } while (0)
#define M_ABI_SH(op, IDX, PRE, V) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->pc++); break; \
    case ST(op, 2): ABI_ADDR(IDX); break; \
    case ST(op, 3): PRE; SH_WRITE(V); POLL(); break; \
    case ST(op, 4): FETCH(); break;

#define M_IZX_PRE(op) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->ad); break; \
    case ST(op, 2): c->ad = (uint8_t)(c->ad + c->x); RD(c->ad); break; \
    case ST(op, 3): c->dl = d; RD((uint8_t)(c->ad + 1)); break;
#define M_IZX_R(op, R) M_IZX_PRE(op) \
    case ST(op, 4): c->ad = (uint16_t)((d << 8) | c->dl); RD(c->ad); POLL(); break; \
    case ST(op, 5): R(d); FETCH(); break;
#define M_IZX_W(op, W) M_IZX_PRE(op) \
    case ST(op, 4): c->ad = (uint16_t)((d << 8) | c->dl); WR(c->ad, W); POLL(); break; \
    case ST(op, 5): FETCH(); break;
#define M_IZX_M(op, M) M_IZX_PRE(op) \
    case ST(op, 4): c->ad = (uint16_t)((d << 8) | c->dl); RD(c->ad); break; \
    case ST(op, 5): c->dl = d; WR(c->ad, c->dl); break; \
    case ST(op, 6): c->dl = M(c->dl); WR(c->ad, c->dl); POLL(); break; \
    case ST(op, 7): FETCH(); break;

#define IZY_ADDR() \
    c->hb = d; c->ad = (uint16_t)(((d << 8) | c->dl) + c->y); \
    RD((d << 8) | (c->ad & 0xFF)); c->fix = ABI_CROSSED()
#define M_IZY_PRE(op) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->ad = d; RD(c->ad); break; \
    case ST(op, 2): c->dl = d; RD((uint8_t)(c->ad + 1)); break;
#define M_IZY_R(op, R) M_IZY_PRE(op) \
    case ST(op, 3): IZY_ADDR(); if (!ABI_CROSSED()) { POLL(); SKIP(); } break; \
    case ST(op, 4): RD(c->ad); POLL(); break; \
    case ST(op, 5): R(d); FETCH(); break;
#define M_IZY_W(op, W) M_IZY_PRE(op) \
    case ST(op, 3): IZY_ADDR(); break; \
    case ST(op, 4): WR(c->ad, W); POLL(); break; \
    case ST(op, 5): FETCH(); break;
#define M_IZY_M(op, M) M_IZY_PRE(op) \
    case ST(op, 3): IZY_ADDR(); break; \
    case ST(op, 4): RD(c->ad); break; \
    case ST(op, 5): c->dl = d; WR(c->ad, c->dl); break; \
    case ST(op, 6): c->dl = M(c->dl); WR(c->ad, c->dl); POLL(); break; \
    case ST(op, 7): FETCH(); break;

#define M_BRA(op, COND) \
    case ST(op, 0): RD(c->pc++); POLL(); break; \
    case ST(op, 1): if (!(COND)) { FETCH(); break; } \
        c->ad = (uint16_t)(c->pc + (int8_t)d); RD(c->pc); break; \
    case ST(op, 2): if (!((c->ad ^ c->pc) & 0xFF00)) { c->pc = c->ad; FETCH(); break; } \
        RD((c->pc & 0xFF00) | (c->ad & 0xFF)); POLL(); \
        c->fix = (uint8_t)((c->ad >> 8) - (c->pc >> 8)) == 1; /* forward crossing */ break; \
    case ST(op, 3): c->pc = c->ad; FETCH(); break;

#define M_JAM(op) \
    case ST(op, 0): RD(c->pc++); break; \
    case ST(op, 1): c->jam = 1; RD(0xFFFF); break; \
    case ST(op, 2): RD(0xFFFE); break; \
    case ST(op, 3): RD(0xFFFE); break; \
    case ST(op, 4): RD(0xFFFF); c->t--; break;

#define SP (0x0100 | c->s)

/* --------------------------------------------------------------- public */
void fastcpu_init(fastcpu_t *c) {
    memset(c, 0, sizeof(*c));
    /* what the netlist settles to after netcpu_init() */
    c->pc = 0x00FF; c->a = 0xFF; c->x = 0x44; c->y = 0x00; c->s = 0xC4; c->p = 0xA0;
    c->res = 1;          /* /RES held since power-up */
    c->hold = 0x00FF | (uint32_t)CPU_RW;
}

fastcpu_regs_t fastcpu_regs(const fastcpu_t *c) {
    fastcpu_regs_t r = { c->pc, c->a, c->x, c->y, c->s, (uint8_t)((c->p & 0xCF) | 0x20) };
    return r;
}
void fastcpu_set_regs(fastcpu_t *c, const fastcpu_regs_t *r) {
    c->pc = r->pc; c->a = r->a; c->x = r->x; c->y = r->y; c->s = r->s; c->p = r->p;
    if (c->res) c->hold = c->pc | (uint32_t)CPU_RW;
}
uint16_t fastcpu_pc(const fastcpu_t *c) { return c->pc; }
uint8_t  fastcpu_a(const fastcpu_t *c) { return c->a; }
uint8_t  fastcpu_x(const fastcpu_t *c) { return c->x; }
uint8_t  fastcpu_y(const fastcpu_t *c) { return c->y; }
uint8_t  fastcpu_s(const fastcpu_t *c) { return c->s; }
uint8_t  fastcpu_p(const fastcpu_t *c) { return (uint8_t)((c->p & 0xCF) | 0x20); }
uint8_t  fastcpu_ir(const fastcpu_t *c) { return c->ir; }
bool     fastcpu_jammed(const fastcpu_t *c) { return c->jam != 0; }

/* interrupt inputs are sampled at the end of every cycle (also while
   halted by RDY or in reset) */
static inline void sample_inputs(fastcpu_t *c, uint64_t pins) {
    c->irq_s = (pins & CPU_IRQ) != 0;
    uint8_t nmi = (pins & CPU_NMI) != 0;
    if (UNLIKELY(c->nmi_chk)) { if (!nmi) c->nmi_f = 0; c->nmi_chk = 0; }
    c->nmi_f |= (uint8_t)(nmi & !c->nmi_prev);
    c->nmi_prev = nmi;
}

/* address of a repeated (RDY) page-crossing dummy read: the corrected one,
   except for SHA/SHX/SHY/TAS, whose high byte becomes the stored value */
static uint16_t sh_fix_addr(const fastcpu_t *c) {
    uint8_t v;
    switch (c->ir) {
    case 0x93: case 0x9B: case 0x9F: v = c->a & c->x; break;   /* SHA, TAS */
    case 0x9C: v = c->y; break;
    case 0x9E: v = c->x; break;
    default: return c->ad;
    }
    v &= (uint8_t)(c->hb + 1);
    return (uint16_t)((v << 8) | (c->ad & 0xFF));
}

/* Rare paths: /RES activity and RDY repeats.  Returns 1 if it produced the
 * cycle, 0 if a normal step follows.
 *
 * /RES (measured on the netlist):
 *  - The reset sequence always starts with an opcode fetch (SYNC) in the
 *    third cycle after /RES goes high (rel == 3), or later if RDY halted
 *    the cycle before, at the address then on the bus; that fetch is turned
 *    into the BRK-like reset sequence.  Hold cycles are reads: RDY halts
 *    and repeats them like any other read.
 *  - res == 1, power-up hold: every cycle reads (data << 8) | (previous
 *    high byte - 1), data being the byte read in the previous cycle.
 *  - Warm reset (res_age counts cycles since /RES went low): the next cycle
 *    is unaffected, from the one after that writes become reads; when the
 *    instruction ends the bus holds at PC (res == 2).  In a JAM the bus
 *    switches to PC three cycles after /RES went low.  This part is an
 *    approximation of the netlist (see README); power-up and JAM are exact.
 *
 * RDY: a read cycle with RDY low is repeated.  If the repeated cycle is the
 * one in which the instruction polls for interrupts, the poll is evaluated
 * again on every repeat (CLI/SEI: with the new I).  A halted dummy read of a
 * page-crossing indexed access or branch is repeated at the corrected
 * address (the high-byte carry happens during the halt). */
static int slow_tick(fastcpu_t *c, uint64_t pins, uint64_t *out) {
    const int res_now = (pins & CPU_RES) != 0;
    if (res_now | c->res | c->res_age) {
        const int was_halted = c->halted;
        if (res_now) {
            c->rel = 0;
            if (!c->res) c->res_age = c->res_age ? (uint8_t)(c->res_age + (c->res_age < 200)) : 1;
        } else {
            if (c->rel < 200) c->rel++;
            if (c->res_age) c->res_age = (uint8_t)(c->res_age + (c->res_age < 200));
        }
        if (!c->res && c->jam && c->res_age >= 4) c->res = 2;
        /* the reset SYNC: earliest in the third cycle after /RES went high,
           and only after a cycle that RDY did not halt */
        const int go = c->rel >= 3 && !was_halted;
        if (c->res || go) {
            uint16_t prev = (uint16_t)c->hold, addr;
            /* JAM $B2 sits in the LDX column: once /RES restarts the timing
               it loads X (and N/Z) from the data bus on every cycle */
            if (c->jam && c->ir == 0xB2) { c->x = CPU_GET_DATA(pins); NZ(c->x); }
            if (was_halted) addr = prev;                       /* RDY repeat */
            else if (c->res == 1) addr = (uint16_t)((CPU_GET_DATA(pins) << 8) | (uint8_t)((prev >> 8) - 1));
            else addr = c->pc;
            uint32_t bus = addr | (uint32_t)CPU_RW;
            if (go) {
                bus |= (uint32_t)CPU_SYNC;
                c->pc = addr;
                c->res = 0; c->rel = 0; c->res_age = 0;
                c->res_pend = 1;
                c->jam = 0;
                c->sync = 1;
                c->intr = 0;
                c->pcyc = 0; c->fix = 0;
            }
            c->hold = bus;
            c->halted = (pins & CPU_RDY) == 0;   /* hold cycles are reads */
            sample_inputs(c, pins);
            c->cycles++;
            *out = (pins & ~(CPU_A_MASK | CPU_RW | CPU_SYNC)) | bus;
            return 1;
        }
    }
    if (c->halted) {
        c->halted = (pins & CPU_RDY) == 0;
        /* the netlist already performs the I update that follows a halted
           C2 of CLI/SEI (PLP loads only N/V early; its I comes after) ... */
        if (!c->sync && c->t == 1) {
            if (c->ir == 0x58) c->p &= ~FI;
            else if (c->ir == 0x78) c->p |= FI;
        }
        /* ... and evaluates the interrupt poll again */
        if (c->pcyc) c->intr |= (c->irq_s & !(c->p & FI)) | c->nmi_f;
        /* a halted page-crossing dummy read is repeated with the carry
           already added to the high byte (forward crossings only) */
        if (c->fix) c->hold = (c->hold & ~0xFFFFu) | sh_fix_addr(c);
        if ((c->t == 3 && (c->ir == 0x9B || c->ir == 0x9C || c->ir == 0x9E || c->ir == 0x9F)) ||
            (c->t == 4 && c->ir == 0x93))
            c->sh_halt = 1;
        sample_inputs(c, pins);
        c->cycles++;
        *out = (pins & ~(CPU_A_MASK | CPU_RW | CPU_SYNC)) | c->hold;
        return 1;
    }
    return 0;
}

uint64_t fastcpu_tick(fastcpu_t *c, uint64_t pins) {
    if (UNLIKELY(c->halted | c->res | c->res_age | ((pins & CPU_RES) != 0))) {
        uint64_t out;
        if (slow_tick(c, pins, &out)) return out;
    }

    const uint8_t d = CPU_GET_DATA(pins);
    uint16_t ab = 0;
    int rw = 1, sync = 0;
    uint8_t db = 0;

    if (c->sync) {   /* the previous cycle fetched an opcode */
        c->t = 0;
        if (UNLIKELY(c->intr | c->res_pend)) {
            c->ir = 0x00;
            c->brk = c->res_pend ? BRK_RES : BRK_HW;
            c->res_pend = 0;
        } else {
            c->ir = d;
            c->brk = BRK_SW;
            c->pc++;
        }
        c->intr = 0;
        c->sh_halt = 0;
    }
    c->pcyc = 0;
    c->fix = 0;

    switch (((unsigned)c->ir << 3) | c->t) {
    /* ---- BRK / IRQ / NMI / RESET */
    case ST(0x00, 0): if (c->brk == BRK_SW) RD(c->pc++); else RD(c->pc); break;
    case ST(0x00, 1):
        if (c->brk == BRK_RES) RD(SP); else WR(SP, c->pc >> 8);
        c->s--; break;
    case ST(0x00, 2):
        if (c->brk == BRK_RES) RD(SP); else WR(SP, c->pc);
        c->s--; break;
    case ST(0x00, 3):
        if (c->brk == BRK_RES) RD(SP);
        else WR(SP, c->brk == BRK_SW ? (c->p | FB | FU) : ((c->p | FU) & ~FB));
        c->s--;
        c->ad = c->brk == BRK_RES ? 0xFFFC : c->nmi_f ? 0xFFFA : 0xFFFE;
        break;
    case ST(0x00, 4): RD(c->ad); break;
    case ST(0x00, 5):
        /* vector high byte.  NMI vector: the NMI is consumed.  IRQ/BRK
           vector: an NMI that came too late to hijack (C5/C6) survives
           only if /NMI is still low in this cycle (checked at the end of
           the tick).  This cycle does not poll, but when RDY stretches
           it the repeats do (and can start the NMI right away). */
        c->dl = d; RD(c->ad + 1); c->p |= FI;
        if (c->ad == 0xFFFA) c->nmi_f = 0; else c->nmi_chk = 1;
        c->pcyc = 1;
        break;
    case ST(0x00, 6): c->pc = (uint16_t)((d << 8) | c->dl); FETCH(); break;

    /* ---- control flow and stack */
    case ST(0x20, 0): RD(c->pc++); break;                                  /* JSR */
    case ST(0x20, 1): c->ad = d; RD(SP); break;
    case ST(0x20, 2): WR(SP, c->pc >> 8); c->s--; break;
    case ST(0x20, 3): WR(SP, c->pc); c->s--; break;
    case ST(0x20, 4): RD(c->pc); POLL(); break;
    case ST(0x20, 5): c->pc = (uint16_t)((d << 8) | c->ad); FETCH(); break;

    case ST(0x60, 0): RD(c->pc); break;                                    /* RTS */
    case ST(0x60, 1): RD(SP); break;
    case ST(0x60, 2): c->s++; RD(SP); break;
    case ST(0x60, 3): c->ad = d; c->s++; RD(SP); break;
    case ST(0x60, 4): c->pc = (uint16_t)((d << 8) | c->ad); RD(c->pc++); POLL(); break;
    case ST(0x60, 5): FETCH(); break;

    case ST(0x40, 0): RD(c->pc); break;                                    /* RTI */
    case ST(0x40, 1): RD(SP); break;
    case ST(0x40, 2): c->s++; RD(SP); break;
    case ST(0x40, 3): c->p = d; c->s++; RD(SP); break;
    case ST(0x40, 4): c->ad = d; c->s++; RD(SP); POLL(); break;
    case ST(0x40, 5): c->pc = (uint16_t)((d << 8) | c->ad); FETCH(); break;

    case ST(0x4C, 0): RD(c->pc++); break;                                  /* JMP abs */
    case ST(0x4C, 1): c->ad = d; RD(c->pc); POLL(); break;
    case ST(0x4C, 2): c->pc = (uint16_t)((d << 8) | c->ad); FETCH(); break;

    case ST(0x6C, 0): RD(c->pc++); break;                                  /* JMP (ind) */
    case ST(0x6C, 1): c->ad = d; RD(c->pc++); break;
    case ST(0x6C, 2): c->ad |= (uint16_t)(d << 8); RD(c->ad); break;
    case ST(0x6C, 3): c->dl = d; RD((c->ad & 0xFF00) | ((c->ad + 1) & 0xFF)); POLL(); break;
    case ST(0x6C, 4): c->pc = (uint16_t)((d << 8) | c->dl); FETCH(); break;

    case ST(0x48, 0): RD(c->pc); break;                                    /* PHA */
    case ST(0x48, 1): WR(SP, c->a); c->s--; POLL(); break;
    case ST(0x48, 2): FETCH(); break;
    case ST(0x08, 0): RD(c->pc); break;                                    /* PHP */
    case ST(0x08, 1): WR(SP, c->p | FB | FU); c->s--; POLL(); break;
    case ST(0x08, 2): FETCH(); break;
    case ST(0x68, 0): RD(c->pc); break;                                    /* PLA */
    case ST(0x68, 1): RD(SP); break;
    case ST(0x68, 2): c->s++; RD(SP); POLL(); break;
    case ST(0x68, 3): LDA(d); FETCH(); break;
    case ST(0x28, 0): RD(c->pc); break;                                    /* PLP */
    case ST(0x28, 1): RD(SP); break;
    case ST(0x28, 2): c->s++; RD(SP); POLL(); break;
    case ST(0x28, 3): c->p = d; FETCH(); break;

    M_BRA(0x10, !(c->p & FN))   M_BRA(0x30, c->p & FN)
    M_BRA(0x50, !(c->p & FV))   M_BRA(0x70, c->p & FV)
    M_BRA(0x90, !(c->p & FC))   M_BRA(0xB0, c->p & FC)
    M_BRA(0xD0, !(c->p & FZ))   M_BRA(0xF0, c->p & FZ)

    /* ---- implied / accumulator */
    M_IMP(0x18, c->p &= ~FC)              M_IMP(0x38, c->p |= FC)
    M_IMP(0x58, c->p &= ~FI)              M_IMP(0x78, c->p |= FI)
    M_IMP(0xB8, c->p &= ~FV)              M_IMP(0xD8, c->p &= ~FD)
    M_IMP(0xF8, c->p |= FD)
    M_IMP(0xAA, LDX(c->a))                M_IMP(0xA8, LDY(c->a))
    M_IMP(0x8A, LDA(c->x))                M_IMP(0x98, LDA(c->y))
    M_IMP(0xBA, LDX(c->s))                M_IMP(0x9A, c->s = c->x)
    M_IMP(0xE8, LDX((uint8_t)(c->x + 1))) M_IMP(0xC8, LDY((uint8_t)(c->y + 1)))
    M_IMP(0xCA, LDX((uint8_t)(c->x - 1))) M_IMP(0x88, LDY((uint8_t)(c->y - 1)))
    M_IMP(0x0A, c->a = ASL(c->a))         M_IMP(0x4A, c->a = LSR(c->a))
    M_IMP(0x2A, c->a = ROL(c->a))         M_IMP(0x6A, c->a = ROR(c->a))
    M_IMP(0xEA, (void)0)
    M_IMP(0x1A, (void)0) M_IMP(0x3A, (void)0) M_IMP(0x5A, (void)0)
    M_IMP(0x7A, (void)0) M_IMP(0xDA, (void)0) M_IMP(0xFA, (void)0)

    /* ---- immediate */
    M_IMM(0x09, ORA) M_IMM(0x29, AND) M_IMM(0x49, EOR) M_IMM(0x69, ADC)
    M_IMM(0xA9, LDA) M_IMM(0xC9, CMP) M_IMM(0xE9, SBC) M_IMM(0xEB, SBC)
    M_IMM(0xA0, LDY) M_IMM(0xA2, LDX) M_IMM(0xC0, CPY) M_IMM(0xE0, CPX)
    M_IMM(0x0B, ANC) M_IMM(0x2B, ANC) M_IMM(0x4B, ALR) M_IMM(0x6B, ARR)
    M_IMM(0xCB, SBX) M_IMM(0x8B, XAA) M_IMM(0xAB, LXA)
    M_IMM(0x80, NOP) M_IMM(0x82, NOP) M_IMM(0x89, NOP) M_IMM(0xC2, NOP) M_IMM(0xE2, NOP)

    /* ---- zero page */
    M_ZP_R(0x05, ORA) M_ZP_R(0x25, AND) M_ZP_R(0x45, EOR) M_ZP_R(0x65, ADC)
    M_ZP_R(0xA5, LDA) M_ZP_R(0xC5, CMP) M_ZP_R(0xE5, SBC) M_ZP_R(0x24, BIT)
    M_ZP_R(0xA4, LDY) M_ZP_R(0xA6, LDX) M_ZP_R(0xC4, CPY) M_ZP_R(0xE4, CPX)
    M_ZP_R(0xA7, LAX)
    M_ZP_R(0x04, NOP) M_ZP_R(0x44, NOP) M_ZP_R(0x64, NOP)
    M_ZP_W(0x85, c->a) M_ZP_W(0x86, c->x) M_ZP_W(0x84, c->y) M_ZP_W(0x87, c->a & c->x)
    M_ZP_M(0x06, ASL) M_ZP_M(0x46, LSR) M_ZP_M(0x26, ROL) M_ZP_M(0x66, ROR)
    M_ZP_M(0xE6, INC) M_ZP_M(0xC6, DEC)
    M_ZP_M(0x07, SLO) M_ZP_M(0x27, RLA) M_ZP_M(0x47, SRE) M_ZP_M(0x67, RRA)
    M_ZP_M(0xC7, DCP) M_ZP_M(0xE7, ISC)

    /* ---- zero page indexed */
    M_ZPI_R(0x15, c->x, ORA) M_ZPI_R(0x35, c->x, AND) M_ZPI_R(0x55, c->x, EOR)
    M_ZPI_R(0x75, c->x, ADC) M_ZPI_R(0xB5, c->x, LDA) M_ZPI_R(0xD5, c->x, CMP)
    M_ZPI_R(0xF5, c->x, SBC) M_ZPI_R(0xB4, c->x, LDY) M_ZPI_R(0xB6, c->y, LDX)
    M_ZPI_R(0xB7, c->y, LAX)
    M_ZPI_R(0x14, c->x, NOP) M_ZPI_R(0x34, c->x, NOP) M_ZPI_R(0x54, c->x, NOP)
    M_ZPI_R(0x74, c->x, NOP) M_ZPI_R(0xD4, c->x, NOP) M_ZPI_R(0xF4, c->x, NOP)
    M_ZPI_W(0x95, c->x, c->a) M_ZPI_W(0x94, c->x, c->y) M_ZPI_W(0x96, c->y, c->x)
    M_ZPI_W(0x97, c->y, c->a & c->x)
    M_ZPI_M(0x16, c->x, ASL) M_ZPI_M(0x56, c->x, LSR) M_ZPI_M(0x36, c->x, ROL)
    M_ZPI_M(0x76, c->x, ROR) M_ZPI_M(0xF6, c->x, INC) M_ZPI_M(0xD6, c->x, DEC)
    M_ZPI_M(0x17, c->x, SLO) M_ZPI_M(0x37, c->x, RLA) M_ZPI_M(0x57, c->x, SRE)
    M_ZPI_M(0x77, c->x, RRA) M_ZPI_M(0xD7, c->x, DCP) M_ZPI_M(0xF7, c->x, ISC)

    /* ---- absolute */
    M_ABS_R(0x0D, ORA) M_ABS_R(0x2D, AND) M_ABS_R(0x4D, EOR) M_ABS_R(0x6D, ADC)
    M_ABS_R(0xAD, LDA) M_ABS_R(0xCD, CMP) M_ABS_R(0xED, SBC) M_ABS_R(0x2C, BIT)
    M_ABS_R(0xAC, LDY) M_ABS_R(0xAE, LDX) M_ABS_R(0xCC, CPY) M_ABS_R(0xEC, CPX)
    M_ABS_R(0xAF, LAX) M_ABS_R(0x0C, NOP)
    M_ABS_W(0x8D, c->a) M_ABS_W(0x8E, c->x) M_ABS_W(0x8C, c->y) M_ABS_W(0x8F, c->a & c->x)
    M_ABS_M(0x0E, ASL) M_ABS_M(0x4E, LSR) M_ABS_M(0x2E, ROL) M_ABS_M(0x6E, ROR)
    M_ABS_M(0xEE, INC) M_ABS_M(0xCE, DEC)
    M_ABS_M(0x0F, SLO) M_ABS_M(0x2F, RLA) M_ABS_M(0x4F, SRE) M_ABS_M(0x6F, RRA)
    M_ABS_M(0xCF, DCP) M_ABS_M(0xEF, ISC)

    /* ---- absolute indexed */
    M_ABI_R(0x1D, c->x, ORA) M_ABI_R(0x3D, c->x, AND) M_ABI_R(0x5D, c->x, EOR)
    M_ABI_R(0x7D, c->x, ADC) M_ABI_R(0xBD, c->x, LDA) M_ABI_R(0xDD, c->x, CMP)
    M_ABI_R(0xFD, c->x, SBC) M_ABI_R(0xBC, c->x, LDY)
    M_ABI_R(0x19, c->y, ORA) M_ABI_R(0x39, c->y, AND) M_ABI_R(0x59, c->y, EOR)
    M_ABI_R(0x79, c->y, ADC) M_ABI_R(0xB9, c->y, LDA) M_ABI_R(0xD9, c->y, CMP)
    M_ABI_R(0xF9, c->y, SBC) M_ABI_R(0xBE, c->y, LDX) M_ABI_R(0xBF, c->y, LAX)
    M_ABI_R(0xBB, c->y, LAS)
    M_ABI_R(0x1C, c->x, NOP) M_ABI_R(0x3C, c->x, NOP) M_ABI_R(0x5C, c->x, NOP)
    M_ABI_R(0x7C, c->x, NOP) M_ABI_R(0xDC, c->x, NOP) M_ABI_R(0xFC, c->x, NOP)
    M_ABI_W(0x9D, c->x, c->a) M_ABI_W(0x99, c->y, c->a)
    M_ABI_M(0x1E, c->x, ASL) M_ABI_M(0x5E, c->x, LSR) M_ABI_M(0x3E, c->x, ROL)
    M_ABI_M(0x7E, c->x, ROR) M_ABI_M(0xFE, c->x, INC) M_ABI_M(0xDE, c->x, DEC)
    M_ABI_M(0x1F, c->x, SLO) M_ABI_M(0x3F, c->x, RLA) M_ABI_M(0x5F, c->x, SRE)
    M_ABI_M(0x7F, c->x, RRA) M_ABI_M(0xDF, c->x, DCP) M_ABI_M(0xFF, c->x, ISC)
    M_ABI_M(0x1B, c->y, SLO) M_ABI_M(0x3B, c->y, RLA) M_ABI_M(0x5B, c->y, SRE)
    M_ABI_M(0x7B, c->y, RRA) M_ABI_M(0xDB, c->y, DCP) M_ABI_M(0xFB, c->y, ISC)
    M_ABI_SH(0x9C, c->x, (void)0, c->y)                  /* SHY abs,X */
    M_ABI_SH(0x9E, c->y, (void)0, c->x)                  /* SHX abs,Y */
    M_ABI_SH(0x9F, c->y, (void)0, c->a & c->x)           /* SHA abs,Y */
    M_ABI_SH(0x9B, c->y, c->s = c->a & c->x, c->s)       /* TAS abs,Y */

    /* ---- (zp,X) */
    M_IZX_R(0x01, ORA) M_IZX_R(0x21, AND) M_IZX_R(0x41, EOR) M_IZX_R(0x61, ADC)
    M_IZX_R(0xA1, LDA) M_IZX_R(0xC1, CMP) M_IZX_R(0xE1, SBC) M_IZX_R(0xA3, LAX)
    M_IZX_W(0x81, c->a) M_IZX_W(0x83, c->a & c->x)
    M_IZX_M(0x03, SLO) M_IZX_M(0x23, RLA) M_IZX_M(0x43, SRE) M_IZX_M(0x63, RRA)
    M_IZX_M(0xC3, DCP) M_IZX_M(0xE3, ISC)

    /* ---- (zp),Y */
    M_IZY_R(0x11, ORA) M_IZY_R(0x31, AND) M_IZY_R(0x51, EOR) M_IZY_R(0x71, ADC)
    M_IZY_R(0xB1, LDA) M_IZY_R(0xD1, CMP) M_IZY_R(0xF1, SBC) M_IZY_R(0xB3, LAX)
    M_IZY_W(0x91, c->a)
    M_IZY_M(0x13, SLO) M_IZY_M(0x33, RLA) M_IZY_M(0x53, SRE) M_IZY_M(0x73, RRA)
    M_IZY_M(0xD3, DCP) M_IZY_M(0xF3, ISC)
    M_IZY_PRE(0x93)                                       /* SHA (zp),Y */
    case ST(0x93, 3): IZY_ADDR(); break;
    case ST(0x93, 4): SH_WRITE(c->a & c->x); POLL(); break;
    case ST(0x93, 5): FETCH(); break;

    /* ---- JAM / KIL: $FFFF, $FFFE, $FFFE, then $FFFF forever */
    M_JAM(0x02) M_JAM(0x12) M_JAM(0x22) M_JAM(0x32) M_JAM(0x42) M_JAM(0x52)
    M_JAM(0x62) M_JAM(0x72) M_JAM(0x92) M_JAM(0xB2) M_JAM(0xD2) M_JAM(0xF2)

    default: FETCH(); break;   /* not reached: every opcode ends in FETCH */
    }
    c->t++;
    if (UNLIKELY(c->res_age >= 3)) {
        /* warm reset (see slow_tick): writes become reads, and instead of
           the next opcode fetch the bus holds at PC */
        rw = 1;
        if (sync) { sync = 0; c->res = 2; }
    }
    c->sync = (uint8_t)sync;

    uint32_t bus = ab | (rw ? (uint32_t)CPU_RW : 0) | (sync ? (uint32_t)CPU_SYNC : 0);
    uint64_t out = (pins & ~(CPU_A_MASK | CPU_RW | CPU_SYNC)) | bus;
    if (!rw) out = CPU_SET_DATA(out, db);
    c->hold = bus;
    c->halted = (uint8_t)(rw & ((pins & CPU_RDY) == 0));
    sample_inputs(c, pins);
    c->cycles++;
    return out;
}
