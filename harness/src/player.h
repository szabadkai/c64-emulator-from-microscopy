/* player.h - frame-at-a-time API for interactive front ends (app/C64Player.swift).
 *
 *     player_t *p = player_create("roms", "intro.prg", 6581, err, sizeof err);
 *     loop: player_run_frame(p);
 *           player_frame_rgb(p)   -> 384x272 RGB (VICE "normal borders" crop)
 *           player_audio(p, &n)   -> the n 44.1 kHz samples produced by that frame
 *
 * The PRG (optional) is autostarted like --prg: injected once READY. shows,
 * then RUN<RETURN> is typed through the keyboard matrix.
 *
 * Keyboard: player_key() presses matrix positions directly (live keys);
 * player_type() queues text (e.g. piped stdin) that is typed through the
 * matrix once BASIC is up, like --type. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLAYER_W 384
#define PLAYER_H 272

typedef struct player player_t;

/* cpu_core: 0 = transistor netlist, 1 = fast behavioural, 2 = compiled netlist */
player_t      *player_create(const char *rom_dir, const char *prg_path, int sid_model, int cpu_core,
                             char *err, size_t errlen);
void           player_destroy(player_t *p);
void           player_run_frame(player_t *p);
const uint8_t *player_frame_rgb(const player_t *p);          /* PLAYER_W * PLAYER_H * 3 */
const int16_t *player_audio(const player_t *p, int *n);
uint64_t       player_frame_number(const player_t *p);
bool           player_autostarted(const player_t *p);
void           player_key(player_t *p, int col, int row, bool down);   /* C64 matrix PA col, PB row */
void           player_restore(player_t *p, bool down);                 /* RESTORE key (-> /NMI) */
void           player_type(player_t *p, const char *text, size_t n);   /* queue text; \n = RETURN */
size_t         player_type_pending(const player_t *p);
int            player_char_key(int ch, int *col, int *row, int *shift);  /* ASCII -> matrix, -1 if none */
void           player_screen_text(player_t *p, char *out /* 25 * 41, rows NUL-terminated */);
