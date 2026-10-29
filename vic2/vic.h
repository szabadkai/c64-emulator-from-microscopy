/* vic.h - cycle-exact MOS 6569 (PAL) VIC-II, pin interface.
 *
 * Drop-in replacement for harness/src/vic.h (same names, same pin layout);
 * everything that was there still exists with the same meaning.  Additions
 * are marked "[added]" and listed in README.md.
 *
 * The VIC is a two-phase bus master, so it is ticked twice per PHI2 cycle:
 *
 *   PHI1 half  (VIC_PHI0 = 0): the VIC starts a new cycle, updates BA / AEC /
 *              IRQ for it and sets VIC_FETCH with the 14-bit address of its
 *              phase-1 access on A0..A13 (g-, p-, s-, refresh or idle access;
 *              the VIC accesses memory in EVERY phase 1).
 *              The board answers on D0..D11 (D8..D11 = colour RAM nybble).
 *   PHI2 half  (VIC_PHI0 = 1): the VIC consumes the phase-1 data, draws the
 *              cycle's 8 pixels, serves a CPU register access if VIC_CS is set
 *              (A0..A5, VIC_RW, D0..D7 in/out), and on cycles where it owns
 *              PHI2 (AEC low: badline c-access, sprite s-access) sets
 *              VIC_FETCH again.  The board answers; the data is consumed by the
 *              next PHI1 call.
 *
 *   Register-write cycles [added rule, no board change needed]: when the board
 *   overwrites D0..D11 with CPU write data in the PHI2 call, the phase-1 fetch
 *   data is lost.  The VIC detects this (VIC_CS && !VIC_RW) and re-issues its
 *   phase-1 address with VIC_FETCH in the PHI2 output; the board answers as for
 *   any PHI2 fetch and the VIC consumes it at the next PHI1.  This is exact:
 *   in such a cycle the CPU wrote to the VIC, so RAM / colour RAM / the CIA2
 *   bank bits cannot have changed, and the VIC never needs PHI2 for itself in
 *   a cycle in which the CPU owns the bus.
 *
 * Bits of control outputs are electrical levels except IRQ (1 = asserted). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VIC_A_MASK   0x3FFFULL
#define VIC_D_SHIFT  16
#define VIC_D_MASK   (0xFFFULL << VIC_D_SHIFT)
#define VIC_PHI0     (1ULL << 28)   /* in */
#define VIC_CS       (1ULL << 29)   /* in: 1 = selected */
#define VIC_RW       (1ULL << 30)   /* in: 1 = read */
#define VIC_IRQ      (1ULL << 31)   /* out: 1 = /IRQ asserted */
#define VIC_BA       (1ULL << 32)   /* out: BA level */
#define VIC_AEC      (1ULL << 33)   /* out: AEC level for this cycle's PHI2 */
#define VIC_FETCH    (1ULL << 34)   /* out: VIC drives A0..A13, wants D0..D11 */
/* [added, optional input] CPU data-bus byte of this PHI2 on bits 36..43.  Only
   used for the colour nybble of the first 3 c-accesses of a badline that
   starts in mid-line (AEC still high, the VIC sees the CPU's D0..D3 through
   the 4066 switch; Bauer 3.14.6).  Without it the VIC uses the low nybble of
   the last byte it saw in a CPU register access. */
#define VIC_CPUD     (1ULL << 35)   /* in: bits 36..43 valid */
#define VIC_CPUD_SHIFT 36

#define VIC_GET_D(p)      ((uint16_t)(((p) >> VIC_D_SHIFT) & 0xFFF))
#define VIC_SET_D(p, d)   (((p) & ~VIC_D_MASK) | ((uint64_t)((d) & 0xFFF) << VIC_D_SHIFT))

#define VIC_CYCLES_PER_LINE 63
#define VIC_LINES           312
#define VIC_FIRST_VIS_LINE  16       /* first line after vertical blank */
#define VIC_SCREEN_W        403      /* 6569: X $1E0 .. $17A (fb column 0 = X $1E0) */
#define VIC_SCREEN_H        284      /* lines 16 .. 299 */

/* [added] VICE "normal borders" PAL crop: 384 x 272, display window (X 24,
   raster 51) at (32, 35); column 0 = X $1F0, row 0 = raster 16. */
#define VIC_CROP_W 384
#define VIC_CROP_H 272

typedef struct {
    /* ---- registers -------------------------------------------------- */
    uint8_t reg[64];      /* register file as written (cycle-level logic view) */
    uint8_t preg[64];     /* [added] pixel-pipeline view of reg[] (lags by wdelay) */
    uint8_t wdelay[64];   /* [added] pixel index 0..8 inside the write cycle's 8
                             output pixels from which a write is visible in the
                             pixel pipeline (see README "write timing") */
    /* ---- beam ------------------------------------------------------- */
    int cycle;            /* 1..63 (C. Bauer's numbering) */
    int line;             /* 0..311 structural line counter (RASTER except in
                             cycle 1 of line 0, where RASTER still reads 311) */
    uint64_t frame;       /* incremented when line wraps to 0 */
    bool raster_match;    /* raster IRQ already triggered in this line */
    uint16_t raster_cmp;  /* [added] IRQ compare line ($d011.7 / $d012 written) */
    uint8_t irq_latch, irq_mask;
    /* ---- text/bitmap display logic ---------------------------------- */
    bool den_frame;       /* DEN was set in some cycle of line $30 */
    bool badline;         /* bad line condition in the current cycle */
    bool display;         /* display state (false = idle state) */
    uint16_t vc, vcbase;
    uint8_t rc;
    int vmli;
    uint8_t refresh;      /* [added] DRAM refresh counter REF */
    int ba_low;           /* consecutive cycles BA has been low (incl. current) */
    bool ba, aec;
    int fetch_kind, fetch_col;   /* pending phase-1 access (consumed in PHI2) */
    int fetch2_kind, fetch2_idx; /* [added] pending phase-2 access (next PHI1) */
    int p2_kind, p2_idx;         /* [added] phase-2 access decided in PHI1 */
    uint16_t fetch_addr;         /* [added] address of the phase-1 access */
    uint8_t cbuf[40], colbuf[40];          /* video matrix line (c-accesses) */
    uint8_t gdata[40], gchar[40], gcol[40]; /* per column graphics of this line */
    bool gidle[40];
    /* ---- graphics sequencer [added] --------------------------------- */
    bool sq_latch_full;   /* g-data latch holds an unloaded column */
    uint8_t sq_ld, sq_lc, sq_lcol; bool sq_lidle;   /* latch contents */
    uint8_t sq_d, sq_c, sq_col;  /* shift register data and its c-data */
    uint8_t sq_px;        /* pixels shifted since load (>= 8: empty) */
    /* ---- sprites [added] -------------------------------------------- */
    uint8_t mc[8], mcbase[8];
    uint8_t sptr[8];      /* p-access results */
    /* per-sprite flags as bit masks (bit n = sprite n) */
    uint8_t sdma;         /* sprite DMA on */
    uint8_t sdisp;        /* sprite display on */
    uint8_t yexp_ff;      /* "advance line" (Y expansion) flip-flop */
    uint8_t crunch;       /* MxYE cleared in cycle 15 while flip-flop clear */
    uint8_t sarmed;       /* sdata not yet shifted out (waits for X match) */
    uint32_t sfetch[8];   /* bytes being fetched for the next sprite line */
    uint32_t sdata[8];    /* 24 bits of the last complete sprite line */
    uint32_t sshift[8];   /* shifting copy */
    uint8_t spix[8];      /* pixels output since X match */
    uint8_t sactive;      /* bit mask: sprite sequencer shifting */
    uint8_t col_ss, col_sb;  /* $d01e / $d01f */
    /* ---- border ----------------------------------------------------- */
    bool vborder, mborder;
    /* ---- misc [added] ----------------------------------------------- */
    uint8_t cpu_bus;      /* last CPU data byte seen (see VIC_CPUD) */
    uint8_t lpx, lpy;     /* light pen latches (not triggered, read as 0) */
    int x0;               /* display X of the first pixel output this cycle */
    uint8_t fb[VIC_SCREEN_H][VIC_SCREEN_W];  /* palette indices */
    /* statistics of the current / last finished frame */
    uint32_t badlines, last_badlines;
    uint64_t raster_irqs; /* raster compare matches latched into $D019 */
    bool line_badline;    /* [added] current line had BA low for a badline */
} vic_t;

void     vic_init(vic_t *v);
uint64_t vic_tick(vic_t *v, uint64_t pins);
uint16_t vic_screen_addr(const vic_t *v);   /* VM base (14 bit) */
extern const uint32_t vic_palette[16];      /* 0xRRGGBB, Pepto PAL (= vic_palette_pepto) */

/* [added] */
extern const uint32_t vic_palette_pepto[16];   /* Pepto PAL ("VICE default") */
int  vic_raster(const vic_t *v);               /* RASTER as the CPU reads it */
/* framebuffer cropped to VICE "normal borders" (384 x 272), palette indices */
void vic_crop_normal(const vic_t *v, uint8_t *dst /* VIC_CROP_W * VIC_CROP_H */);
/* same, as 0xRR,0xGG,0xBB bytes; pal NULL = Pepto */
void vic_crop_normal_rgb(const vic_t *v, uint8_t *rgb /* 384*272*3 */, const uint32_t *pal);
