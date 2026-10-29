/*
 * sidtest.c - test suite for sid.c. Writes WAV/CSV files to out/ and prints
 * PASS/FAIL lines. Exit status is the number of failed checks (capped).
 */
#define _POSIX_C_SOURCE 200809L
#include "sid.h"
#include "wav.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CLK    985248.0
#define FS     44100
#define FRAME  19656          /* PAL frame: 312 lines * 63 cycles */
#define OUTDIR "out/"

static int g_pass, g_fail;

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    printf("  %s  ", ok ? "PASS" : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (ok) g_pass++; else g_fail++;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* process CPU time: robust against other load on the machine */
static double cpu_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static const char *mname(sid_model m) { return m == SID_6581 ? "6581" : "8580"; }

/* ------------------------------------------------------------------------ */
/* sample buffers / running                                                 */
/* ------------------------------------------------------------------------ */

typedef struct { int16_t *d; size_t n, cap; } sbuf;

static void sb_push(sbuf *b, const int16_t *x, size_t n)
{
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 65536;
        while (c < b->n + n) c *= 2;
        b->d = (int16_t *)realloc(b->d, c * sizeof(int16_t));
        b->cap = c;
    }
    memcpy(b->d + b->n, x, n * sizeof(int16_t));
    b->n += n;
}
static void sb_free(sbuf *b) { free(b->d); b->d = NULL; b->n = b->cap = 0; }

/* clock `cycles`; append output to b (or discard when b == NULL) */
static void run(sid *s, long cycles, sbuf *b)
{
    int16_t tmp[8192];
    while (cycles > 0) {
        int c = cycles > FRAME ? FRAME : (int)cycles;
        int k = sid_clock(s, c, tmp, 8192);
        if (b && k) sb_push(b, tmp, (size_t)k);
        cycles -= c;
    }
}

static long sec(double s) { return (long)llround(s * CLK); }

static void w(sid *s, int reg, int val) { sid_write(s, reg, (uint8_t)val); }

static void setfreq(sid *s, int voice, unsigned f)
{
    w(s, voice * 7 + 0, f & 0xFF);
    w(s, voice * 7 + 1, f >> 8);
}
static void setpw(sid *s, int voice, unsigned p)
{
    w(s, voice * 7 + 2, p & 0xFF);
    w(s, voice * 7 + 3, p >> 8);
}
static void setfc(sid *s, unsigned fc)
{
    w(s, 0x15, fc & 7);
    w(s, 0x16, fc >> 3);
}

static double *to_double(const int16_t *x, size_t n)
{
    double *d = (double *)malloc(n * sizeof(double));
    double m = 0;
    for (size_t i = 0; i < n; i++) m += x[i];
    m /= (double)n;
    for (size_t i = 0; i < n; i++) d[i] = x[i] - m;
    return d;
}

/* ------------------------------------------------------------------------ */
/* DSP helpers                                                              */
/* ------------------------------------------------------------------------ */

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                double xr = re[b] * cr - im[b] * ci;
                double xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
                double t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

/* fundamental by rising zero crossings with hysteresis */
static double zc_freq(const double *x, size_t n, double fs)
{
    double rms = 0;
    for (size_t i = 0; i < n; i++) rms += x[i] * x[i];
    rms = sqrt(rms / n);
    double h = 0.2 * rms, first = -1, last = -1;
    long count = 0;
    int armed = 0;
    for (size_t i = 1; i < n; i++) {
        if (x[i] < -h) armed = 1;
        if (armed && x[i - 1] < 0 && x[i] >= 0) {
            double t = (i - 1) + (-x[i - 1]) / (x[i] - x[i - 1]);
            if (first < 0) first = t;
            last = t;
            count++;
            armed = 0;
        }
    }
    if (count < 2) return 0;
    return (count - 1) / ((last - first) / fs);
}

/* Goertzel-like single bin power with Hann window (normalised) */
static double tone_power(const double *x, size_t n, double f, double fs)
{
    double re = 0, im = 0, wsum = 0;
    for (size_t i = 0; i < n; i++) {
        double wv = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));
        double ph = 2 * M_PI * f * i / fs;
        re += x[i] * wv * cos(ph);
        im -= x[i] * wv * sin(ph);
        wsum += wv;
    }
    re /= wsum; im /= wsum;
    return 2 * (re * re + im * im);   /* = (amplitude^2)/2 for a sine */
}

static double total_power(const double *x, size_t n)
{
    double p = 0;
    for (size_t i = 0; i < n; i++) p += x[i] * x[i];
    return p / n;
}

static double db(double p) { return 10 * log10(p > 1e-30 ? p : 1e-30); }

/* ------------------------------------------------------------------------ */
/* 1. pitch                                                                 */
/* ------------------------------------------------------------------------ */

static void test_pitch(sid_model m)
{
    printf("\n[1] Pitch accuracy (%s): f = reg * %.0f / 2^24\n", mname(m), CLK);
    static const unsigned regs[] = { 0x0400, 0x0800, 0x1CD6, 0x4000, 0x9000, 0xF000 };
    static const int waves[] = { 0x11, 0x21, 0x41 };
    static const char *wn[] = { "triangle", "sawtooth", "pulse" };
    double worst = 0;
    sid s;
    sid_init(&s, m, CLK, FS);
    printf("    %-9s %-7s %10s %10s %9s\n", "wave", "reg", "expect Hz", "meas Hz", "err %");
    for (int wi = 0; wi < 3; wi++) {
        for (int ri = 0; ri < 6; ri++) {
            sbuf b = { 0 };
            sid_reset(&s);
            w(&s, 0x18, 0x0F);
            setfreq(&s, 0, regs[ri]);
            setpw(&s, 0, 0x800);
            w(&s, 5, 0x00); w(&s, 6, 0xF0);
            w(&s, 4, waves[wi]);
            run(&s, sec(0.1), NULL);
            run(&s, sec(1.0), &b);
            double *x = to_double(b.d, b.n);
            double fm = zc_freq(x, b.n, FS);
            double fe = regs[ri] * CLK / 16777216.0;
            double err = 100.0 * (fm - fe) / fe;
            if (fabs(err) > worst) worst = fabs(err);
            printf("    %-9s $%04X  %10.3f %10.3f %+9.4f\n", wn[wi], regs[ri], fe, fm, err);
            free(x);
            sb_free(&b);
        }
    }
    check(worst < 0.5, "pitch: worst error %.4f%% (tolerance 0.5%%)", worst);

    /* listening file: all waveforms incl. combined ones at ~440 Hz */
    sbuf b = { 0 };
    sid_reset(&s);
    w(&s, 0x18, 0x0F);
    setfreq(&s, 0, 0x1D45);
    setpw(&s, 0, 0x800);
    w(&s, 5, 0x00); w(&s, 6, 0xF0);
    static const int wl[] = { 0x11, 0x21, 0x41, 0x81, 0x31, 0x51, 0x61, 0x71 };
    double amp_and[8], amp_model[8];
    for (int i = 0; i < 8; i++) {
        w(&s, 4, wl[i]);
        size_t st = b.n;
        run(&s, sec(0.6), &b);
        double *x = to_double(b.d + st + 4410, b.n - st - 4410);
        amp_model[i] = sqrt(total_power(x, b.n - st - 4410));
        free(x);
        amp_and[i] = 0;
    }
    char path[256];
    snprintf(path, sizeof path, OUTDIR "waveforms_%s.wav", mname(m));
    wav_write(path, b.d, b.n, FS);
    printf("    RMS of waveforms (T,S,P,N,TS,PT,PS,PTS) at env max: ");
    for (int i = 0; i < 8; i++) printf("%.0f ", amp_model[i]);
    printf("\n    -> %s\n", path);
    (void)amp_and;
    sb_free(&b);
    sid_free(&s);
}

/* ------------------------------------------------------------------------ */
/* 2. envelope timing                                                       */
/* ------------------------------------------------------------------------ */

static const double ds_attack_ms[16] = {
    2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000
};
static const unsigned period_tab[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251
};

/* clock until ENV3 == target; returns cycles waited (polling granularity step) */
static long wait_env(sid *s, int target, long step, long limit)
{
    long c = 0;
    while (c < limit) {
        if (sid_read(s, 0x1C) == target) return c;
        sid_clock(s, (int)step, NULL, 0);
        sid_drop_pending(s);
        c += step;
    }
    return -1;
}

static void test_envelope(void)
{
    printf("\n[2] Envelope timing vs datasheet (6581; times converted to a 1 MHz clock for comparison)\n");
    sid s;
    sid_init(&s, SID_6581, CLK, FS);
    int ok_exact = 1, ok_ds = 1;
    double worst_ds = 0;
    printf("    rate | attack: meas(cyc) exact(cyc) ms@1MHz  sheet | decay: meas(cyc) exact(cyc) ms@1MHz sheet | release: meas(cyc) ms@1MHz\n");
    for (int r = 0; r < 16; r++) {
        long P = period_tab[r];
        long step = P / 16 > 0 ? P / 16 : 1;
        /* attack */
        sid_reset(&s);
        w(&s, 0x0E * 1 + 5, r << 4);      /* voice 3 AD */
        w(&s, 0x0E + 6, 0xF0);
        w(&s, 0x0E + 4, 0x11);
        long ta = wait_env(&s, 0xFF, step, 40000000);
        long ea = 255 * P;
        /* decay: attack 0, decay r, sustain 0 */
        sid_reset(&s);
        w(&s, 0x0E + 5, 0x00 | r);
        w(&s, 0x0E + 6, 0x00);
        w(&s, 0x0E + 4, 0x11);
        wait_env(&s, 0xFF, 1, 10000);
        long td = wait_env(&s, 0x00, step, 100000000);
        long ed = 756 * P;
        /* release: sustain F, decay = release rate so the rate counter
         * is already below the release period when the gate goes off */
        sid_reset(&s);
        w(&s, 0x0E + 5, 0x00 | r);
        w(&s, 0x0E + 6, 0xF0 | r);
        w(&s, 0x0E + 4, 0x11);
        wait_env(&s, 0xFF, 1, 10000);
        run(&s, 1000, NULL);
        w(&s, 0x0E + 4, 0x10);
        long tr = wait_env(&s, 0x00, step, 100000000);
        long er = 756 * P;
        int e1 = labs(ta - ea) <= P + step, e2 = labs(td - ed) <= P + step, e3 = labs(tr - er) <= P + step;
        if (!(e1 && e2 && e3)) ok_exact = 0;
        double da = 100.0 * (ta / 1000.0 - ds_attack_ms[r]) / ds_attack_ms[r];
        double dd = 100.0 * (td / 1000.0 - 3 * ds_attack_ms[r]) / (3 * ds_attack_ms[r]);
        double dr = 100.0 * (tr / 1000.0 - 3 * ds_attack_ms[r]) / (3 * ds_attack_ms[r]);
        double mx = fmax(fabs(da), fmax(fabs(dd), fabs(dr)));
        if (mx > worst_ds) worst_ds = mx;
        if (mx > 20) ok_ds = 0;
        printf("    %4d | %9ld %9ld %9.1f %6.0f | %10ld %9ld %9.1f %6.0f | %10ld %9.1f\n",
               r, ta, ea, ta / 1000.0, ds_attack_ms[r], td, ed, td / 1000.0,
               3 * ds_attack_ms[r], tr, tr / 1000.0);
    }
    check(ok_exact, "envelope: attack = 255*period, decay/release = 756*period (exp. counter) within one period");
    check(ok_ds, "envelope: all 48 times within 20%% of datasheet table (worst %.1f%%; rate 0 is +15%% on real chips too)", worst_ds);

    /* ENV3 trajectory sampled once per PAL frame */
    sid_reset(&s);
    w(&s, 0x18, 0x0F);
    setfreq(&s, 2, 0x1CD6);
    w(&s, 0x0E + 5, 0x89);   /* A=8 (100 ms) D=9 (750 ms) */
    w(&s, 0x0E + 6, 0xA9);   /* S=A R=9 (750 ms)          */
    w(&s, 0x0E + 4, 0x11);
    FILE *f = fopen(OUTDIR "env3_trajectory.csv", "w");
    if (f) fprintf(f, "frame,env3,gate\n");
    int env[120], mono_a = 1, peak_frame = -1;
    printf("    ENV3 per frame (A=8 D=9 S=A R=9, gate off at frame 60):\n    ");
    for (int fr = 0; fr < 120; fr++) {
        if (fr == 60) w(&s, 0x0E + 4, 0x10);
        env[fr] = sid_read(&s, 0x1C);
        if (f) fprintf(f, "%d,%d,%d\n", fr, env[fr], fr < 60);
        printf("%02X%s", env[fr], (fr % 30 == 29) ? "\n    " : " ");
        if (peak_frame < 0 && fr > 0 && env[fr] < env[fr - 1]) peak_frame = fr - 1;
        run(&s, FRAME, NULL);
    }
    if (f) fclose(f);
    printf("\n");
    mono_a = peak_frame > 0 && env[peak_frame] >= 0xE0;
    check(mono_a && peak_frame >= 4 && peak_frame <= 6 && env[59] == 0xAA && env[119] == 0,
          "ENV3 trajectory: attack peak at frame %d (~100 ms), sustain $%02X, end of release $%02X -> out/env3_trajectory.csv",
          peak_frame, env[59], env[119]);
    sid_free(&s);
}

/* ------------------------------------------------------------------------ */
/* 3. ADSR delay bug                                                        */
/* ------------------------------------------------------------------------ */

static long attack_delay(sid *s)
{
    /* gate on (AD already set) and count cycles until ENV3 leaves 0 */
    w(s, 0x0E + 4, 0x11);
    long c = 0;
    while (sid_read(s, 0x1C) == 0 && c < 100000) {
        sid_clock(s, 1, NULL, 0);
        c++;
    }
    sid_drop_pending(s);
    return c;
}

static void test_adsr_bug(void)
{
    printf("\n[3] ADSR delay bug (15-bit rate counter must wrap when the period is lowered)\n");
    sid s;
    sid_init(&s, SID_6581, CLK, FS);
    const int N = 200;
    long mn = 1 << 30, mx = 0, big = 0;
    double sum = 0;
    for (int k = 0; k < N; k++) {
        sid_reset(&s);
        w(&s, 0x0E + 5, 0x00);
        w(&s, 0x0E + 6, 0x0F);           /* release 15: period 31251 */
        w(&s, 0x0E + 4, 0x11);
        run(&s, 12000, NULL);              /* attack+decay to 0 */
        w(&s, 0x0E + 4, 0x10);             /* release (slow rate) */
        run(&s, 40000 + 1237L * k, NULL);  /* random counter phase */
        w(&s, 0x0E + 5, 0x00);             /* attack 0: period 9 */
        long d = attack_delay(&s);
        if (d < mn) mn = d;
        if (d > mx) mx = d;
        if (d > 0.025 * CLK) big++;
        sum += d;
    }
    double ms = 1000.0 / CLK;
    printf("    no hard restart: %d trials, delay min %.2f ms, mean %.2f ms, max %.2f ms (%ld..%ld cycles)\n",
           N, mn * ms, sum / N * ms, mx * ms, mn, mx);
    check(mx * ms > 31.0 && mx * ms <= 33.4 && big > 0,
          "delay bug: attack delayed up to %.2f ms (theory: up to 32768 cycles = %.2f ms)", mx * ms, 32768 * ms);

    /* GoatTracker-style hard restart: gate off with ADSR $0F00 two frames
     * before the note -> rate counter runs with period 9 -> no delay */
    long mx2 = 0;
    for (int k = 0; k < N; k++) {
        sid_reset(&s);
        w(&s, 0x0E + 5, 0x00);
        w(&s, 0x0E + 6, 0x0F);
        w(&s, 0x0E + 4, 0x11);
        run(&s, 12000, NULL);
        w(&s, 0x0E + 4, 0x10);
        run(&s, 40000 + 1237L * k, NULL);
        w(&s, 0x0E + 5, 0x0F);             /* hard restart ADSR $0F00 */
        w(&s, 0x0E + 6, 0x00);
        run(&s, 2 * FRAME, NULL);
        w(&s, 0x0E + 5, 0x00);
        long d = attack_delay(&s);
        if (d > mx2) mx2 = d;
    }
    printf("    with 2-frame hard restart ($0F00): max delay %ld cycles\n", mx2);
    check(mx2 <= 9, "hard restart: attack starts within %ld cycles (<= 9) in all %d trials", mx2, N);
    sid_free(&s);
}

/* ------------------------------------------------------------------------ */
/* 4. noise                                                                 */
/* ------------------------------------------------------------------------ */

static void test_noise(void)
{
    printf("\n[4] Noise generator\n");
    /* LFSR period */
    uint32_t start = 0x7FFFFF, l = start;
    long period = 0;
    do { l = sid_lfsr_next(l); period++; } while (l != start && period < 20000000);
    check(period == 8388607, "LFSR period %ld (expect 2^23-1 = 8388607)", period);

    sid s;
    sid_init(&s, SID_6581, CLK, FS);
    /* OSC3 statistics */
    setfreq(&s, 2, 0xFFFF);
    w(&s, 0x0E + 4, 0x80);
    long hist[256] = { 0 }, bits[8] = { 0 };
    const int NS = 50000;
    double mean = 0;
    for (int i = 0; i < NS; i++) {
        run(&s, 256, NULL);
        int v = sid_read(&s, 0x1B);
        hist[v]++;
        mean += v;
        for (int b = 0; b < 8; b++) bits[b] += v >> b & 1;
    }
    mean /= NS;
    double chi = 0, e = NS / 256.0;
    for (int i = 0; i < 256; i++) chi += (hist[i] - e) * (hist[i] - e) / e;
    double worstbit = 0;
    for (int b = 0; b < 8; b++) worstbit = fmax(worstbit, fabs(bits[b] / (double)NS - 0.5));
    printf("    OSC3 noise: mean %.2f (ideal 127.5), chi^2(255 dof) = %.1f, worst bit bias %.4f\n", mean, chi, worstbit);
    check(fabs(mean - 127.5) < 2.0 && chi < 340 && worstbit < 0.01, "noise OSC3 values uniformly distributed");

    /* spectrum */
    sbuf b = { 0 };
    sid_reset(&s);
    w(&s, 0x18, 0x0F);
    setfreq(&s, 0, 0xFFFF);
    w(&s, 5, 0x00); w(&s, 6, 0xF0);
    w(&s, 4, 0x81);
    run(&s, sec(0.1), NULL);
    run(&s, sec(3.0), &b);
    wav_write(OUTDIR "noise_6581.wav", b.d, b.n, FS);
    const int NF = 4096;
    double *psd = (double *)calloc(NF / 2, sizeof(double));
    double *re = (double *)malloc(NF * sizeof(double)), *im = (double *)malloc(NF * sizeof(double));
    int segs = 0;
    for (size_t st = 0; st + NF <= b.n; st += NF / 2, segs++) {
        for (int i = 0; i < NF; i++) {
            double wv = 0.5 - 0.5 * cos(2 * M_PI * i / NF);
            re[i] = b.d[st + i] * wv; im[i] = 0;
        }
        fft(re, im, NF);
        for (int k = 0; k < NF / 2; k++) psd[k] += re[k] * re[k] + im[k] * im[k];
    }
    double bandc[] = { 125, 250, 500, 1000, 2000, 4000, 8000 };
    double bdb[7], bmin = 1e9, bmax = -1e9;
    printf("    octave band levels (dB, rel.):");
    for (int i = 0; i < 7; i++) {
        int k0 = (int)(bandc[i] / sqrt(2) * NF / FS), k1 = (int)(bandc[i] * sqrt(2) * NF / FS);
        double p = 0;
        for (int k = k0; k <= k1; k++) p += psd[k];
        bdb[i] = db(p / (k1 - k0 + 1));
    }
    for (int i = 0; i < 7; i++) {
        printf(" %.0fHz:%+.1f", bandc[i], bdb[i] - bdb[3]);
        bmin = fmin(bmin, bdb[i]); bmax = fmax(bmax, bdb[i]);
    }
    printf("\n");
    /* spectral flatness 100 Hz .. 10 kHz */
    int k0 = 100 * NF / FS, k1 = 10000 * NF / FS;
    double lg = 0, ar = 0;
    for (int k = k0; k <= k1; k++) { lg += log(psd[k]); ar += psd[k]; }
    double flat = exp(lg / (k1 - k0 + 1)) / (ar / (k1 - k0 + 1));
    check(bmax - bmin < 10.0 && flat > 0.7,
          "noise spectrum broad: octave bands 125 Hz-8 kHz within %.1f dB (LFSR tap correlation gives a slight tilt), flatness %.2f -> out/noise_6581.wav",
          bmax - bmin, flat);
    free(psd); free(re); free(im);
    sb_free(&b);

    /* lock-up: noise combined with sawtooth fills the LFSR with zeros */
    sid_reset(&s);
    setfreq(&s, 2, 0x2000);
    w(&s, 0x0E + 4, 0x80);
    int distinct_before = 0, distinct_locked = 0, distinct_after = 0;
    int seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < 1000; i++) { run(&s, 200, NULL); int v = sid_read(&s, 0x1B); if (!seen[v]++) distinct_before++; }
    w(&s, 0x0E + 4, 0xA0);             /* noise + saw */
    run(&s, sec(0.1), NULL);
    w(&s, 0x0E + 4, 0x80);             /* back to noise only */
    memset(seen, 0, sizeof seen);
    int lockval = -1;
    for (int i = 0; i < 1000; i++) { run(&s, 200, NULL); int v = sid_read(&s, 0x1B); lockval = v; if (!seen[v]++) distinct_locked++; }
    w(&s, 0x0E + 4, 0x88);             /* test bit resets the LFSR */
    run(&s, 100, NULL);
    w(&s, 0x0E + 4, 0x80);
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < 1000; i++) { run(&s, 200, NULL); int v = sid_read(&s, 0x1B); if (!seen[v]++) distinct_after++; }
    printf("    lock-up: distinct OSC3 values before %d, after noise+saw %d (value $%02X), after TEST %d\n",
           distinct_before, distinct_locked, lockval, distinct_after);
    check(distinct_before > 200 && distinct_locked == 1 && lockval == 0 && distinct_after > 200,
          "noise lock-up after combining with saw, recovered by TEST bit");
    sid_free(&s);
}

/* ------------------------------------------------------------------------ */
/* 5. filter cutoff curves                                                  */
/* ------------------------------------------------------------------------ */

#define NFFT 8192

/* capture noise through filter (route=1) or direct (route=0) */
static void filter_capture(sid *s, int route, unsigned fc, int mode, int res, sbuf *b)
{
    sid_reset(s);
    w(s, 0x18, 0x0F | mode);
    w(s, 0x17, (res << 4) | (route ? 1 : 0));
    setfc(s, fc);
    setfreq(s, 0, 0xFFFF);
    w(s, 5, 0x00); w(s, 6, 0x80);
    w(s, 4, 0x81);
    run(s, sec(0.1), NULL);
    run(s, sec(1.5), b);
}

/* cross-spectral transfer function |H|^2 (dB) of y relative to x */
static void transfer_db(const sbuf *x, const sbuf *y, double *hdb)
{
    static double xr[NFFT], xi[NFFT], yr[NFFT], yi[NFFT];
    static double sxx[NFFT / 2], sxyr[NFFT / 2], sxyi[NFFT / 2];
    memset(sxx, 0, sizeof sxx); memset(sxyr, 0, sizeof sxyr); memset(sxyi, 0, sizeof sxyi);
    size_t n = x->n < y->n ? x->n : y->n;
    for (size_t st = 0; st + NFFT <= n; st += NFFT / 2) {
        for (int i = 0; i < NFFT; i++) {
            double wv = 0.5 - 0.5 * cos(2 * M_PI * i / NFFT);
            xr[i] = x->d[st + i] * wv; xi[i] = 0;
            yr[i] = y->d[st + i] * wv; yi[i] = 0;
        }
        fft(xr, xi, NFFT);
        fft(yr, yi, NFFT);
        for (int k = 0; k < NFFT / 2; k++) {
            sxx[k] += xr[k] * xr[k] + xi[k] * xi[k];
            sxyr[k] += yr[k] * xr[k] + yi[k] * xi[k];
            sxyi[k] += yi[k] * xr[k] - yr[k] * xi[k];
        }
    }
    for (int k = 0; k < NFFT / 2; k++) {
        double hr = sxyr[k] / sxx[k], hi = sxyi[k] / sxx[k];
        hdb[k] = db(hr * hr + hi * hi);
    }
}

static double find_3db(const double *hdb, double fmax)
{
    double bw = (double)FS / NFFT;
    for (int k = 3; k < NFFT / 2; k++) {
        double f = k * bw;
        if (f > fmax) break;
        if (hdb[k] < -3.0103) {
            double f0 = (k - 1) * bw, a = hdb[k - 1], c = hdb[k];
            double t = (a + 3.0103) / (a - c);
            return exp(log(f0) + t * (log(f) - log(f0)));
        }
    }
    return -1;
}

static void test_filter(sid_model m)
{
    printf("\n[5] Filter cutoff (-3 dB, LP, res 0) measured with noise, %s\n", mname(m));
    sid s;
    sid_init(&s, m, CLK, FS);
    sbuf ref = { 0 };
    filter_capture(&s, 0, 0, 0x10, 0, &ref);
    static const unsigned fcs[] = { 0x000, 0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x3FF,
                                    0x400, 0x480, 0x500, 0x580, 0x600, 0x680, 0x700, 0x780, 0x7FF };
    const int NFC = (int)(sizeof fcs / sizeof fcs[0]);
    double meas[32], model[32];
    static double hdb[NFFT / 2];
    char path[256];
    snprintf(path, sizeof path, OUTDIR "filter_curve_%s.csv", mname(m));
    FILE *f = fopen(path, "w");
    if (f) fprintf(f, "fc_reg,model_hz,measured_hz\n");
    printf("    FC     model Hz   meas Hz   err%%\n");
    int ok_match = 1;
    for (int i = 0; i < NFC; i++) {
        sbuf y = { 0 };
        filter_capture(&s, 1, fcs[i], 0x10, 0, &y);
        transfer_db(&ref, &y, hdb);
        meas[i] = find_3db(hdb, 19500);
        model[i] = sid_cutoff_hz(&s, (int)fcs[i]);
        double err = meas[i] > 0 ? 100 * (meas[i] - model[i]) / model[i] : 0;
        if (meas[i] > 0 && fabs(err) > 10) ok_match = 0;
        if (meas[i] < 0 && model[i] < 18000) ok_match = 0;
        printf("    $%03X %9.1f %9.1f %+6.1f%s\n", fcs[i], model[i], meas[i], err, meas[i] < 0 ? "  (>19.5 kHz, beyond measurable band)" : "");
        if (f) fprintf(f, "%u,%.2f,%.2f\n", fcs[i], model[i], meas[i]);
        sb_free(&y);
    }
    if (f) fclose(f);
    check(ok_match, "%s: measured -3 dB points agree with the cutoff model within 10%% -> %s", mname(m), path);
    if (m == SID_6581) {
        double f0 = meas[0], f100 = meas[2], f200 = meas[4], f3ff = meas[8], f400 = meas[9], f600 = meas[13];
        check(f0 > 150 && f0 < 300, "6581: FC=$000 cutoff %.0f Hz (published measurements ~220 Hz)", f0);
        check(f100 / f0 < 1.3, "6581: flat bottom, fc($100)/fc($000) = %.2f", f100 / f0);
        check(f600 / f200 > 10, "6581: steep rise, fc($600)/fc($200) = %.1f", f600 / f200);
        check(f400 < f3ff, "6581: DAC kink at $3FF->$400 (%.0f -> %.0f Hz)", f3ff, f400);
        check(meas[NFC - 1] < 0 || meas[NFC - 1] > 12000, "6581: FC=$7FF cutoff %.0f Hz (>12 kHz)", meas[NFC - 1]);
    } else {
        /* linear regression on measured points */
        double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
        int n = 0;
        for (int i = 0; i < NFC; i++) if (meas[i] > 0) {
            double xv = fcs[i], yv = meas[i];
            sx += xv; sy += yv; sxx += xv * xv; sxy += xv * yv; syy += yv * yv; n++;
        }
        double r = (n * sxy - sx * sy) / sqrt((n * sxx - sx * sx) * (n * syy - sy * sy));
        double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        check(r * r > 0.995, "8580: near-linear curve, R^2 = %.4f, slope %.2f Hz/step, fc($7FF) = %.0f Hz", r * r, slope, meas[NFC - 1]);
    }

    /* resonance: LP peak gain at fc ~ 1 kHz, res 0 vs 15 */
    unsigned fcr = 0;
    for (unsigned k = 0; k < 2048; k++) if (sid_cutoff_hz(&s, (int)k) >= 1000) { fcr = k; break; }
    double peak[2];
    for (int ri = 0; ri < 2; ri++) {
        sbuf y = { 0 };
        filter_capture(&s, 1, fcr, 0x10, ri ? 15 : 0, &y);
        transfer_db(&ref, &y, hdb);
        peak[ri] = -1e9;
        for (int k = 3; k < NFFT / 2; k++) peak[ri] = fmax(peak[ri], hdb[k]);
        sb_free(&y);
    }
    printf("    resonance: LP peak gain at FC=$%03X: res0 %+.1f dB, res15 %+.1f dB\n", fcr, peak[0], peak[1]);
    check(peak[1] > peak[0] + 4, "%s: resonance raises the peak by %.1f dB", mname(m), peak[1] - peak[0]);
    sb_free(&ref);
    sid_free(&s);
}

/* ------------------------------------------------------------------------ */
/* 6. sync / ring modulation                                                */
/* ------------------------------------------------------------------------ */

static double acf_freq(const double *x, size_t n, double fs, int lagmin, int lagmax)
{
    double *r = (double *)calloc((size_t)lagmax + 2, sizeof(double));
    double r0 = 0;
    for (size_t i = 0; i < n; i++) r0 += x[i] * x[i];
    double mx = -1;
    for (int L = lagmin - 1; L <= lagmax + 1; L++) {
        double a = 0;
        for (size_t i = 0; i + L < n; i++) a += x[i] * x[i + L];
        r[L] = a / r0;
        if (L >= lagmin && L <= lagmax && r[L] > mx) mx = r[L];
    }
    int best = -1;
    for (int L = lagmin; L <= lagmax; L++) {
        if (r[L] > 0.95 * mx && r[L] >= r[L - 1] && r[L] >= r[L + 1]) { best = L; break; }
    }
    double fr = 0;
    if (best > 0) {
        double a = r[best - 1], b = r[best], c = r[best + 1];
        double off = 0.5 * (a - c) / (a - 2 * b + c);
        fr = fs / (best + off);
    }
    free(r);
    return fr;
}

static void test_sync_ring(void)
{
    printf("\n[6] Hard sync and ring modulation (6581)\n");
    sid s;
    sid_init(&s, SID_6581, CLK, FS);
    double f1 = 0x1CD6 * CLK / 16777216.0, f3 = 0x0A00 * CLK / 16777216.0;
    double side[2], carrier[2];
    for (int ring = 0; ring < 2; ring++) {
        sbuf b = { 0 };
        sid_reset(&s);
        w(&s, 0x18, 0x8F);                 /* 3 OFF */
        setfreq(&s, 0, 0x1CD6);
        setfreq(&s, 2, 0x0A00);
        w(&s, 5, 0x00); w(&s, 6, 0xF0);
        w(&s, 0x0E + 4, 0x10);             /* voice 3 running, silent */
        w(&s, 4, ring ? 0x15 : 0x11);
        run(&s, sec(0.1), NULL);
        run(&s, sec(1.0), &b);
        if (ring) wav_write(OUTDIR "ringmod_6581.wav", b.d, b.n, FS);
        double *x = to_double(b.d, b.n);
        double tp = total_power(x, b.n);
        side[ring] = db((tone_power(x, b.n, f1 + f3, FS) + tone_power(x, b.n, f1 - f3, FS)) / tp);
        carrier[ring] = db(tone_power(x, b.n, f1, FS) / tp);
        free(x);
        sb_free(&b);
    }
    printf("    f1 = %.1f Hz, f3 = %.1f Hz; sidebands f1+-f3: off %.1f dB, on %.1f dB; carrier: off %.1f dB, on %.1f dB (re total)\n",
           f1, f3, side[0], side[1], carrier[0], carrier[1]);
    check(side[1] - side[0] > 30 && carrier[0] - carrier[1] > 10,
          "ring mod creates f1+-f3 sidebands (+%.0f dB) and suppresses the carrier (-%.0f dB)",
          side[1] - side[0], carrier[0] - carrier[1]);

    /* hard sync: saw at 481 Hz synced to voice 3 at 165 Hz */
    double fs1 = 0x2000 * CLK / 16777216.0, fs3 = 0x0B00 * CLK / 16777216.0, fm[2];
    for (int sy = 0; sy < 2; sy++) {
        sbuf b = { 0 };
        sid_reset(&s);
        w(&s, 0x18, 0x8F);
        setfreq(&s, 0, 0x2000);
        setfreq(&s, 2, 0x0B00);
        w(&s, 5, 0x00); w(&s, 6, 0xF0);
        w(&s, 0x0E + 4, 0x20);
        w(&s, 4, sy ? 0x23 : 0x21);
        run(&s, sec(0.1), NULL);
        run(&s, sec(0.5), &b);
        double *x = to_double(b.d, b.n);
        fm[sy] = acf_freq(x, b.n, FS, 30, 400);
        free(x);
        sb_free(&b);
    }
    printf("    sync: saw %.2f Hz synced to %.2f Hz -> measured fundamental: off %.2f Hz, on %.2f Hz\n",
           fs1, fs3, fm[0], fm[1]);
    check(fabs(fm[0] - fs1) / fs1 < 0.005 && fabs(fm[1] - fs3) / fs3 < 0.005,
          "hard sync: output period follows the sync source (voice 1 <- voice 3)");

    /* sync sweep listening file */
    sbuf b = { 0 };
    sid_reset(&s);
    w(&s, 0x18, 0x8F);
    setfreq(&s, 2, 0x0800);
    w(&s, 5, 0x00); w(&s, 6, 0xF0);
    w(&s, 0x0E + 4, 0x20);
    w(&s, 4, 0x23);
    for (int fr = 0; fr < 150; fr++) {
        setfreq(&s, 0, 0x0900 + fr * 0x80);
        run(&s, FRAME, &b);
    }
    wav_write(OUTDIR "sync_sweep_6581.wav", b.d, b.n, FS);
    sb_free(&b);
    sid_free(&s);
}

/* ------------------------------------------------------------------------ */
/* 7. musical demo                                                          */
/* ------------------------------------------------------------------------ */

static unsigned note_freq(int midi)
{
    double hz = 440.0 * pow(2.0, (midi - 69) / 12.0);
    return (unsigned)lround(hz * 16777216.0 / CLK);
}

typedef struct { long clipped; double peak, rms; size_t n; } demo_stats;

/* Am - F - C - G, two bars each; chord tones as MIDI numbers */
static const int chords[4][3] = { { 57, 60, 64 }, { 53, 57, 60 }, { 48, 52, 55 }, { 55, 59, 62 } };
static const int bassn[4] = { 33, 29, 36, 31 };
static const int mel[16] = { 76, 74, 72, 74, 76, 76, 76, 0, 74, 74, 74, 0, 76, 79, 79, 0 };

static void demo_init(sid *s)
{
    sid_reset(s);
    w(s, 0x17, 0xC3);                     /* res C, filter voices 1+2 */
    w(s, 0x18, 0x1F);                     /* LP, vol 15 */
    w(s, 5, 0x00); w(s, 6, 0xA6);         /* arp voice */
    w(s, 7 + 5, 0x09); w(s, 7 + 6, 0x00); /* bass */
    w(s, 14 + 5, 0x28); w(s, 14 + 6, 0x8A);
}

/* register writes of the demo "player" for frame fr */
static void demo_frame(sid *s, int fr)
{
    const int beat = 12;                   /* frames per 8th note */
    int bar = (fr / (beat * 8)) % 8;
    int ch = bar / 2;
    /* voice 1: 50 Hz arpeggio with pulse-width modulation */
    int an = chords[ch][fr % 3] + 12;
    setfreq(s, 0, note_freq(an));
    unsigned pw = 0x800 + (unsigned)(0x700 * sin(fr * 2 * M_PI / 160.0));
    setpw(s, 0, pw);
    if (fr == 0) w(s, 4, 0x41);
    /* voice 2: bass with hard restart */
    int ph = fr % beat;
    if (ph == beat - 2) { w(s, 7 + 4, 0x20); w(s, 7 + 5, 0x0F); w(s, 7 + 6, 0x00); }
    if (ph == 0) {
        int bn = bassn[ch] + ((fr / beat) % 2 ? 12 : 0);
        setfreq(s, 1, note_freq(bn));
        w(s, 7 + 5, 0x09); w(s, 7 + 6, 0x00);
        w(s, 7 + 4, 0x21);
    }
    /* voice 3: melody (triangle) */
    if (fr % (beat * 2) == 0) {
        int mn = mel[(fr / (beat * 2)) % 16];
        w(s, 14 + 4, 0x10);
        if (mn) { setfreq(s, 2, note_freq(mn)); w(s, 14 + 4, 0x11); }
    }
    /* filter sweep */
    unsigned fc = (unsigned)(0x380 + 0x300 * sin(fr * 2 * M_PI / 200.0));
    setfc(s, fc);
}

/* plays the demo; if b == NULL only clocks (used by the benchmark) */
static void play_demo(sid *s, int frames, int batch, sbuf *b)
{
    int16_t tmp[8192];
    demo_init(s);
    for (int fr = 0; fr < frames; fr++) {
        demo_frame(s, fr);
        for (int c = 0; c < FRAME; c += batch) {
            int n = FRAME - c < batch ? FRAME - c : batch;
            int k = sid_clock(s, n, tmp, 8192);
            if (b && k) sb_push(b, tmp, (size_t)k);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* 12. savestates                                                           */
/* ------------------------------------------------------------------------ */

/* continue the demo from (frame fr, `into` cycles already clocked in it) for
 * `frames_left` more frame starts, collecting everything into b */
static void demo_continue(sid *s, int fr, int into, int frames_left, sbuf *b)
{
    int16_t tmp[8192];
    int k = sid_clock(s, 0, tmp, 8192);          /* drain pending samples */
    if (k) sb_push(b, tmp, (size_t)k);
    int k2 = sid_clock(s, FRAME - into, tmp, 8192);
    if (k2) sb_push(b, tmp, (size_t)k2);
    for (int i = 1; i <= frames_left; i++) {
        demo_frame(s, fr + i);
        /* odd batch sizes, as a machine would clock between register accesses */
        for (int c = 0; c < FRAME; ) {
            int n = 1 + (c * 7 + i * 13) % 977;
            if (n > FRAME - c) n = FRAME - c;
            int kk = sid_clock(s, n, tmp, 8192);
            if (kk) sb_push(b, tmp, (size_t)kk);
            c += n;
        }
    }
}

static void test_savestate(void)
{
    printf("\n[12] Savestates: save mid-tune, continue vs. load into a fresh instance\n");
    for (int mi = 0; mi < 2; mi++) {
        sid_model m = mi ? SID_8580 : SID_6581;
        sid a, b;
        sid_init(&a, m, CLK, FS);
        sid_set_filter_curve(&a, 0.3);          /* non-default setting must travel too */
        int16_t tmp[8192];
        demo_init(&a);
        const int N_FRAMES = 137, INTO = 12345, M_FRAMES = 150;
        for (int fr = 0; fr < N_FRAMES; fr++) {
            demo_frame(&a, fr);
            sid_clock(&a, FRAME, tmp, 8192);
        }
        demo_frame(&a, N_FRAMES);
        sid_clock(&a, INTO, tmp, 100);          /* leave samples pending in the FIFO */
        int pending = sid_pending(&a);
        long n_cycles = (long)N_FRAMES * FRAME + INTO;
        size_t len = sid_state_size(&a);
        unsigned char *st = (unsigned char *)malloc(len);
        sid_state_save(&a, st);

        sbuf A = { 0 }, B = { 0 };
        demo_continue(&a, N_FRAMES, INTO, M_FRAMES, &A);

        /* fresh instance; for the 8580 case deliberately initialised as a
         * 6581 at 48 kHz to exercise re-initialisation on load */
        if (mi) sid_init(&b, SID_6581, CLK, 48000);
        else    sid_init(&b, m, CLK, FS);
        int lr = sid_state_load(&b, st, len);
        int bad = sid_state_load(&b, st, len - 1) == 0;   /* truncated must fail */
        demo_continue(&b, N_FRAMES, INTO, M_FRAMES, &B);

        int same = lr == 0 && A.n == B.n && memcmp(A.d, B.d, A.n * sizeof(int16_t)) == 0;
        size_t first_diff = 0;
        if (!same) while (first_diff < A.n && first_diff < B.n && A.d[first_diff] == B.d[first_diff]) first_diff++;
        double rms = 0;
        for (size_t i = 0; i < A.n; i++) rms += (double)A.d[i] * A.d[i];
        rms = A.n ? sqrt(rms / A.n) : 0;
        printf("    %s: N = %ld cycles, state %zu bytes (%d pending samples), M = %ld cycles -> %zu samples (RMS %.0f)%s\n",
               mname(m), n_cycles, len, pending, (long)M_FRAMES * FRAME + (FRAME - INTO), A.n, rms,
               mi ? ", loaded into a 6581/48 kHz instance" : "");
        if (!same) printf("    first difference at sample %zu (A %zu samples, B %zu samples, load %d)\n", first_diff, A.n, B.n, lr);
        check(same && !bad && pending > 0 && rms > 500,
              "%s savestate: continuation after load is bit-identical (%zu samples)", mname(m), A.n);
        free(st);
        sb_free(&A); sb_free(&B);
        sid_free(&a); sid_free(&b);
    }
}

static void test_demo(void)
{
    printf("\n[7] Musical test: 3-voice arpeggio/bass/melody, PWM, resonant LP sweep\n");
    for (int mi = 0; mi < 2; mi++) {
        sid_model m = mi ? SID_8580 : SID_6581;
        sid s;
        sid_init(&s, m, CLK, FS);
        sbuf b = { 0 };
        play_demo(&s, 500, FRAME, &b);
        char path[256];
        snprintf(path, sizeof path, OUTDIR "demo_%s.wav", mname(m));
        wav_write(path, b.d, b.n, FS);
        long clip = 0;
        double pk = 0, rms = 0;
        for (size_t i = 0; i < b.n; i++) {
            int v = b.d[i];
            if (v >= 32767 || v <= -32768) clip++;
            pk = fmax(pk, fabs((double)v));
            rms += (double)v * v;
        }
        rms = sqrt(rms / b.n);
        double secs = b.n / (double)FS;
        check(clip == 0 && rms > 1000 && fabs(secs - 500.0 * FRAME / CLK) < 0.01,
              "%s demo: %.2f s, peak %.1f dBFS, RMS %.1f dBFS, %ld clipped -> %s",
              mname(m), secs, 20 * log10(pk / 32768), 20 * log10(rms / 32768), clip, path);
        sb_free(&b);
        sid_free(&s);
    }
}

/* ------------------------------------------------------------------------ */
/* 8. speed                                                                 */
/* ------------------------------------------------------------------------ */

static void test_speed(void)
{
    printf("\n[8] Speed (demo tune, all voices + filter, 44.1 kHz output; single thread)\n");
    static const int batches[] = { FRAME, 63, 1 };
    static const char *bn[] = { "per frame (19656 cycles)", "per raster line (63)", "per cycle (1)" };
    double xrt[3];
    for (int i = 0; i < 3; i++) {
        sid s;
        sid_init(&s, SID_6581, CLK, FS);
        int frames = i == 2 ? 250 : 1000;
        double t0 = cpu_s(), w0 = now_s();
        play_demo(&s, frames, batches[i], NULL);
        double dt = cpu_s() - t0, dw = now_s() - w0;
        double cyc = (double)frames * FRAME;
        xrt[i] = cyc / CLK / dt;
        printf("    %-26s %7.2f Mcycles/s  = %6.1fx real time (CPU time; wall %.1fx)\n",
               bn[i], cyc / dt / 1e6, xrt[i], cyc / CLK / dw);
        sid_free(&s);
    }
    check(xrt[0] >= 20, "speed: %.0fx real time with batched clocking (target >= 20x)", xrt[0]);
}

/* ------------------------------------------------------------------------ */
/* 9. $D418 digi                                                            */
/* ------------------------------------------------------------------------ */

static void test_digi(void)
{
    printf("\n[9] 4-bit $D418 volume digi (DC level change on volume writes)\n");
    double lvl[2];
    for (int mi = 0; mi < 2; mi++) {
        sid_model m = mi ? SID_8580 : SID_6581;
        sid s;
        sid_init(&s, m, CLK, FS);
        sbuf b = { 0 };
        w(&s, 0x18, 0x0F);
        run(&s, sec(0.2), NULL);
        const int per = 126;                 /* ~7.8 kHz sample rate */
        long n = sec(1.0) / per;
        for (long i = 0; i < n; i++) {
            double v = 7.5 + 7.49 * sin(2 * M_PI * 1000.0 * i * per / CLK);
            w(&s, 0x18, (int)lround(v) & 15);
            run(&s, per, &b);
        }
        char path[256];
        snprintf(path, sizeof path, OUTDIR "digi_%s.wav", mname(m));
        wav_write(path, b.d, b.n, FS);
        double *x = to_double(b.d + 4410, b.n - 4410);
        lvl[mi] = db(tone_power(x, b.n - 4410, 1000.0, FS) / (32768.0 * 32768.0 / 2));
        free(x);
        sb_free(&b);
        sid_free(&s);
    }
    printf("    1 kHz digi level: 6581 %.1f dBFS, 8580 %.1f dBFS\n", lvl[0], lvl[1]);
    check(lvl[0] > -30 && lvl[0] - lvl[1] > 20, "volume-register digis audible on 6581, ~%.0f dB quieter on 8580", lvl[0] - lvl[1]);
}

/* ------------------------------------------------------------------------ */
/* 10. aliasing                                                             */
/* ------------------------------------------------------------------------ */

static double inharmonic_db(const double *x, size_t n, double f0)
{
    int N = 1;
    while ((size_t)(N * 2) <= n) N *= 2;
    double *re = (double *)malloc(N * sizeof(double)), *im = (double *)malloc(N * sizeof(double));
    for (int i = 0; i < N; i++) {
        double a = 2 * M_PI * i / (N - 1);
        double wv = 0.35875 - 0.48829 * cos(a) + 0.14128 * cos(2 * a) - 0.01168 * cos(3 * a);
        re[i] = x[i] * wv; im[i] = 0;
    }
    fft(re, im, N);
    double bw = (double)FS / N, ph = 0, pi = 0;
    for (int k = (int)(20 / bw); k < (int)(20000 / bw); k++) {
        double p = re[k] * re[k] + im[k] * im[k];
        double f = k * bw, h = f / f0;
        double dist = fabs(h - round(h)) * f0 / bw;
        if (dist < 8) ph += p; else pi += p;
    }
    free(re); free(im);
    return db(pi / ph);
}

static void test_alias(void)
{
    printf("\n[10] Resampling quality: saw at exactly 1024 cycles/period (%.2f Hz)\n", CLK / 1024);
    sid s;
    sid_init(&s, SID_6581, CLK, FS);
    sbuf b = { 0 };
    w(&s, 0x18, 0x0F);
    setfreq(&s, 0, 0x4000);
    w(&s, 5, 0x00); w(&s, 6, 0xF0);
    w(&s, 4, 0x21);
    run(&s, sec(0.2), NULL);
    /* resampled output and naive point-sampled raw chip output, same span */
    size_t nn = 0, cap = (size_t)(1.6 * FS) + 16;
    double *naive = (double *)malloc(cap * sizeof(double));
    double tnext = 0, step = CLK / FS;
    long cyc = 0, total = sec(1.6);
    int16_t tmp[64];
    for (; cyc < total; cyc++) {
        int k = sid_clock(&s, 1, tmp, 64);
        if (k) sb_push(&b, tmp, (size_t)k);
        if (cyc >= tnext && nn < cap) { naive[nn++] = sid_last_raw(&s); tnext += step; }
    }
    double *x = to_double(b.d, b.n);
    double m = 0;
    for (size_t i = 0; i < nn; i++) m += naive[i];
    for (size_t i = 0; i < nn; i++) naive[i] -= m / nn;
    double a1 = inharmonic_db(x, b.n, CLK / 1024), a0 = inharmonic_db(naive, nn, CLK / 1024);
    printf("    inharmonic (alias) energy 20 Hz-20 kHz re harmonic energy: resampler %.1f dB, naive point sampling %.1f dB\n", a1, a0);
    check(a1 < -70 && a1 < a0 - 30, "anti-aliased resampling: alias floor %.1f dB (naive %.1f dB)", a1, a0);
    free(x); free(naive);
    sb_free(&b);
    sid_free(&s);
}

/* ------------------------------------------------------------------------ */
/* 11. register reads                                                       */
/* ------------------------------------------------------------------------ */

static void test_reads(void)
{
    printf("\n[11] Register reads: POTX/POTY, OSC3, ENV3, write-only data bus fade\n");
    for (int mi = 0; mi < 2; mi++) {
        sid_model m = mi ? SID_8580 : SID_6581;
        sid s;
        sid_init(&s, m, CLK, FS);
        int px = sid_read(&s, 0x19), py = sid_read(&s, 0x1A);
        sid_set_pots(&s, 0x40, 0x80);
        int px2 = sid_read(&s, 0x19), py2 = sid_read(&s, 0x1A);
        /* OSC3 with saw at freq $0100 -> top byte advances 1 per 256 cycles */
        setfreq(&s, 2, 0x0100);
        w(&s, 0x0E + 4, 0x20);
        int prev = sid_read(&s, 0x1B), mono = 1;
        for (int i = 0; i < 300; i++) {
            run(&s, 256, NULL);
            int v = sid_read(&s, 0x1B);
            if (((v - prev) & 0xFF) != 1) mono = 0;
            prev = v;
        }
        w(&s, 0x01, 0x5A);
        int b0 = sid_read(&s, 0x00);
        run(&s, 2000, NULL);
        int b1 = sid_read(&s, 0x05);
        run(&s, 20000, NULL);
        int b2 = sid_read(&s, 0x12);
        printf("    %s: POT $%02X/$%02X (set $40/$80 -> $%02X/$%02X), OSC3 saw steps ok=%d, bus: $%02X, +2000cyc $%02X, +22000cyc $%02X\n",
               mname(m), px, py, px2, py2, mono, b0, b1, b2);
        int busok = (b0 == 0x5A && b1 == 0x5A && (m == SID_6581 ? b2 == 0 : b2 == 0x5A));
        check(px == 0xFF && py == 0xFF && px2 == 0x40 && py2 == 0x80 && mono && busok,
              "%s register reads (pots, OSC3, data-bus fade %s)", mname(m), m == SID_6581 ? "after ~7000 cycles" : "slow");
        sid_free(&s);
    }
}

static int want(int argc, char **argv, const char *name)
{
    if (argc < 2) return 1;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], name) == 0) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    double t0 = now_s();
    printf("sidtest - MOS 6581/8580 SID emulation tests (PAL %.0f Hz, %d Hz output)\n", CLK, FS);
    if (want(argc, argv, "pitch"))  { test_pitch(SID_6581); test_pitch(SID_8580); }
    if (want(argc, argv, "env"))    test_envelope();
    if (want(argc, argv, "adsr"))   test_adsr_bug();
    if (want(argc, argv, "noise"))  test_noise();
    if (want(argc, argv, "filter")) { test_filter(SID_6581); test_filter(SID_8580); }
    if (want(argc, argv, "sync"))   test_sync_ring();
    if (want(argc, argv, "demo"))   test_demo();
    if (want(argc, argv, "speed"))  test_speed();
    if (want(argc, argv, "digi"))   test_digi();
    if (want(argc, argv, "alias"))  test_alias();
    if (want(argc, argv, "reads"))  test_reads();
    if (want(argc, argv, "state"))  test_savestate();
    printf("\n%d passed, %d failed (%.1f s)\n", g_pass, g_fail, now_s() - t0);
    return g_fail > 100 ? 100 : g_fail;
}
