/* sidpins.c - pin wrapper for the SID model (../sid/sid.c), see sidpins.h. */
#include "sidpins.h"
#include <stdlib.h>
#include <string.h>

#define SIDP_BUF 8192   /* >= 65536/22 + 2 samples per clock chunk */

int sidpins_init(sidpins_t *sp, sid_model model) {
    memset(sp, 0, sizeof(*sp));
    if (sid_init(&sp->chip, model, (double)SID_PAL_CLOCK, 44100.0) != 0) return -1;
    sp->cap = SIDP_BUF;
    sp->buf = malloc(sizeof(int16_t) * SIDP_BUF);
    return sp->buf ? 0 : -1;
}

void sidpins_free(sidpins_t *sp) {
    sid_free(&sp->chip);
    free(sp->buf);
    sp->buf = NULL;
}

/* clock the chip through 'n' cycles, passing samples to the sink */
static void clock_chip(sidpins_t *sp, uint64_t n) {
    while (n > 0) {
        int chunk = n > 65536 ? 65536 : (int)n;
        int k = sid_clock(&sp->chip, chunk, sp->buf, sp->cap);
        if (k > 0) {
            sp->samples += (uint64_t)k;
            if (sp->wav) wav_append(sp->wav, sp->buf, (size_t)k);
            if (sp->on_samples) sp->on_samples(sp->on_samples_ctx, sp->buf, k);
        }
        sp->clocked += (uint64_t)chunk;
        n -= (uint64_t)chunk;
    }
    while (sid_pending(&sp->chip) > 0) {   /* drain anything queued internally */
        int k = sid_clock(&sp->chip, 0, sp->buf, sp->cap);
        if (k <= 0) break;
        sp->samples += (uint64_t)k;
        if (sp->wav) wav_append(sp->wav, sp->buf, (size_t)k);
        if (sp->on_samples) sp->on_samples(sp->on_samples_ctx, sp->buf, k);
    }
}

void sidpins_sync(sidpins_t *sp) {
    if (sp->now > sp->clocked) clock_chip(sp, sp->now - sp->clocked);
}

void sidpins_reset(sidpins_t *sp) {
    sidpins_sync(sp);
    sid_reset(&sp->chip);
}

uint64_t sidpins_tick(sidpins_t *sp, uint64_t pins) {
    if (pins & SIDP_CS) {
        sidpins_sync(sp);   /* the chip has now run all cycles before this one */
        int r = (int)(pins & SIDP_A_MASK);
        if (pins & SIDP_RW) {
            uint8_t v = sid_read(&sp->chip, r);
            pins = (pins & ~SIDP_D_MASK) | ((uint64_t)v << SIDP_D_SHIFT);
            sp->reads++;
        } else {
            sid_write(&sp->chip, r, (uint8_t)((pins >> SIDP_D_SHIFT) & 0xFF));
            sp->writes++;
        }
    }
    sp->now++;
    return pins;
}
