/*
 * sid.c - cycle-clocked MOS 6581 / 8580 SID emulation (C99, no dependencies)
 *
 * Own implementation written from public documentation; see README.md for the
 * model description and the list of documents that informed each part.
 *
 * Signal units used internally ("voice units"): a voice at full envelope with
 * a full-scale waveform swings 1.0 peak-to-peak.
 */
#include "sid.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------------ */
/* constants                                                                 */
/* ------------------------------------------------------------------------ */

#define BLK        512        /* cycles synthesised per inner block         */
#define FIFO_SIZE  4096       /* queued output samples                      */
#define N1_MAX     256        /* max stage-1 decimator taps                 */
#define P2         64         /* stage-2 polyphase phases (linear interp.)  */
#define OUT_SCALE  12000.0    /* voice units -> int16                       */
#define X1_SIZE    (N1_MAX + 8 * BLK)

enum { ENV_ATTACK = 0, ENV_DECAY = 1, ENV_RELEASE = 2 };

/* Envelope rate-counter periods (cycles between envelope steps) for the 16
 * rate settings. Derived from the datasheet times (2 ms .. 8 s for 256
 * steps at 1 MHz) as documented for the real chip: the counter compares
 * against a period that is one more than round(t * 1 MHz / 256). */
static const uint16_t rate_period[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907,
    11720, 19532, 31251
};

/* ------------------------------------------------------------------------ */
/* tables / resampler state (heap)                                           */
/* ------------------------------------------------------------------------ */

struct sid_tables {
    float    wave_dac[4096];      /* 12-bit waveform DAC, 0..1               */
    float    env_dac[256];        /* 8-bit envelope DAC, 0..1                */
    float    fc_w[2048];          /* SVF coefficient per FC value            */
    double   fc_hz[2048];         /* cutoff (Hz) per FC value                */
    uint16_t comb[4][8192];       /* combined waveforms: TS, PT, PS, PST      */

    /* stage 1: integer decimation by D with an n1-tap FIR */
    int    D, n1, dcount;
    float  h1[N1_MAX];
    float  x1[X1_SIZE];           /* linear history buffer, compacted rarely */
    int    x1pos;                 /* index where the next sample goes        */
    /* stage 2: fractional polyphase resampler */
    int    n2, x2pos;
    float *h2;                    /* (P2 + 1) * n2                           */
    float *x2;                    /* 2 * n2                                  */
    int64_t d, step;              /* 32.32 fixed point                       */
    /* output FIFO */
    int16_t fifo[FIFO_SIZE];
    int     fhead, fcount;
};

/* ------------------------------------------------------------------------ */
/* small helpers                                                             */
/* ------------------------------------------------------------------------ */

/* R-2R ladder DAC by superposition. r2 is the 2R/R ratio; `terminated`
 * selects whether the LSB end has the 2R termination to ground. On the 6581
 * the termination is missing and 2R/R is about 2.2, which makes the bit
 * weights non-binary (the MSB is worth slightly less than all lower bits). */
static void ladder_weights(double *w, int bits, double r2, int terminated)
{
    for (int k = 0; k < bits; k++) {
        double vt = 0, rt = 0;
        for (int j = 0; j < bits; j++) {
            double b = (j == k) ? 1.0 : 0.0;
            if (j == 0) {
                if (terminated) { vt = b * 0.5; rt = r2 * 0.5; }
                else            { vt = b;       rt = r2; }
            } else {
                double rs = rt + 1.0;              /* series R to next node */
                vt = (vt * r2 + b * rs) / (r2 + rs);
                rt = rs * r2 / (rs + r2);
            }
        }
        w[k] = vt;
    }
    double sum = 0;
    for (int k = 0; k < bits; k++) sum += w[k];
    for (int k = 0; k < bits; k++) w[k] /= sum;    /* full scale = 1.0 */
}

static void build_dac(float *out, int bits, int is6581)
{
    double w[16];
    if (is6581) ladder_weights(w, bits, 2.20, 0);
    else        ladder_weights(w, bits, 2.00, 1);
    for (int c = 0; c < (1 << bits); c++) {
        double v = 0;
        for (int k = 0; k < bits; k++) if (c >> k & 1) v += w[k];
        out[c] = (float)v;
    }
}

static double softplus(double u) { return u > 30 ? u : log1p(exp(u)); }

/* cutoff frequency model (Hz) for an 11-bit FC value */
static double cutoff_model(sid_model m, int fc, double curve)
{
    if (m == SID_8580) {
        /* near-linear: ~30 Hz .. 12 kHz */
        return 30.0 + fc * (11970.0 / 2047.0);
    }
    /* 6581: FC -> unterminated R-2R DAC (x, 0..1, with the kink at $400)
     * -> VCR gate voltage. Conductance of the VCR transistor follows an
     * EKV-style softplus (exponential below threshold, linear above),
     * in parallel with a fixed resistor (sets the ~215 Hz floor) and in
     * series with a fixed resistance (limits the top end, ~18 kHz). */
    double w[11];
    ladder_weights(w, 11, 2.20, 0);
    double x = 0;
    for (int k = 0; k < 11; k++) if (fc >> k & 1) x += w[k];
    double x0 = 0.48 + (curve - 0.5) * 0.10;
    double g  = 7370.0 * softplus((x - x0) / 0.065);
    g = g / (1.0 + g / 25000.0);
    return 215.0 + g;
}

/* Combined waveforms. The waveform selector ties the selected waveform bit
 * lines together (a "wired AND": a 0 driver beats a 1 driver), and on the
 * 6581 neighbouring bit lines leak into each other so isolated 1 bits are
 * pulled down. Model: per bit, drive v = ones / (ones + 3*zeros); smear v
 * over neighbours with a coupling kernel; output bit = all sources 1 and
 * smeared drive > threshold. 8580 uses much weaker coupling (close to a
 * plain AND). Approximate - not derived from sampled chip data. */
static void build_combined(struct sid_tables *t, int is6581)
{
    const double cw6581[4] = { 1.0, 0.65, 0.30, 0.10 };
    const double cw8580[4] = { 1.0, 0.15, 0.03, 0.0 };
    const double *cw = is6581 ? cw6581 : cw8580;
    const double thr = is6581 ? 0.60 : 0.50;
    for (int idx = 0; idx < 8192; idx++) {
        unsigned a = idx & 0xFFF, flip = idx >> 12;
        unsigned msb = ((a >> 11) ^ flip) & 1;
        unsigned S = a;
        unsigned T = (((msb ? ~a : a) << 1) & 0xFFE) | msb;
        unsigned P = 0xFFF;
        unsigned srcs[4][3] = { {T, S, 0}, {P, T, 0}, {P, S, 0}, {P, S, T} };
        int nsrc[4] = { 2, 2, 2, 3 };
        for (int c = 0; c < 4; c++) {
            double v[12];
            unsigned andv = 0xFFF;
            for (int i = 0; i < 12; i++) {
                int ones = 0;
                for (int k = 0; k < nsrc[c]; k++) ones += (srcs[c][k] >> i) & 1;
                int zeros = nsrc[c] - ones;
                v[i] = ones / (ones + 3.0 * zeros);
            }
            for (int k = 0; k < nsrc[c]; k++) andv &= srcs[c][k];
            unsigned outv = 0;
            for (int i = 0; i < 12; i++) {
                if (!(andv >> i & 1)) continue;
                double num = 0, den = 0;
                for (int j = 0; j < 12; j++) {
                    int dd = abs(i - j);
                    if (dd > 3) continue;
                    num += cw[dd] * v[j];
                    den += cw[dd];
                }
                if (num / den > thr) outv |= 1u << i;
            }
            t->comb[c][idx] = (uint16_t)outv;
        }
    }
}

static double bessel_i0(double x)
{
    double sum = 1, term = 1, q = x * x / 4;
    for (int k = 1; k < 64; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

static double sinc(double x) { return fabs(x) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x); }

/* windowed-sinc value; x in samples, fcn = cutoff / rate, half = half length */
static double kaiser_sinc(double x, double fcn, double half, double beta)
{
    double r = x / half;
    if (r <= -1.0 || r >= 1.0) return 0.0;
    return 2.0 * fcn * sinc(2.0 * fcn * x) * bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
}

/* ------------------------------------------------------------------------ */
/* noise                                                                     */
/* ------------------------------------------------------------------------ */

/* 23-bit LFSR, feedback = bit22 XOR bit17 shifted into bit 0. */
uint32_t sid_lfsr_next(uint32_t l)
{
    return ((l << 1) | (((l >> 22) ^ (l >> 17)) & 1)) & 0x7FFFFF;
}

/* LFSR bits 22,20,16,13,11,7,4,2 -> waveform bits 11..4 */
static inline uint32_t noise_bits(uint32_t l)
{
    return ((l >> 11) & 0x800) | ((l >> 10) & 0x400) | ((l >> 7) & 0x200) |
           ((l >> 5) & 0x100) | ((l >> 4) & 0x080) | ((l >> 1) & 0x040) |
           ((l << 1) & 0x020) | ((l << 2) & 0x010);
}

/* inverse mapping: waveform bits 11..4 -> LFSR tap positions */
static inline uint32_t noise_unbits(uint32_t w)
{
    return ((w & 0x800) << 11) | ((w & 0x400) << 10) | ((w & 0x200) << 7) |
           ((w & 0x100) << 5) | ((w & 0x080) << 4) | ((w & 0x040) << 1) |
           ((w & 0x020) >> 1) | ((w & 0x010) >> 2);
}
#define NOISE_TAPS 0x512894u   /* bits 22,20,16,13,11,7,4,2 */

/* ------------------------------------------------------------------------ */
/* waveform output                                                           */
/* ------------------------------------------------------------------------ */

static uint32_t wave_nonnoise(const struct sid_tables *t, const sid_voice *v,
                              unsigned sel, uint32_t acc, uint32_t src_acc)
{
    uint32_t a = acc >> 12;
    uint32_t flip = (v->ctrl & 4) ? (src_acc >> 23) & 1 : 0;
    uint32_t pulse = ((a >= v->pw) || (v->ctrl & 8)) ? 0xFFF : 0;
    switch (sel & 7) {
    case 0: return 0xFFF;
    case 1: {
        uint32_t m = (acc ^ (flip << 23)) & 0x800000;
        return ((m ? ~acc : acc) >> 11) & 0xFFF;
    }
    case 2: return a;
    case 3: return t->comb[0][a | flip << 12];
    case 4: return pulse;
    case 5: return pulse ? t->comb[1][a | flip << 12] : 0;
    case 6: return pulse ? t->comb[2][a] : 0;
    default: return pulse ? t->comb[3][a | flip << 12] : 0;
    }
}

/* full waveform selector output (12 bit) for any control value */
static inline uint32_t wave_out(const struct sid_tables *t, sid_voice *v,
                                uint32_t acc, uint32_t src_acc)
{
    unsigned sel = v->ctrl >> 4;
    switch (sel) {
    case 0x0:
        if (v->float_ttl > 0 && --v->float_ttl == 0) v->float_out = 0;
        return v->float_out;
    case 0x1: {
        uint32_t m = acc;
        if (v->ctrl & 4) m ^= src_acc;
        return (((m & 0x800000) ? ~acc : acc) >> 11) & 0xFFF;
    }
    case 0x2: return acc >> 12;
    case 0x4: return (((acc >> 12) >= v->pw) || (v->ctrl & 8)) ? 0xFFF : 0;
    case 0x8: return v->noise_out;
    default:
        if (sel & 8) return v->noise_out & wave_nonnoise(t, v, sel, acc, src_acc);
        return wave_nonnoise(t, v, sel, acc, src_acc);
    }
}

/* Clock the noise LFSR once. When noise is combined with other waveforms,
 * the selector pulls LFSR tap bits low wherever the combined output is 0
 * (this is what eventually "locks up" the noise generator). */
static void noise_clock(const struct sid_tables *t, sid_voice *v,
                        uint32_t acc, uint32_t src_acc)
{
    v->lfsr = sid_lfsr_next(v->lfsr);
    v->noise_out = noise_bits(v->lfsr);
    if ((v->ctrl & 0x80) && (v->ctrl & 0x70)) {
        uint32_t w = v->noise_out & wave_nonnoise(t, v, v->ctrl >> 4, acc, src_acc);
        v->lfsr &= ~NOISE_TAPS | noise_unbits(w);
        v->noise_out = noise_bits(v->lfsr);
    }
}

/* ------------------------------------------------------------------------ */
/* envelope                                                                  */
/* ------------------------------------------------------------------------ */

static inline void env_step(sid_voice *v)
{
    if (v->state != ENV_ATTACK) {
        if (++v->exp_cnt != v->exp_per) return;
    }
    v->exp_cnt = 0;
    if (v->hold_zero) return;
    switch (v->state) {
    case ENV_ATTACK:
        v->env = (v->env + 1) & 0xFF;
        if (v->env == 0xFF) {
            v->state = ENV_DECAY;
            v->rate_per = rate_period[v->ad & 15];
        }
        break;
    case ENV_DECAY:
        if (v->env != (v->sr >> 4) * 0x11) v->env = (v->env - 1) & 0xFF;
        break;
    default:
        v->env = (v->env - 1) & 0xFF;
        break;
    }
    /* exponential decay approximation: the step divider changes at these
     * envelope levels (piece-wise linear approximation of an exponential) */
    switch (v->env) {
    case 0xFF: v->exp_per = 1;  break;
    case 0x5D: v->exp_per = 2;  break;
    case 0x36: v->exp_per = 4;  break;
    case 0x1A: v->exp_per = 8;  break;
    case 0x0E: v->exp_per = 16; break;
    case 0x06: v->exp_per = 30; break;
    case 0x00: v->exp_per = 1;  v->hold_zero = 1; break;
    default: break;
    }
}

/* ------------------------------------------------------------------------ */
/* mixer / filter control                                                    */
/* ------------------------------------------------------------------------ */

static void update_filter(sid *s)
{
    unsigned filt = s->res_filt & 15, res = s->res_filt >> 4;
    unsigned mv = s->mode_vol;
    for (int i = 0; i < 3; i++) {
        s->route_f[i] = (filt >> i & 1) ? 1.0f : 0.0f;
        s->route_d[i] = (filt >> i & 1) ? 0.0f : 1.0f;
    }
    if ((mv & 0x80) && !(filt & 4)) s->route_d[2] = 0.0f;   /* 3 OFF */
    s->mode_lp = (mv & 0x10) ? 1.0f : 0.0f;
    s->mode_bp = (mv & 0x20) ? 1.0f : 0.0f;
    s->mode_hp = (mv & 0x40) ? 1.0f : 0.0f;
    s->vol = (float)(mv & 15) / 15.0f;
    s->w_fc = s->t->fc_w[s->fc & 0x7FF];
    if (s->model == SID_6581) s->q_inv = (float)(1.4142 - 0.064 * res);
    else                      s->q_inv = (float)pow(2.0, (4.0 - res) / 8.0);
}

static void build_fc_tables(sid *s)
{
    for (int fc = 0; fc < 2048; fc++) {
        double hz = cutoff_model(s->model, fc, s->filter_curve);
        s->t->fc_hz[fc] = hz;
        s->t->fc_w[fc] = (float)(2.0 * sin(M_PI * hz / s->clock_hz));
    }
}

/* ------------------------------------------------------------------------ */
/* resampler                                                                 */
/* ------------------------------------------------------------------------ */

static int build_resampler(sid *s)
{
    struct sid_tables *t = s->t;
    const double A = 80.0, beta = 0.1102 * (A - 8.7);
    double clk = s->clock_hz, fs = s->sample_rate;

    /* stage 1 */
    int D = (int)floor(clk / (2.5 * fs));
    if (D < 1) D = 1;
    t->D = D;
    double fi = clk / D;
    if (D == 1) {
        t->n1 = 1; t->h1[0] = 1.0f;
    } else {
        double fp = fmin(20000.0, 0.45 * fs), fst = fi - 0.5 * fs;
        double dw = 2 * M_PI * (fst - fp) / clk;
        int n = (int)ceil((A - 7.95) / (2.285 * dw)) + 1;
        if (n < 8) n = 8;
        if (n > N1_MAX) n = N1_MAX;
        t->n1 = n;
        double fcn = 0.5 * (fp + fst) / clk, half = 0.5 * n, sum = 0;
        for (int k = 0; k < n; k++) {
            double x = k - 0.5 * (n - 1);
            t->h1[k] = (float)kaiser_sinc(x, fcn, half, beta);
            sum += t->h1[k];
        }
        for (int k = 0; k < n; k++) t->h1[k] = (float)(t->h1[k] / sum);
    }
    memset(t->x1, 0, sizeof t->x1);
    t->x1pos = t->n1 - 1;
    t->dcount = D;

    /* stage 2: passband 0..fp2, stopband from fs - fp2. Components between
     * fs/2 and fs - fp2 alias only into fp2..fs/2 (above 20 kHz). */
    double fp2 = fmin(20000.0, 0.45 * fs), fs2 = fs - fp2;
    if (fs2 > fi * 0.5) fs2 = fi * 0.5;
    double dw2 = 2 * M_PI * (fs2 - fp2) / fi;
    int n2 = (int)ceil((A - 7.95) / (2.285 * dw2)) + 1;
    if (n2 < 8) n2 = 8;
    n2 = (n2 + 7) & ~7;
    t->n2 = n2;
    t->h2 = (float *)malloc(sizeof(float) * (size_t)(P2 + 1) * n2);
    t->x2 = (float *)calloc((size_t)2 * n2, sizeof(float));
    if (!t->h2 || !t->x2) return -1;
    double fcn2 = 0.5 * (fp2 + fs2) / fi, half2 = 0.5 * n2;
    for (int p = 0; p <= P2; p++) {
        double f = (double)p / P2, sum = 0;
        float *row = t->h2 + (size_t)p * n2;
        for (int q = 0; q < n2; q++) {
            row[q] = (float)kaiser_sinc(f + half2 - 1 - q, fcn2, half2, beta);
            sum += row[q];
        }
        for (int q = 0; q < n2; q++) row[q] = (float)(row[q] / sum);
    }
    t->x2pos = 0;
    t->d = 0;
    t->step = (int64_t)llround(fi / fs * 4294967296.0);
    return 0;
}

/* Dot products with independent partial sums so the compiler can keep
 * several FMAs in flight / vectorise without -ffast-math. */
static inline float dot(const float *a, const float *b, int n)
{
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 += a[i] * b[i];         s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2]; s3 += a[i + 3] * b[i + 3];
        s4 += a[i + 4] * b[i + 4]; s5 += a[i + 5] * b[i + 5];
        s6 += a[i + 6] * b[i + 6]; s7 += a[i + 7] * b[i + 7];
    }
    for (; i < n; i++) s0 += a[i] * b[i];
    return ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));
}

/* two dot products sharing x: returns a.x in *ra, b.x in *rb */
static inline void dot2(const float *a, const float *b, const float *x, int n,
                        float *ra, float *rb)
{
    float sa[8] = { 0 }, sb[8] = { 0 };
    int i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; k++) {
            sa[k] += a[i + k] * x[i + k];
            sb[k] += b[i + k] * x[i + k];
        }
    for (; i < n; i++) { sa[0] += a[i] * x[i]; sb[0] += b[i] * x[i]; }
    *ra = ((sa[0] + sa[1]) + (sa[2] + sa[3])) + ((sa[4] + sa[5]) + (sa[6] + sa[7]));
    *rb = ((sb[0] + sb[1]) + (sb[2] + sb[3])) + ((sb[4] + sb[5]) + (sb[6] + sb[7]));
}

static inline void fifo_put(struct sid_tables *t, int16_t v)
{
    int pos = t->fhead + t->fcount;
    if (pos >= FIFO_SIZE) pos -= FIFO_SIZE;
    t->fifo[pos] = v;
    if (t->fcount < FIFO_SIZE) t->fcount++;
    else if (++t->fhead == FIFO_SIZE) t->fhead = 0;    /* drop oldest */
}

static void stage2_push(sid *s, float y)
{
    struct sid_tables *t = s->t;
    const int n2 = t->n2;
    t->x2[t->x2pos] = y;
    t->x2[t->x2pos + n2] = y;
    if (++t->x2pos == n2) t->x2pos = 0;
    const int64_t ONE = (int64_t)1 << 32;
    while (t->d < ONE) {
        uint32_t f = (uint32_t)t->d;                    /* 0.32 fraction */
        int p = (int)(f >> 26);                         /* 6 bits: P2=64 */
        float al = (float)(f & 0x3FFFFFF) * (1.0f / 67108864.0f);
        const float *ha = t->h2 + (size_t)p * n2, *hb = ha + n2;
        const float *x = t->x2 + t->x2pos;
        float sa, sb;
        dot2(ha, hb, x, n2, &sa, &sb);
        double o = sa + al * (sb - sa);
        /* C64 output stage: AC coupling high-pass (~16 Hz) */
        double hp = s->hp_a * (s->hp_y + o - s->hp_x);
        s->hp_x = o; s->hp_y = hp;
        double v = hp * OUT_SCALE;
        int iv = (int)lrint(v);
        if (iv > 32767) iv = 32767;
        if (iv < -32768) iv = -32768;
        fifo_put(t, (int16_t)iv);
        t->d += t->step;
    }
    t->d -= ONE;
}

static void decimate(sid *s, int n)
{
    struct sid_tables *t = s->t;
    const int n1 = t->n1, D = t->D;
    int j;
    /* window for the sample at block index j: x1[x1pos + j - (n1-1) .. x1pos + j] */
    const float *base = t->x1 + t->x1pos - (n1 - 1);
    for (j = t->dcount - 1; j < n; j += D) {
        stage2_push(s, dot(t->h1, base + j, n1));
    }
    t->dcount = j - n + 1;
    t->x1pos += n;
}

/* make room for up to BLK new samples at x1pos */
static inline float *x1_reserve(struct sid_tables *t)
{
    if (t->x1pos + BLK > X1_SIZE) {
        memmove(t->x1, t->x1 + t->x1pos - (t->n1 - 1), sizeof(float) * (size_t)(t->n1 - 1));
        t->x1pos = t->n1 - 1;
    }
    return t->x1 + t->x1pos;
}

/* ------------------------------------------------------------------------ */
/* per-cycle synthesis                                                       */
/* ------------------------------------------------------------------------ */

/* Cubic soft clipper: y = x - x^3/(3L^2) for |x| <= L, +-2L/3 beyond.
 * Unity slope at 0, smooth up to the knee, no division. Used as a static
 * (memoryless) nonlinearity on the 6581 filter summer output. */
static inline float sat(float x, float L, float inv3l2)
{
    float xc = x > L ? L : (x < -L ? -L : x);
    return xc - xc * xc * xc * inv3l2;
}

/* The 15-bit rate counter (incremented in synth()) reached its period:
 * reset it and step the envelope. The comparison is for equality, so if the
 * period is lowered below the current count the counter has to wrap around
 * 0x8000 first (the "ADSR delay bug", up to ~33 ms). */
static void env_tick(sid_voice *v)
{
    v->rate_cnt = 0;
    env_step(v);
}

static void synth(sid *s, float *dst, int n)
{
    struct sid_tables *t = s->t;
    sid_voice *v0 = &s->v[0], *v1 = &s->v[1], *v2 = &s->v[2];
    const float *wdac = t->wave_dac, *edac = t->env_dac;
    const float wz = s->wave_zero;
    const float rf0 = s->route_f[0], rf1 = s->route_f[1], rf2 = s->route_f[2];
    const float rd0 = s->route_d[0], rd1 = s->route_d[1], rd2 = s->route_d[2];
    const float mlp = s->mode_lp, mbp = s->mode_bp, mhp = s->mode_hp;
    const float w = s->w_fc, q = s->q_inv, vol = s->vol, mdc = s->mixer_dc;
    const float L = s->sat_level, ea = s->ext_lp_a;
    const float inv3l2 = 1.0f / (3.0f * L * L);
    const int nonlin = (s->model == SID_6581);
    float hp = s->hp, bp = s->bp, lp = s->lp, ext = s->ext_lp;
    const uint32_t inc0 = (v0->ctrl & 8) ? 0 : v0->freq;
    const uint32_t inc1 = (v1->ctrl & 8) ? 0 : v1->freq;
    const uint32_t inc2 = (v2->ctrl & 8) ? 0 : v2->freq;
    const uint32_t sy0 = v0->ctrl & 2, sy1 = v1->ctrl & 2, sy2 = v2->ctrl & 2;
    /* hot per-voice state kept in locals */
    uint32_t a0 = v0->acc, a1 = v1->acc, a2 = v2->acc;
    uint32_t rc0 = v0->rate_cnt, rc1 = v1->rate_cnt, rc2 = v2->rate_cnt;
    uint32_t rp0 = v0->rate_per, rp1 = v1->rate_per, rp2 = v2->rate_per;
    float e0 = edac[v0->env], e1 = edac[v1->env], e2 = edac[v2->env];
    uint32_t w0 = v0->last_wave, w1 = v1->last_wave, w2 = v2->last_wave;
    float out = 0;

    for (int i = 0; i < n; i++) {
        /* ---- oscillators ---- */
        uint32_t n0 = (a0 + inc0) & 0xFFFFFF;
        uint32_t n1 = (a1 + inc1) & 0xFFFFFF;
        uint32_t n2 = (a2 + inc2) & 0xFFFFFF;
        uint32_t r0 = ~a0 & n0, r1 = ~a1 & n1, r2 = ~a2 & n2;   /* rising bits */
        /* hard sync: v0 <- v2, v1 <- v0, v2 <- v1 (MSB rising edge). A voice
         * whose sync source is itself being synced this cycle is not synced. */
        if ((r0 | r1 | r2) & 0x800000) {
            int s0 = sy0 && (r2 & 0x800000);
            int s1 = sy1 && (r0 & 0x800000);
            int s2 = sy2 && (r1 & 0x800000);
            if (s0 && !s2) n0 = 0;
            if (s1 && !s0) n1 = 0;
            if (s2 && !s1) n2 = 0;
        }
        a0 = n0; a1 = n1; a2 = n2;
        /* noise LFSR clocked by accumulator bit 19 rising */
        if ((r0 | r1 | r2) & 0x080000) {
            if (r0 & 0x080000) noise_clock(t, v0, n0, n2);
            if (r1 & 0x080000) noise_clock(t, v1, n1, n0);
            if (r2 & 0x080000) noise_clock(t, v2, n2, n1);
        }

        /* ---- waveform selector ---- */
        w0 = wave_out(t, v0, n0, n2);
        w1 = wave_out(t, v1, n1, n0);
        w2 = wave_out(t, v2, n2, n1);

        /* ---- envelopes (15-bit rate counters; slow path out of line) ---- */
        rc0 = (rc0 + 1) & 0x7FFF;
        rc1 = (rc1 + 1) & 0x7FFF;
        rc2 = (rc2 + 1) & 0x7FFF;
        if (rc0 == rp0) { env_tick(v0); rc0 = 0; rp0 = v0->rate_per; e0 = edac[v0->env]; }
        if (rc1 == rp1) { env_tick(v1); rc1 = 0; rp1 = v1->rate_per; e1 = edac[v1->env]; }
        if (rc2 == rp2) { env_tick(v2); rc2 = 0; rp2 = v2->rate_per; e2 = edac[v2->env]; }

        /* ---- voice DCA: (waveform DAC - zero level) * envelope DAC ---- */
        float o0 = (wdac[w0] - wz) * e0;
        float o1 = (wdac[w1] - wz) * e1;
        float o2 = (wdac[w2] - wz) * e2;

        /* ---- filter (two-integrator-loop state variable filter) ---- */
        float fin = o0 * rf0 + o1 * rf1 + o2 * rf2;
        float dir = o0 * rd0 + o1 * rd1 + o2 * rd2;
        /* Chamberlin form: LP integrator first, then summer, then BP */
        float t0 = fin - q * bp;
        lp += w * bp;
        hp = t0 - lp;
        /* 6581: the NMOS "op-amps" have limited swing: soft-clip the summer
         * (HP) output that drives the first integrator */
        if (nonlin) hp = sat(hp, L, inv3l2);
        bp += w * hp;
        float fo = mlp * lp + mbp * bp + mhp * hp;

        /* ---- master volume (multiplying DAC, incl. mixer DC) ---- */
        out = (dir + fo + mdc) * vol;

        /* ---- C64 output stage low-pass (~16 kHz) ---- */
        ext += ea * (out - ext);
        dst[i] = ext;
    }
    /* flush denormal-prone tiny states; guard against non-finite values */
    if (fabsf(bp) < 1e-20f || !(fabsf(bp) < 1e6f)) bp = 0;
    if (fabsf(lp) < 1e-20f || !(fabsf(lp) < 1e6f)) lp = 0;
    s->hp = hp; s->bp = bp; s->lp = lp; s->ext_lp = ext;
    v0->acc = a0; v1->acc = a1; v2->acc = a2;
    v0->rate_cnt = rc0; v1->rate_cnt = rc1; v2->rate_cnt = rc2;
    v0->last_wave = w0; v1->last_wave = w1; v2->last_wave = w2;
    s->last_raw = out;
    s->cycle += (uint64_t)n;
}

/* ------------------------------------------------------------------------ */
/* public API                                                                */
/* ------------------------------------------------------------------------ */

int sid_init(sid *s, sid_model model, double clock_hz, double sample_rate)
{
    memset(s, 0, sizeof *s);
    s->model = (model == SID_8580) ? SID_8580 : SID_6581;
    s->clock_hz = clock_hz > 0 ? clock_hz : SID_PAL_CLOCK;
    s->sample_rate = sample_rate > 0 ? sample_rate : 44100;
    s->filter_curve = 0.5f;
    s->potx = s->poty = 0xFF;
    s->t = (struct sid_tables *)calloc(1, sizeof *s->t);
    if (!s->t) return -1;
    int is6581 = (s->model == SID_6581);
    build_dac(s->t->wave_dac, 12, is6581);
    build_dac(s->t->env_dac, 8, is6581);
    build_combined(s->t, is6581);
    build_fc_tables(s);
    if (build_resampler(s) != 0) { sid_free(s); return -1; }

    if (is6581) {
        s->wave_zero = (float)(0x380 / 4095.0);  /* waveform DAC zero level  */
        s->mixer_dc = 1.0f;                       /* volume-scaled DC offset  */
        s->sat_level = 2.0f;
        s->float_ttl_init = 50000;                /* floating DAC input ~50ms */
        s->bus_ttl = 7000;                        /* data bus value fade      */
    } else {
        s->wave_zero = 0.5f;
        s->mixer_dc = 0.05f;
        s->sat_level = 4.0f;
        s->float_ttl_init = 500000;
        s->bus_ttl = 600000;
    }
    s->ext_lp_a = (float)(1.0 - exp(-2.0 * M_PI * 16000.0 / s->clock_hz));
    s->hp_a = exp(-2.0 * M_PI * 16.0 / s->sample_rate);
    sid_reset(s);
    return 0;
}

void sid_free(sid *s)
{
    if (s->t) {
        free(s->t->h2);
        free(s->t->x2);
        free(s->t);
        s->t = NULL;
    }
}

void sid_reset(sid *s)
{
    for (int i = 0; i < 3; i++) {
        sid_voice *v = &s->v[i];
        memset(v, 0, sizeof *v);
        v->lfsr = 0x7FFFFF;
        v->noise_out = noise_bits(v->lfsr);
        v->state = ENV_RELEASE;
        v->hold_zero = 1;
        v->rate_per = rate_period[0];
        v->exp_per = 1;
    }
    memset(s->regs, 0, sizeof s->regs);
    s->fc = 0; s->res_filt = 0; s->mode_vol = 0;
    s->hp = s->bp = s->lp = 0;
    s->ext_lp = 0;
    s->hp_x = s->hp_y = 0;
    s->bus_value = 0;
    s->bus_time = s->cycle;
    s->last_raw = 0;
    struct sid_tables *t = s->t;
    memset(t->x1, 0, sizeof t->x1);
    memset(t->x2, 0, sizeof(float) * 2 * (size_t)t->n2);
    t->x1pos = t->n1 - 1;
    t->dcount = t->D; t->x2pos = 0; t->d = 0;
    t->fhead = t->fcount = 0;
    update_filter(s);
}

void sid_set_pots(sid *s, uint8_t potx, uint8_t poty) { s->potx = potx; s->poty = poty; }

void sid_set_filter_curve(sid *s, double pos)
{
    if (pos < 0) pos = 0;
    if (pos > 1) pos = 1;
    s->filter_curve = (float)pos;
    build_fc_tables(s);
    update_filter(s);
}

static void write_ctrl(sid *s, int vi, uint8_t val)
{
    sid_voice *v = &s->v[vi];
    uint32_t old = v->ctrl;
    sid_voice *src = &s->v[(vi + 2) % 3];
    /* test bit: accumulator held at zero; noise register refilled with ones.
     * (On the chip the LFSR bits charge towards 1 while TEST is held; we
     * apply this immediately.) */
    if ((val & 8) && !(old & 8)) {
        v->acc = 0;
        v->lfsr = 0x7FFFFF;
        v->noise_out = noise_bits(v->lfsr);
    }
    /* waveform switched off: DAC input floats, keeps the last value */
    if ((val >> 4) == 0 && (old >> 4) != 0) {
        v->float_out = v->last_wave;
        v->float_ttl = s->float_ttl_init;
    }
    /* gate */
    if ((val & 1) && !(old & 1)) {
        v->state = ENV_ATTACK;
        v->rate_per = rate_period[v->ad >> 4];
        v->hold_zero = 0;
    } else if (!(val & 1) && (old & 1)) {
        v->state = ENV_RELEASE;
        v->rate_per = rate_period[v->sr & 15];
    }
    v->ctrl = val;
    /* combined noise: selector immediately pulls LFSR taps low */
    if ((val & 0x80) && (val & 0x70)) {
        uint32_t w = v->noise_out & wave_nonnoise(s->t, v, val >> 4, v->acc, src->acc);
        v->lfsr &= ~NOISE_TAPS | noise_unbits(w);
        v->noise_out = noise_bits(v->lfsr);
    }
}

void sid_write(sid *s, int reg, uint8_t val)
{
    reg &= 31;
    s->bus_value = val;
    s->bus_time = s->cycle;
    if (reg > 0x18) return;
    s->regs[reg] = val;
    if (reg < 21) {
        int vi = reg / 7;
        sid_voice *v = &s->v[vi];
        switch (reg % 7) {
        case 0: v->freq = (v->freq & 0xFF00) | val; break;
        case 1: v->freq = (v->freq & 0x00FF) | ((uint32_t)val << 8); break;
        case 2: v->pw = (v->pw & 0xF00) | val; break;
        case 3: v->pw = (v->pw & 0x0FF) | ((uint32_t)(val & 15) << 8); break;
        case 4: write_ctrl(s, vi, val); break;
        case 5:
            v->ad = val;
            if (v->state == ENV_ATTACK)     v->rate_per = rate_period[val >> 4];
            else if (v->state == ENV_DECAY) v->rate_per = rate_period[val & 15];
            break;
        case 6:
            v->sr = val;
            if (v->state == ENV_RELEASE) v->rate_per = rate_period[val & 15];
            break;
        }
        return;
    }
    switch (reg) {
    case 0x15: s->fc = (s->fc & 0x7F8) | (val & 7); break;
    case 0x16: s->fc = (s->fc & 7) | ((uint32_t)val << 3); break;
    case 0x17: s->res_filt = val; break;
    case 0x18: s->mode_vol = val; break;
    }
    update_filter(s);
}

uint8_t sid_read(sid *s, int reg)
{
    uint8_t v;
    reg &= 31;
    switch (reg) {
    case 0x19: v = s->potx; break;
    case 0x1A: v = s->poty; break;
    case 0x1B: v = (uint8_t)(s->v[2].last_wave >> 4); break;
    case 0x1C: v = (uint8_t)s->v[2].env; break;
    default:
        /* write-only / unused: the last value on the internal data bus,
         * which leaks away after a while (much faster on the 6581) */
        if (s->cycle - s->bus_time > s->bus_ttl) s->bus_value = 0;
        return s->bus_value;
    }
    s->bus_value = v;
    s->bus_time = s->cycle;
    return v;
}

int sid_clock(sid *s, int cycles, int16_t *out, int max_samples)
{
    struct sid_tables *t = s->t;
    while (cycles > 0) {
        int n = cycles < BLK ? cycles : BLK;
        synth(s, x1_reserve(t), n);
        decimate(s, n);
        cycles -= n;
    }
    int k = 0;
    if (out) {
        while (k < max_samples && t->fcount > 0) {
            out[k++] = t->fifo[t->fhead];
            if (++t->fhead == FIFO_SIZE) t->fhead = 0;
            t->fcount--;
        }
    }
    return k;
}

int sid_pending(const sid *s) { return s->t->fcount; }
void sid_drop_pending(sid *s) { s->t->fcount = 0; s->t->fhead = 0; }

/* ------------------------------------------------------------------------ */
/* savestates                                                                */
/* ------------------------------------------------------------------------ */

#define STATE_MAGIC   0x53494453u     /* "SIDS" */
#define STATE_VERSION 1u

typedef struct state_hdr {
    uint32_t magic, version, struct_size, model;
    double   clock_hz, sample_rate;
    int32_t  n1, n2, fcount, reserved;
} state_hdr;

static size_t state_len(int n1, int n2, int fcount)
{
    return sizeof(state_hdr) + sizeof(sid)
         + sizeof(float) * (size_t)(n1 - 1)          /* stage-1 history      */
         + sizeof(float) * (size_t)(2 * n2)          /* stage-2 ring         */
         + 2 * sizeof(int32_t) + 2 * sizeof(int64_t) /* x2pos, dcount, d, step */
         + sizeof(int16_t) * (size_t)fcount;         /* pending samples      */
}

size_t sid_state_size(const sid *s)
{
    return state_len(s->t->n1, s->t->n2, s->t->fcount);
}

void sid_state_save(const sid *s, void *buf)
{
    const struct sid_tables *t = s->t;
    unsigned char *p = (unsigned char *)buf;
    state_hdr h;
    memset(&h, 0, sizeof h);
    h.magic = STATE_MAGIC; h.version = STATE_VERSION;
    h.struct_size = (uint32_t)sizeof(sid);
    h.model = (uint32_t)s->model;
    h.clock_hz = s->clock_hz; h.sample_rate = s->sample_rate;
    h.n1 = t->n1; h.n2 = t->n2; h.fcount = t->fcount;
    memcpy(p, &h, sizeof h); p += sizeof h;
    sid tmp = *s;
    tmp.t = NULL;
    memcpy(p, &tmp, sizeof tmp); p += sizeof tmp;
    memcpy(p, t->x1 + t->x1pos - (t->n1 - 1), sizeof(float) * (size_t)(t->n1 - 1));
    p += sizeof(float) * (size_t)(t->n1 - 1);
    memcpy(p, t->x2, sizeof(float) * (size_t)(2 * t->n2));
    p += sizeof(float) * (size_t)(2 * t->n2);
    int32_t i32[2] = { t->x2pos, t->dcount };
    int64_t i64[2] = { t->d, t->step };
    memcpy(p, i32, sizeof i32); p += sizeof i32;
    memcpy(p, i64, sizeof i64); p += sizeof i64;
    for (int i = 0; i < t->fcount; i++) {
        int k = t->fhead + i;
        if (k >= FIFO_SIZE) k -= FIFO_SIZE;
        memcpy(p, &t->fifo[k], sizeof(int16_t)); p += sizeof(int16_t);
    }
}

int sid_state_load(sid *s, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    state_hdr h;
    if (!buf || len < sizeof h || !s->t) return -1;
    memcpy(&h, p, sizeof h); p += sizeof h;
    if (h.magic != STATE_MAGIC || h.version != STATE_VERSION ||
        h.struct_size != sizeof(sid) || h.n1 < 1 || h.n1 > N1_MAX ||
        h.n2 < 1 || h.fcount < 0 || h.fcount > FIFO_SIZE ||
        (h.model != SID_6581 && h.model != SID_8580))
        return -1;
    if (len < state_len(h.n1, h.n2, h.fcount)) return -1;
    if ((uint32_t)s->model != h.model || s->clock_hz != h.clock_hz ||
        s->sample_rate != h.sample_rate) {
        sid_free(s);
        if (sid_init(s, (sid_model)h.model, h.clock_hz, h.sample_rate) != 0) return -1;
    }
    struct sid_tables *t = s->t;
    if (t->n1 != h.n1 || t->n2 != h.n2) return -1;
    float old_curve = s->filter_curve;
    memcpy(s, p, sizeof(sid)); p += sizeof(sid);
    s->t = t;
    if (s->filter_curve != old_curve) build_fc_tables(s);
    t->x1pos = t->n1 - 1;
    memcpy(t->x1, p, sizeof(float) * (size_t)(t->n1 - 1));
    p += sizeof(float) * (size_t)(t->n1 - 1);
    memcpy(t->x2, p, sizeof(float) * (size_t)(2 * t->n2));
    p += sizeof(float) * (size_t)(2 * t->n2);
    int32_t i32[2];
    int64_t i64[2];
    memcpy(i32, p, sizeof i32); p += sizeof i32;
    memcpy(i64, p, sizeof i64); p += sizeof i64;
    t->x2pos = i32[0]; t->dcount = i32[1];
    t->d = i64[0]; t->step = i64[1];
    if (t->x2pos < 0 || t->x2pos >= t->n2 || t->dcount < 1 || t->dcount > t->D) return -1;
    t->fhead = 0;
    t->fcount = h.fcount;
    memcpy(t->fifo, p, sizeof(int16_t) * (size_t)h.fcount);
    return 0;
}

double sid_cutoff_hz(const sid *s, int fc) { return s->t->fc_hz[fc & 0x7FF]; }
float sid_last_raw(const sid *s) { return s->last_raw; }
double sid_output_scale(void) { return OUT_SCALE; }
