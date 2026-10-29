/* sidpins.h - pin-level wrapper around the SID model in ../sid (sid.h).
 *
 *     pins = sidpins_tick(&sp, pins);   once per PHI2 cycle
 *
 * In: A0..A4, D0..D7, SIDP_CS (1 = selected), SIDP_RW (1 = read).
 * Out: D0..D7 on reads.
 * The chip itself is clocked lazily: before every register access it is
 * clocked through all cycles that precede the access, so writes and reads
 * land on their exact cycle; sidpins_sync() (called by the board at every
 * frame end and before audio is flushed) clocks it up to "now" and hands the
 * produced 44.1 kHz samples to the optional WAV sink. */
#pragma once
#include <stdint.h>
#include "sid.h"
#include "wav.h"

#define SIDP_A_MASK   0x1FULL
#define SIDP_D_SHIFT  16
#define SIDP_D_MASK   (0xFFULL << SIDP_D_SHIFT)
#define SIDP_CS       (1ULL << 24)
#define SIDP_RW       (1ULL << 25)

typedef struct {
    sid chip;
    uint64_t now;        /* PHI2 cycles ticked so far */
    uint64_t clocked;    /* cycles the chip has been clocked through */
    uint64_t writes, reads, samples;
    /* audio sink - not machine state (not saved) */
    int16_t *buf;
    int cap;
    wav_writer *wav;
    void (*on_samples)(void *ctx, const int16_t *samples, int n);   /* live audio */
    void *on_samples_ctx;
} sidpins_t;

int      sidpins_init(sidpins_t *sp, sid_model model);   /* 0 = ok */
void     sidpins_free(sidpins_t *sp);
void     sidpins_reset(sidpins_t *sp);                   /* /RES */
uint64_t sidpins_tick(sidpins_t *sp, uint64_t pins);
void     sidpins_sync(sidpins_t *sp);                    /* clock up to now, flush audio */
