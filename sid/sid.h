/*
 * sid.h - cycle-clocked MOS 6581 / 8580 SID emulation (C99, no dependencies)
 *
 * Own implementation, written from public documentation (see README.md).
 *
 * Usage (typical machine integration):
 *
 *     sid s;
 *     sid_init(&s, SID_6581, 985248, 44100);    // PAL clock, 44.1 kHz output
 *     ...
 *     // before every SID register access at machine cycle T:
 *     n += sid_clock(&s, T - last_sid_cycle, buf + n, cap - n);
 *     last_sid_cycle = T;
 *     sid_write(&s, reg, val);   or   v = sid_read(&s, reg);
 *     ...
 *     // at the end of each frame: clock the remainder, hand buf to audio.
 *     sid_free(&s);
 *
 * sid_clock() may also be called with cycles == 1 once per PHI2 cycle,
 * but batching is considerably faster (see README "Integration notes").
 */
#ifndef SID_H
#define SID_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SID_6581 = 6581, SID_8580 = 8580 } sid_model;

#define SID_PAL_CLOCK  985248
#define SID_NTSC_CLOCK 1022727

struct sid_tables;   /* big read-only tables + resampler, heap allocated */

typedef struct sid_voice {
    /* oscillator / waveform generator */
    uint32_t acc;        /* 24-bit phase accumulator                      */
    uint32_t freq;       /* 16-bit frequency register                     */
    uint32_t pw;         /* 12-bit pulse width                            */
    uint32_t ctrl;       /* control register ($04)                        */
    uint32_t lfsr;       /* 23-bit noise shift register                   */
    uint32_t noise_out;  /* 12-bit noise waveform (bits 11..4)            */
    uint32_t float_out;  /* value held by the floating waveform DAC input */
    int32_t  float_ttl;  /* cycles until the floating value leaks away    */
    uint32_t last_wave;  /* last 12-bit waveform output                   */
    /* envelope generator */
    uint32_t ad, sr;
    uint32_t env;        /* 8-bit envelope counter                        */
    uint32_t state;      /* 0 attack, 1 decay/sustain, 2 release          */
    uint32_t hold_zero;  /* envelope frozen at zero                       */
    uint32_t rate_cnt;   /* 15-bit rate counter                           */
    uint32_t rate_per;   /* current rate period (compare value)           */
    uint32_t exp_cnt;    /* exponential (decay curve) counter             */
    uint32_t exp_per;    /* exponential counter period                    */
} sid_voice;

typedef struct sid {
    sid_model model;
    double clock_hz, sample_rate;
    sid_voice v[3];
    uint8_t  regs[32];        /* shadow of written registers               */
    /* filter / mixer registers */
    uint32_t fc;              /* 11-bit cutoff                              */
    uint32_t res_filt, mode_vol;
    /* derived float mixer controls */
    float route_f[3], route_d[3];   /* voice -> filter / direct gains      */
    float mode_lp, mode_bp, mode_hp, vol;
    float w_fc;                     /* SVF frequency coefficient             */
    float q_inv;                    /* 1/Q                                   */
    /* analog state */
    float hp, bp, lp;               /* state-variable filter                 */
    float ext_lp;                   /* C64 output stage low-pass (per cycle) */
    float ext_lp_a;
    double hp_x, hp_y, hp_a;        /* C64 output stage AC coupling (per sample) */
    float last_raw;                 /* last per-cycle chip output (debug)    */
    /* model constants */
    float wave_zero, mixer_dc, sat_level;
    int32_t float_ttl_init;
    uint32_t bus_ttl;
    float filter_curve;             /* 6581 curve position 0..1 (0.5 = default) */
    /* data bus / pots */
    uint8_t bus_value;
    uint64_t bus_time;
    uint8_t potx, poty;
    uint64_t cycle;                 /* total cycles clocked                  */
    struct sid_tables *t;
} sid;

/* Lifecycle. sid_init allocates tables (about 150 KB). Returns 0 on success. */
int  sid_init(sid *s, sid_model model, double clock_hz, double sample_rate);
void sid_reset(sid *s);
void sid_free(sid *s);

/* Register access; reg is masked to 0..31. */
void    sid_write(sid *s, int reg, uint8_t val);
uint8_t sid_read(sid *s, int reg);

/* Advance the chip by `cycles` PHI2 cycles. Up to `max_samples` 16-bit mono
 * samples are written to `out` (out may be NULL / max_samples 0 to discard
 * nothing: samples that do not fit are queued (up to 4096) and returned by
 * the next call). Returns the number of samples written. */
int sid_clock(sid *s, int cycles, int16_t *out, int max_samples);

/* Number of samples currently queued inside the chip (not yet returned). */
int sid_pending(const sid *s);
/* Drop queued samples. */
void sid_drop_pending(sid *s);

/* Options */
void sid_set_pots(sid *s, uint8_t potx, uint8_t poty);   /* default $FF,$FF */
/* 6581 only: filter curve position 0..1; 0 = bright (higher cutoffs),
 * 1 = dark; default 0.5 (chip-to-chip variation of the VCR threshold). */
void sid_set_filter_curve(sid *s, double pos);

/* ---- savestates ----
 * The complete dynamic state (voices, envelopes, filter, output-stage
 * filters, resampler/decimator history and phase, data-bus fade, pending
 * output samples, filter-curve setting) in a native-endian, build-specific
 * format. sid_state_size() depends on the number of pending samples, so call
 * it right before sid_state_save(). sid_state_load() needs an instance that
 * has been through sid_init(); if the saved model / clock / sample rate
 * differ, the instance is re-initialised to the saved configuration.
 * Returns 0 on success, -1 on a bad/truncated buffer or allocation failure. */
size_t sid_state_size(const sid *s);
void   sid_state_save(const sid *s, void *buf);
int    sid_state_load(sid *s, const void *buf, size_t len);

/* ---- test / debug hooks (used by sidtest) ---- */
uint32_t sid_lfsr_next(uint32_t lfsr);            /* one noise LFSR shift   */
double   sid_cutoff_hz(const sid *s, int fc);       /* model cutoff for FC    */
float    sid_last_raw(const sid *s);                /* last chip-rate output  */
double   sid_output_scale(void);                    /* raw units -> int16     */

#ifdef __cplusplus
}
#endif
#endif
