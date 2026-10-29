/* vic.c - cycle-exact MOS 6569 (PAL) VIC-II.
 *
 * Written from C. Bauer, "The MOS 6567/6569 video controller (VIC-II) and its
 * application in the Commodore 64" (revision 2024-09-29); section numbers in
 * the comments refer to that article.  See README.md for the per-cycle table
 * and for the choices made where the article is silent.
 *
 * Coordinates
 *   cycle  1..63, line 0..311.  Cycle n starts at X counter value
 *          Xd(n) = $194 + 8(n-1)  (mod $1F8)          (3.4, 3.6.3)
 *   The video output lags the X counter by 8 pixels (3.6.3: "the border ...
 *   approx. 8 pixels later than shown in the Graph. line"; 3.6.1: g-data is
 *   displayed 12 pixels after its g-access).  So the 8 pixels emitted during
 *   cycle n are the sprite-coordinate X positions Xd(n)-8 .. Xd(n)-1 ("display
 *   X").  Everything in the pixel pipeline (sequencer, sprites, border
 *   comparators) works in display X, so it is mutually consistent: a sprite
 *   at X=24 lines up with the first character column and with the CSEL=1
 *   left border edge.
 */
#include "vic.h"
#include <string.h>

/* Pepto's PAL palette ("Calculating the color palette of the VIC II",
   www.pepto.de/projects/colorvic/, the classic 2001 table used by VICE as
   "pepto-pal") */
const uint32_t vic_palette_pepto[16] = {
    0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2, 0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
    0x6F4F25, 0x433900, 0x9A6759, 0x444444, 0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
};
const uint32_t vic_palette[16] = {
    0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2, 0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
    0x6F4F25, 0x433900, 0x9A6759, 0x444444, 0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
};

/* access kinds */
enum { F_NONE = 0, F_G, F_P, F_S1, F_C, F_S0, F_S2, F_REFETCH, F_REFRESH, F_IDLE };

#define XMAX 0x1F8   /* 504 X positions per line on the 6569 */

/* ------------------------------------------------------------- tables */
static int8_t  tab_p[64];      /* sprite whose p-access is in phase 1 of cycle (and s0 in phase 2) */
static int8_t  tab_s1[64];     /* sprite whose s-access #1 is in phase 1 (and #2 in phase 2) */
static uint8_t tab_ba[64];     /* sprites whose DMA pulls BA low in this cycle */
static int16_t tab_x0[64];     /* display X of the first pixel emitted in cycle */
static int16_t tab_fx[XMAX];   /* display X -> framebuffer column, -1 = not visible */
static bool tabs_ready;

static const int spr_pcycle[8] = { 58, 60, 62, 1, 3, 5, 7, 9 };   /* 3.6.3 */

static int wrapc(int c) { while (c < 1) c += 63; while (c > 63) c -= 63; return c; }

static void init_tables(void) {
    if (tabs_ready) return;
    for (int c = 0; c < 64; c++) { tab_p[c] = tab_s1[c] = -1; tab_ba[c] = 0; }
    for (int n = 0; n < 8; n++) {
        int p = spr_pcycle[n];
        tab_p[p] = (int8_t)n;
        tab_s1[wrapc(p + 1)] = (int8_t)n;
        /* BA low 3 cycles before the first s-access (phase 2 of p) through
           the last one (phase 2 of p+1)  (2.4.3, 3.8.1) */
        for (int c = p - 3; c <= p + 1; c++) tab_ba[wrapc(c)] |= (uint8_t)(1 << n);
    }
    for (int c = 1; c <= 63; c++) {
        int xd = (0x194 + 8 * (c - 1)) % XMAX;
        tab_x0[c] = (int16_t)((xd - 8 + XMAX) % XMAX);
    }
    for (int x = 0; x < XMAX; x++) {
        int fx = x >= 0x1E0 ? x - 0x1E0 : x + (XMAX - 0x1E0);
        tab_fx[x] = (int16_t)(fx < VIC_SCREEN_W ? fx : -1);
    }
    tabs_ready = true;
}

/* -------------------------------------------------------------- basics */
void vic_init(vic_t *v) {
    init_tables();
    memset(v, 0, sizeof(*v));
    v->cycle = VIC_CYCLES_PER_LINE;
    v->line = VIC_LINES - 1;
    v->ba = v->aec = true;
    v->vborder = v->mborder = true;
    v->refresh = 0xFF;
    v->sq_px = 8;
    v->cpu_bus = 0xFF;
    /* write delays (README "write timing"): colour registers change in the
       middle of the write cycle's output, everything else from the next
       cycle's first pixel on */
    for (int r = 0; r < 64; r++) v->wdelay[r] = 8;
    for (int r = 0x20; r <= 0x2E; r++) v->wdelay[r] = 4;
    v->wdelay[0x1E] = v->wdelay[0x1F] = 4;   /* collision reads */
}

uint16_t vic_screen_addr(const vic_t *v) { return (uint16_t)((v->reg[0x18] & 0xF0) << 6); }

int vic_raster(const vic_t *v) {
    /* 3.6.3: in line 0, RASTER is reset one cycle later than usual */
    return (v->line == 0 && v->cycle == 1) ? VIC_LINES - 1 : v->line;
}

static inline uint64_t out_irq(const vic_t *v, uint64_t pins) {
    if (v->irq_latch & v->irq_mask & 0x0F) return pins | VIC_IRQ;
    return pins & ~VIC_IRQ;
}

static void raster_check(vic_t *v) {
    /* 3.12: compare in cycle 1 (line 0: cycle 2) and on writes to $d011/$d012,
       at most one IRQ per raster line */
    if (!v->raster_match && vic_raster(v) == v->raster_cmp) {
        v->irq_latch |= 0x01;
        v->raster_match = true;
        v->raster_irqs++;
    }
}

/* ---------------------------------------------------- register access */
static uint8_t reg_read(vic_t *v, int r) {
    switch (r) {
    case 0x11: return (uint8_t)((v->reg[0x11] & 0x7F) | ((vic_raster(v) & 0x100) >> 1));
    case 0x12: return (uint8_t)(vic_raster(v) & 0xFF);
    case 0x13: return v->lpx;
    case 0x14: return v->lpy;
    case 0x16: return v->reg[0x16] | 0xC0;
    case 0x18: return v->reg[0x18] | 0x01;
    case 0x19:
        return (uint8_t)((v->irq_latch & 0x0F) | 0x70 |
                         ((v->irq_latch & v->irq_mask & 0x0F) ? 0x80 : 0));
    case 0x1A: return v->irq_mask | 0xF0;
    case 0x1E: { uint8_t t = v->col_ss; v->col_ss = 0; return t; }   /* cleared by reading */
    case 0x1F: { uint8_t t = v->col_sb; v->col_sb = 0; return t; }
    default:
        if (r >= 0x20 && r <= 0x2E) return (uint8_t)(v->reg[r] | 0xF0);
        if (r >= 0x2F) return 0xFF;
        return v->reg[r];
    }
}

static void reg_write(vic_t *v, int r, uint8_t d) {
    switch (r) {
    case 0x11:
        v->reg[0x11] = d;
        v->raster_cmp = (uint16_t)((v->raster_cmp & 0xFF) | ((d & 0x80) << 1));
        if (v->line == 0x30 && (d & 0x10)) v->den_frame = true;   /* 3.5 / 3.10 */
        raster_check(v);
        break;
    case 0x12:
        v->reg[0x12] = d;
        v->raster_cmp = (uint16_t)((v->raster_cmp & 0x100) | d);
        raster_check(v);
        break;
    case 0x13: case 0x14: break;             /* light pen latches: read only */
    case 0x17: {
        uint8_t cleared = (uint8_t)(v->reg[0x17] & ~d);
        /* 3.8.1 rule 7a: MxYE cleared in cycle 15 while the flip-flop was clear */
        if (v->cycle == 15) v->crunch |= (uint8_t)(cleared & ~v->yexp_ff);
        v->yexp_ff |= (uint8_t)~d;           /* rule 1 */
        v->reg[0x17] = d;
        break;
    }
    case 0x19: v->irq_latch &= (uint8_t)~(d & 0x0F); break;
    case 0x1A: v->irq_mask = d & 0x0F; break;
    case 0x1E: case 0x1F: break;             /* not writable */
    default:
        if (r <= 0x2E) v->reg[r] = (r >= 0x20) ? (uint8_t)(d & 0x0F) : d;
        break;                               /* $d02f-$d03f: ignored */
    }
}

/* ----------------------------------------------------- pixel pipeline */
static void emit(vic_t *v, int i0, int i1) {
    if (i0 >= i1) return;
    const uint8_t *pr = v->preg;
    uint8_t *row = (v->line >= VIC_FIRST_VIS_LINE && v->line < VIC_FIRST_VIS_LINE + VIC_SCREEN_H)
                       ? v->fb[v->line - VIC_FIRST_VIS_LINE] : NULL;
    const uint8_t c11 = pr[0x11], c16 = pr[0x16];
    const int left = (c16 & 8) ? 24 : 31, right = (c16 & 8) ? 344 : 335;     /* 3.9 */
    const int top = (c11 & 8) ? 51 : 55, bottom = (c11 & 8) ? 251 : 247;
    const int xs = c16 & 7;
    const int mode = ((c11 & 0x60) >> 4) | ((c16 & 0x10) >> 4);              /* ECM BMM MCM */
    const uint8_t border = pr[0x20] & 15;
    const uint8_t bgc[4] = { (uint8_t)(pr[0x21] & 15), (uint8_t)(pr[0x22] & 15),
                             (uint8_t)(pr[0x23] & 15), (uint8_t)(pr[0x24] & 15) };
    int x = v->x0 + i0;
    if (x >= XMAX) x -= XMAX;

    /* sprites whose X coordinate is reached inside this run of pixels */
    uint8_t start[8] = { 0 };
    uint8_t cand = v->sdisp & v->sarmed;
    if (cand) {
        for (int s = 0; s < 8; s++) {
            if (!(cand & (1 << s))) continue;
            int sx = pr[2 * s] | (((pr[0x10] >> s) & 1) << 8);
            if (sx >= XMAX) continue;            /* $1f8-$1ff never reached (3.8.1) */
            int d = sx - x;
            if (d < 0) d += XMAX;
            if (d < i1 - i0) start[d] |= (uint8_t)(1 << s);
        }
    }
    const uint8_t xe = pr[0x1D], smc = pr[0x1C], prio = pr[0x1B];

    for (int i = i0; i < i1; i++) {
        /* ---- border unit (3.9) */
        if (x == right) v->mborder = true;
        else if (x == left) {
            if (v->line == bottom) v->vborder = true;
            else if (v->line == top && (c11 & 0x10)) v->vborder = false;
            if (!v->vborder) v->mborder = false;
        }

        /* ---- graphics sequencer (3.7.3): the g-data of column k is offered
           at display X 24+8k and loaded into the shift register at the pixel
           whose X & 7 == XSCROLL */
        if ((x & 7) == 0) {
            int k = (x - 24) >> 3;
            if (x >= 24 && k < 40) {
                v->sq_ld = v->gdata[k];
                v->sq_lc = v->gchar[k];
                v->sq_lcol = v->gcol[k];
                v->sq_latch_full = true;
            } else if (k == 40) {
                v->sq_latch_full = false;
            }
        }
        if ((x & 7) == xs && v->sq_latch_full) {
            v->sq_d = v->sq_ld;
            v->sq_c = v->sq_lc;
            v->sq_col = v->sq_lcol;
            v->sq_px = 0;
            v->sq_latch_full = false;
        }
        uint8_t gc;
        bool fg;
        {
            const int px = v->sq_px;
            const uint8_t d = v->sq_d;
            const int bit = px < 8 ? (d >> (7 - px)) & 1 : 0;
            const int pair = px < 8 ? (d >> (6 - (px & 6))) & 3 : 0;
            const uint8_t col = v->sq_col, ch = v->sq_c;
            switch (mode) {
            case 0:  /* standard text */
                fg = bit; gc = bit ? col : bgc[0]; break;
            case 1:  /* multicolor text */
                if (col & 8) {
                    fg = pair >> 1;
                    gc = pair == 3 ? (col & 7) : bgc[pair];
                } else { fg = bit; gc = bit ? (col & 7) : bgc[0]; }
                break;
            case 2:  /* standard bitmap */
                fg = bit; gc = bit ? (ch >> 4) : (ch & 15); break;
            case 3:  /* multicolor bitmap */
                fg = pair >> 1;
                gc = pair == 0 ? bgc[0] : pair == 1 ? (ch >> 4) : pair == 2 ? (ch & 15) : col;
                break;
            case 4:  /* ECM text */
                fg = bit; gc = bit ? col : bgc[ch >> 6]; break;
            case 5:  /* invalid text */
                fg = (col & 8) ? (pair >> 1) : bit; gc = 0; break;
            case 6:  /* invalid bitmap 1 */
                fg = bit; gc = 0; break;
            default: /* invalid bitmap 2 */
                fg = pair >> 1; gc = 0; break;
            }
            if (px < 8) v->sq_px++;
            if (v->vborder) { gc = bgc[0]; fg = false; }  /* sequencer output off */
        }

        /* ---- sprites (3.8) */
        uint8_t act = v->sactive | start[i - i0];
        uint8_t c = gc;
        if (act) {
            uint8_t smask = 0, scol = 0, sprio = 0;
            for (int s = 0; s < 8; s++) {
                const uint8_t b = (uint8_t)(1 << s);
                if (!(act & b)) continue;
                if (start[i - i0] & b) {
                    v->sshift[s] = v->sdata[s];
                    v->spix[s] = 0;
                    v->sactive |= b;
                    v->sarmed &= (uint8_t)~b;
                }
                int bi = (xe & b) ? v->spix[s] >> 1 : v->spix[s];
                if (bi >= 24) { v->sactive &= (uint8_t)~b; continue; }
                uint8_t pc = 0xFF;
                if (smc & b) {
                    int pair = (v->sshift[s] >> (22 - (bi & ~1))) & 3;
                    if (pair == 1) pc = pr[0x25] & 15;
                    else if (pair == 2) pc = pr[0x27 + s] & 15;
                    else if (pair == 3) pc = pr[0x26] & 15;
                } else if ((v->sshift[s] >> (23 - bi)) & 1) {
                    pc = pr[0x27 + s] & 15;
                }
                v->spix[s]++;
                if (pc != 0xFF) {
                    if (!smask) { scol = pc; sprio = prio & b; }
                    smask |= b;
                }
            }
            if (smask) {
                if (smask & (smask - 1)) {              /* sprite-sprite */
                    if (!v->col_ss) v->irq_latch |= 0x04;
                    v->col_ss |= smask;
                }
                if (fg) {                               /* sprite-data */
                    if (!v->col_sb) v->irq_latch |= 0x02;
                    v->col_sb |= smask;
                }
                if (!(sprio && fg)) c = scol;
            }
        }
        if (v->mborder) c = border;
        if (row) {
            int fx = tab_fx[x];
            if (fx >= 0) row[fx] = c;
        }
        if (++x == XMAX) x = 0;
    }
}

/* -------------------------------------------------------- memory data */
static void consume_phase1(vic_t *v, uint16_t d) {
    switch (v->fetch_kind) {
    case F_G:  v->gdata[v->fetch_col] = (uint8_t)d; break;
    case F_P:  v->sptr[v->fetch_col] = (uint8_t)d; break;
    case F_S1: v->sfetch[v->fetch_col] |= (uint32_t)(d & 0xFF) << 8; break;
    default: break;
    }
    v->fetch_kind = F_NONE;
}

static void sprite_line_done(vic_t *v, int n) {
    v->sdata[n] = v->sfetch[n];
    v->sarmed |= (uint8_t)(1 << n);
}

/* ------------------------------------------------------------- PHI1 */
static uint64_t phi1(vic_t *v, uint64_t pins) {
    /* data of the previous phase-2 access */
    if (v->fetch2_kind != F_NONE) {
        uint16_t d = VIC_GET_D(pins);
        int n = v->fetch2_idx;
        switch (v->fetch2_kind) {
        case F_C:  v->cbuf[n] = (uint8_t)d; v->colbuf[n] = (uint8_t)((d >> 8) & 15); break;
        case F_S0: v->sfetch[n] = (uint32_t)(d & 0xFF) << 16; break;
        case F_S2: v->sfetch[n] |= d & 0xFF; sprite_line_done(v, n); break;
        case F_REFETCH: consume_phase1(v, d); break;
        default: break;
        }
        v->fetch2_kind = F_NONE;
    }
    v->fetch_kind = F_NONE;

    /* advance the beam */
    if (++v->cycle > VIC_CYCLES_PER_LINE) {
        v->cycle = 1;
        if (++v->line >= VIC_LINES) {
            v->line = 0;
            v->frame++;
            v->vcbase = 0;             /* 3.7.2 rule 1 */
            v->den_frame = false;
            v->refresh = 0xFF;         /* 3.13 */
            v->last_badlines = v->badlines;
            v->badlines = 0;
        }
        v->line_badline = false;
    }
    const int cyc = v->cycle, line = v->line;
    v->x0 = tab_x0[cyc];

    /* raster counter changes: cycle 1, or cycle 2 in line 0 (3.6.3, 3.12) */
    if ((cyc == 1 && line != 0) || (cyc == 2 && line == 0)) {
        v->raster_match = false;
        raster_check(v);
    }

    const uint8_t c11 = v->reg[0x11];
    if (line == 0x30 && (c11 & 0x10)) v->den_frame = true;
    const bool bl = v->den_frame && line >= 0x30 && line <= 0xF7 && (line & 7) == (c11 & 7);
    v->badline = bl;

    /* sprite advance-line flip-flop is set while MxYE is clear (3.8.1 rule 1) */
    v->yexp_ff |= (uint8_t)~v->reg[0x17];

    switch (cyc) {
    case 14:                                     /* 3.7.2 rule 2 */
        v->vc = v->vcbase;
        v->vmli = 0;
        if (bl) v->rc = 0;
        break;
    case 16:                                     /* 3.8.1 rule 7 / 7a */
        for (int n = 0; n < 8; n++) {
            const uint8_t b = (uint8_t)(1 << n);
            if (v->yexp_ff & b) {
                if (v->crunch & b)
                    v->mcbase[n] = (uint8_t)((0x2A & (v->mcbase[n] & v->mc[n])) |
                                             (0x15 & (v->mcbase[n] | v->mc[n])));
                else
                    v->mcbase[n] = v->mc[n];
            }
            if (v->mcbase[n] == 63) v->sdma &= (uint8_t)~b;
        }
        v->crunch = 0;
        break;
    case 55: case 56: {                          /* 3.8.1 rule 2 */
        const uint8_t en = v->reg[0x15];
        for (int n = 0; n < 8; n++) {
            const uint8_t b = (uint8_t)(1 << n);
            if ((en & b) && !(v->sdma & b) && v->reg[1 + 2 * n] == (line & 0xFF)) {
                v->sdma |= b;
                v->mcbase[n] = 0;
                v->yexp_ff |= b;
            }
        }
        break;
    }
    case 58: {
        uint8_t ymatch = 0;                      /* 3.8.1 rule 4 */
        for (int n = 0; n < 8; n++) {
            v->mc[n] = v->mcbase[n];
            if (v->reg[1 + 2 * n] == (line & 0xFF)) ymatch |= (uint8_t)(1 << n);
        }
        v->sdisp = (uint8_t)((v->sdisp | ymatch) & v->sdma);
        if (bl) v->display = true;               /* 3.7.2 rule 5 */
        if (v->rc == 7) {
            v->vcbase = v->vc;
            if (!bl) v->display = false;
        }
        if (v->display) v->rc = (v->rc + 1) & 7;
        break;
    }
    case 63: {                                   /* 3.9 rules 2, 3 */
        const int top = (c11 & 8) ? 51 : 55, bottom = (c11 & 8) ? 251 : 247;
        if (line == bottom) v->vborder = true;
        else if (line == top && (c11 & 0x10)) v->vborder = false;
        break;
    }
    default: break;
    }

    /* phase-1 access (3.6.3) */
    uint16_t addr;
    if (cyc >= 16 && cyc <= 55) {                /* g-access */
        const int col = cyc - 16;
        if (v->display) {
            const int ix = v->vmli;
            const uint8_t ch = ix < 40 ? v->cbuf[ix] : 0, cl = ix < 40 ? v->colbuf[ix] : 0;
            if (c11 & 0x20)
                addr = (uint16_t)(((v->reg[0x18] & 0x08) << 10) | (v->vc << 3) | v->rc);
            else
                addr = (uint16_t)(((v->reg[0x18] & 0x0E) << 10) | (ch << 3) | v->rc);
            v->gchar[col] = ch;
            v->gcol[col] = cl;
            v->gidle[col] = false;
            v->vc = (v->vc + 1) & 0x3FF;         /* 3.7.2 rule 4 */
            v->vmli = (v->vmli + 1) & 63;
        } else {
            addr = 0x3FFF;                       /* 3.7.3.9 */
            v->gchar[col] = 0;
            v->gcol[col] = 0;
            v->gidle[col] = true;
        }
        if (c11 & 0x40) addr &= 0x39FF;          /* ECM holds A9/A10 low */
        v->fetch_kind = F_G;
        v->fetch_col = col;
    } else if (tab_p[cyc] >= 0) {                /* p-access, always done */
        const int n = tab_p[cyc];
        addr = (uint16_t)(((v->reg[0x18] & 0xF0) << 6) | 0x3F8 | n);
        v->fetch_kind = F_P;
        v->fetch_col = n;
    } else if (tab_s1[cyc] >= 0 && (v->sdma & (1 << tab_s1[cyc]))) {
        const int n = tab_s1[cyc];               /* 2nd s-access */
        addr = (uint16_t)((v->sptr[n] << 6) | v->mc[n]);
        v->mc[n] = (v->mc[n] + 1) & 63;
        v->fetch_kind = F_S1;
        v->fetch_col = n;
    } else if (cyc >= 11 && cyc <= 15) {         /* DRAM refresh, 3.13 */
        addr = (uint16_t)(0x3F00 | v->refresh);
        v->refresh--;
        v->fetch_kind = F_REFRESH;
    } else {
        addr = 0x3FFF;                           /* idle access */
        v->fetch_kind = F_IDLE;
    }
    v->fetch_addr = addr;

    /* idle -> display as soon as there is a bad line condition (3.7.1); the
       g-access of this very cycle was still generated in the old state
       (DMA delay, 3.14.6: the screen moves by the number of cycles after 14) */
    if (bl) v->display = true;

    /* BA: bad line condition in cycles 12-54 (3.7.2 rule 3) or sprite DMA */
    bool ba_low = (bl && cyc >= 12 && cyc <= 54) || (v->sdma & tab_ba[cyc]);
    if (bl && cyc >= 12 && cyc <= 54 && !v->line_badline) {
        v->line_badline = true;
        v->badlines++;
    }
    v->ba_low = ba_low ? v->ba_low + 1 : 0;

    /* phase-2 access of this cycle */
    v->p2_kind = F_NONE;
    if (bl && cyc >= 15 && cyc <= 54) {
        v->p2_kind = F_C;
    } else if (tab_p[cyc] >= 0 && (v->sdma & (1 << tab_p[cyc]))) {
        v->p2_kind = F_S0; v->p2_idx = tab_p[cyc];
    } else if (tab_s1[cyc] >= 0 && (v->sdma & (1 << tab_s1[cyc]))) {
        v->p2_kind = F_S2; v->p2_idx = tab_s1[cyc];
    }
    /* AEC stays low in phase 2 only from the 4th cycle of BA low on (2.4.3) */
    v->ba = !ba_low;
    v->aec = !(v->p2_kind != F_NONE && v->ba_low >= 4);

    pins &= ~(VIC_FETCH | VIC_A_MASK | VIC_BA | VIC_AEC);
    pins |= VIC_FETCH | addr;
    if (v->ba) pins |= VIC_BA;
    if (v->aec) pins |= VIC_AEC;
    return out_irq(v, pins);
}

/* ------------------------------------------------------------- PHI2 */
static uint64_t phi2(vic_t *v, uint64_t pins) {
    const bool cs = (pins & VIC_CS) != 0;
    const bool wr = cs && !(pins & VIC_RW);
    if (pins & VIC_CPUD) v->cpu_bus = (uint8_t)(pins >> VIC_CPUD_SHIFT);

    /* phase-1 data; lost if the board put CPU write data on D */
    bool refetch = false;
    if (!wr) consume_phase1(v, VIC_GET_D(pins));
    else refetch = v->fetch_kind == F_G || v->fetch_kind == F_P || v->fetch_kind == F_S1;

    /* 8 pixels; a CPU register access lands at pixel wdelay[reg] */
    if (cs) {
        const int r = (int)(pins & 0x3F);
        const int w = v->wdelay[r] > 8 ? 8 : v->wdelay[r];
        emit(v, 0, w);
        if (pins & VIC_RW) {
            uint8_t d = reg_read(v, r);
            v->cpu_bus = d;
            pins = VIC_SET_D(pins, (VIC_GET_D(pins) & 0xF00) | d);
        } else {
            uint8_t d = (uint8_t)VIC_GET_D(pins);
            v->cpu_bus = d;
            reg_write(v, r, d);
            if (r <= 0x2E) v->preg[r] = v->reg[r];
        }
        emit(v, w, 8);
    } else {
        emit(v, 0, 8);
    }

    /* 3.8.1 rule 3: second phase of cycle 56 (after a CPU write in it) */
    if (v->cycle == 56) v->yexp_ff ^= (uint8_t)(v->reg[0x17] & v->sdma);

    /* phase-2 access */
    pins &= ~(VIC_FETCH | VIC_A_MASK);
    const bool own = !v->aec;
    switch (v->p2_kind) {
    case F_C:
        if (own) {
            pins |= VIC_FETCH | (uint16_t)(((v->reg[0x18] & 0xF0) << 6) | v->vc);
            v->fetch2_kind = F_C;
            v->fetch2_idx = v->vmli < 40 ? v->vmli : 39;
        } else if (v->vmli < 40) {
            /* AEC still high: D0-D7 read $ff, D8-D11 see the CPU's D0-D3 (3.14.6) */
            v->cbuf[v->vmli] = 0xFF;
            v->colbuf[v->vmli] = v->cpu_bus & 15;
        }
        break;
    case F_S0: case F_S2: {
        const int n = v->p2_idx;
        if (own) {
            pins |= VIC_FETCH | (uint16_t)((v->sptr[n] << 6) | v->mc[n]);
            v->fetch2_kind = v->p2_kind;
            v->fetch2_idx = n;
        } else if (v->p2_kind == F_S0) {
            v->sfetch[n] = 0xFF0000;
        } else {
            v->sfetch[n] |= 0xFF;
            sprite_line_done(v, n);
        }
        v->mc[n] = (v->mc[n] + 1) & 63;
        break;
    }
    default:
        break;
    }
    if (refetch && !(pins & VIC_FETCH)) {   /* CPU owned this PHI2, bus is free */
        pins |= VIC_FETCH | v->fetch_addr;
        v->fetch2_kind = F_REFETCH;
    }
    return out_irq(v, pins);
}

uint64_t vic_tick(vic_t *v, uint64_t pins) {
    return (pins & VIC_PHI0) ? phi2(v, pins) : phi1(v, pins);
}

/* ------------------------------------------------------------- output */
void vic_crop_normal(const vic_t *v, uint8_t *dst) {
    /* crop column 0 = X $1F0 = fb column 16; row 0 = raster 16 = fb row 0 */
    for (int y = 0; y < VIC_CROP_H; y++)
        memcpy(dst + y * VIC_CROP_W, &v->fb[y][16], VIC_CROP_W);
}

void vic_crop_normal_rgb(const vic_t *v, uint8_t *rgb, const uint32_t *pal) {
    if (!pal) pal = vic_palette_pepto;
    for (int y = 0; y < VIC_CROP_H; y++)
        for (int x = 0; x < VIC_CROP_W; x++) {
            uint32_t c = pal[v->fb[y][16 + x] & 15];
            uint8_t *p = rgb + 3 * (y * VIC_CROP_W + x);
            p[0] = (uint8_t)(c >> 16); p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)c;
        }
}
