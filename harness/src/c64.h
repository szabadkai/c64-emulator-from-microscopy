/* c64.h - the C64 "board": wires the chips together through their pin masks.
 * See c64.c for the per-cycle pin flow. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "cia.h"
#include "pla.h"
#include "port6510.h"
#include "sidpins.h"
#include "vic.h"          /* ../vic2/vic.h: cycle-exact 6569 */

#define C64_CYCLES_PER_FRAME (VIC_CYCLES_PER_LINE * VIC_LINES)   /* 19656 */
#define C64_PAL_HZ 985248
#define C64_MAX_WATCH 8

typedef struct {
    uint32_t cpu_stalled;   /* RDY (=BA) low while the CPU was in a read cycle */
    uint32_t ba_low;        /* cycles with BA low */
    uint32_t vic_stolen;    /* cycles where the VIC owned PHI2 (AEC low) */
    uint32_t badlines;
    uint32_t irq_cycles;    /* cycles with /IRQ asserted */
    uint32_t irqs, nmis;    /* interrupt sequences taken (vector fetches) */
    uint32_t sid_writes;
} c64_frame_stats_t;

typedef struct {
    m6510_t cpu;
    vic_t vic;
    cia_t cia1, cia2;
    sidpins_t sid;
    uint8_t ram[65536];
    uint8_t color[1024];          /* 4-bit colour RAM */
    uint8_t kernal[8192], basic[8192], chargen[4096];

    uint64_t cpu_pins, vic_pins, cia1_pins, cia2_pins;
    uint8_t open_bus;             /* last byte seen on the bus by the VIC */
    int reset_cycles;             /* /RES held low for this many more cycles */
    bool restore;                 /* RESTORE key -> /NMI */
    bool nmi_prev;
    uint8_t keys[8];              /* keyboard matrix: keys[column PA] bit row PB */

    uint64_t cycle;
    c64_frame_stats_t fcur, flast;
    uint64_t total_stalled, total_stolen;
    uint64_t irqs_taken, nmis_taken;
    int64_t first_irq_cycle, first_nmi_cycle;
    uint16_t last_addr;
    bool last_was_push;

    /* opcode-fetch watch points (first SYNC at the address) */
    int nwatch;
    uint16_t watch_addr[C64_MAX_WATCH];
    int64_t watch_cycle[C64_MAX_WATCH];
    uint64_t watch_frame[C64_MAX_WATCH];
    int watch_line[C64_MAX_WATCH], watch_lcycle[C64_MAX_WATCH];

    /* keystroke injection */
    char type_buf[256];
    int type_pos, type_len, type_phase, type_timer;

    /* ---- run-time sinks: not machine state, kept across state loads ---- */
    long trace_left;
    FILE *trace_out;
    FILE *sid_log;
} c64_t;

int  c64_init(c64_t *m, sid_model model);    /* 0 = ok */
void c64_free(c64_t *m);
/* switch CPU core: call right after c64_init, before the first tick */
void c64_use_fast_cpu(c64_t *m);
void c64_use_compiled_cpu(c64_t *m);
/* returns 0 on success; err gets a message listing what was searched */
int  c64_load_roms(c64_t *m, const char *dir, char *err, size_t errlen);
void c64_tick(c64_t *m);
void c64_run_cycles(c64_t *m, uint64_t n);
void c64_run_frames(c64_t *m, uint64_t n);

uint8_t c64_vic_read(c64_t *m, uint16_t va, uint8_t *color);  /* VIC's view of memory */
void c64_screen_text(c64_t *m, char out[25][41]);
bool c64_screen_contains(c64_t *m, const char *s);

void c64_key(c64_t *m, int col, int row, bool down);
/* matrix position of an ASCII character (+ whether SHIFT is needed); -1 if none */
int  c64_char_key(int ch, int *col, int *row, int *shift);
void c64_type(c64_t *m, const char *text);   /* queued, \n = RETURN */
bool c64_typing(const c64_t *m);

/* autostart.c */
int  c64_inject_prg(c64_t *m, const uint8_t *prg, size_t len, uint16_t *start, uint16_t *end,
                    char *msg, size_t msglen);
int  c64_install_psid(c64_t *m, const uint8_t *file, size_t len, int song,
                      char *msg, size_t msglen);

/* state.c - complete machine state incl. netlist, chips, cycle counter */
int  c64_save_state(c64_t *m, const char *path, char *msg, size_t msglen);
int  c64_load_state(c64_t *m, const char *path, char *msg, size_t msglen);
uint64_t c64_state_hash(c64_t *m);   /* FNV-1a over RAM, chips, netlist, SID */
