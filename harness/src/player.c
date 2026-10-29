/* player.c - see player.h */
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c64.h"

#define PLAYER_AUDIO_CAP 8192   /* one PAL frame is ~880 samples */

struct player {
    c64_t m;
    uint8_t *prg;
    size_t prg_len;
    bool auto_done;
    uint8_t rgb[PLAYER_W * PLAYER_H * 3];
    int16_t audio[PLAYER_AUDIO_CAP];
    int naudio;
    bool booted;          /* READY. has been on screen once */
    char *tq;             /* queued text for player_type */
    size_t tq_len, tq_pos, tq_cap;
};

static void on_samples(void *ctx, const int16_t *s, int n) {
    player_t *p = ctx;
    if (n > PLAYER_AUDIO_CAP - p->naudio) n = PLAYER_AUDIO_CAP - p->naudio;
    memcpy(p->audio + p->naudio, s, sizeof(int16_t) * (size_t)n);
    p->naudio += n;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

player_t *player_create(const char *rom_dir, const char *prg_path, int sid_model, int cpu_core,
                        char *err, size_t errlen) {
    player_t *p = calloc(1, sizeof *p);
    if (!p) { snprintf(err, errlen, "out of memory"); return NULL; }
    if (c64_init(&p->m, sid_model == 8580 ? SID_8580 : SID_6581) != 0) {
        snprintf(err, errlen, "machine init failed");
        free(p);
        return NULL;
    }
    if (cpu_core == M6510_FAST) c64_use_fast_cpu(&p->m);
    if (cpu_core == M6510_COMPILED) c64_use_compiled_cpu(&p->m);
    if (c64_load_roms(&p->m, rom_dir, err, errlen) != 0) { player_destroy(p); return NULL; }
    if (prg_path && *prg_path) {
        if (!(p->prg = read_file(prg_path, &p->prg_len))) {
            snprintf(err, errlen, "cannot read %s", prg_path);
            player_destroy(p);
            return NULL;
        }
    } else {
        p->auto_done = true;
    }
    p->m.sid.on_samples = on_samples;
    p->m.sid.on_samples_ctx = p;
    return p;
}

void player_destroy(player_t *p) {
    if (!p) return;
    c64_free(&p->m);
    free(p->prg);
    free(p->tq);
    free(p);
}

void player_run_frame(player_t *p) {
    p->naudio = 0;
    uint64_t f = p->m.vic.frame;
    while (p->m.vic.frame == f) c64_tick(&p->m);
    sidpins_sync(&p->m.sid);
    if (!p->booted && c64_screen_contains(&p->m, "READY.")) p->booted = true;
    /* queued text: typed in chunks once BASIC is up and any autostart has finished */
    if (p->booted && p->auto_done && !c64_typing(&p->m) && p->tq_pos < p->tq_len) {
        char chunk[200];
        size_t n = 0;
        while (n < sizeof chunk - 1 && p->tq_pos < p->tq_len) {
            char c = p->tq[p->tq_pos++];
            if (c == '\\') continue;      /* c64_type reads backslash-n as RETURN; the C64 has no backslash */
            chunk[n++] = c == '\r' ? '\n' : c;
        }
        chunk[n] = 0;
        c64_type(&p->m, chunk);
    }
    if (!p->auto_done && c64_screen_contains(&p->m, "READY.")) {
        char msg[256];
        uint16_t s, e;
        p->auto_done = true;
        if (c64_inject_prg(&p->m, p->prg, p->prg_len, &s, &e, msg, sizeof msg) == 0)
            c64_type(&p->m, "RUN\\n");
        else
            fprintf(stderr, "%s\n", msg);
    }
    vic_crop_normal_rgb(&p->m.vic, p->rgb, vic_palette);
}

const uint8_t *player_frame_rgb(const player_t *p) { return p->rgb; }

const int16_t *player_audio(const player_t *p, int *n) {
    *n = p->naudio;
    return p->audio;
}

uint64_t player_frame_number(const player_t *p) { return p->m.vic.frame; }

bool player_autostarted(const player_t *p) { return p->auto_done && !c64_typing(&p->m); }

void player_key(player_t *p, int col, int row, bool down) { c64_key(&p->m, col, row, down); }

void player_restore(player_t *p, bool down) { p->m.restore = down; }

void player_type(player_t *p, const char *text, size_t n) {
    if (p->tq_len + n > p->tq_cap) {
        size_t cap = (p->tq_len + n) * 2 + 256;
        char *b = realloc(p->tq, cap);
        if (!b) return;
        p->tq = b;
        p->tq_cap = cap;
    }
    memcpy(p->tq + p->tq_len, text, n);
    p->tq_len += n;
}

size_t player_type_pending(const player_t *p) {
    return p->tq_len - p->tq_pos + (c64_typing(&p->m) ? 1 : 0);
}

int player_char_key(int ch, int *col, int *row, int *shift) { return c64_char_key(ch, col, row, shift); }

void player_screen_text(player_t *p, char *out) {
    char t[25][41];
    c64_screen_text(&p->m, t);
    memcpy(out, t, sizeof t);
}
