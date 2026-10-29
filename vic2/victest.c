/* victest.c - test bench for vic.c.
 *
 * Drives the VIC exactly like harness/src/c64.c does: one PHI1 call (answer
 * VIC_FETCH from a synthetic 16 KB RAM + 1 KB colour RAM), then the "CPU"
 * (a script of register reads/writes at given (line, cycle), only allowed
 * when AEC is high, as on the real bus), then one PHI2 call (register access,
 * answer VIC_FETCH).  Register write data is put on D0..D7, clobbering the
 * phase-1 data exactly as the harness board does.
 *
 * Images go to out/ (palettised PNG, Pepto palette).  Exit status = number of
 * failed checks. */
#include "vic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------ helpers */
static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
    vic_t v;
    uint8_t mem[16384];
    uint8_t col[1024];
    uint64_t pins;
    /* per-cycle record of the current and the previous frame */
    uint8_t ba[VIC_LINES][64], aec[VIC_LINES][64], irq[VIC_LINES][64];
    int32_t a1[VIC_LINES][64], a2[VIC_LINES][64];   /* phase-1/2 fetch address or -1 */
    /* scripted CPU */
    void (*cpu)(void *bench, int *reg, int *val, int *rd);   /* per-cycle callback */
    int bad_access;                                         /* CPU access while AEC low */
} bench_t;

static bench_t *bench_new(void) {
    bench_t *b = calloc(1, sizeof *b);
    vic_init(&b->v);
    return b;
}

static uint16_t memread(bench_t *b, uint64_t p) {
    uint16_t a = (uint16_t)(p & VIC_A_MASK);
    return (uint16_t)(b->mem[a] | ((b->col[a & 0x3FF] & 15) << 8));
}

/* one PHI2 cycle; reg >= 0 performs a register access in this cycle.
   returns the read value (or -1) */
static int step(bench_t *b, int reg, int val, int rd) {
    uint64_t p = b->pins & ~(VIC_PHI0 | VIC_CS | VIC_RW);
    p = vic_tick(&b->v, p);
    const int L = b->v.line, C = b->v.cycle;
    b->a1[L][C] = (p & VIC_FETCH) ? (int32_t)(p & VIC_A_MASK) : -1;
    if (p & VIC_FETCH) p = VIC_SET_D(p, memread(b, p));
    b->ba[L][C] = (p & VIC_BA) ? 1 : 0;
    b->aec[L][C] = (p & VIC_AEC) ? 1 : 0;
    b->irq[L][C] = (p & VIC_IRQ) ? 1 : 0;
    if (reg < 0 && b->cpu) b->cpu(b, &reg, &val, &rd);
    int ret = -1;
    p |= VIC_PHI0;
    if (reg >= 0) {
        if (!(p & VIC_AEC)) { if (!b->bad_access) printf("    CPU access at line %d cycle %d while AEC low\n", L, C); b->bad_access++; }
        p = (p & ~VIC_A_MASK) | VIC_CS | (uint64_t)(reg & 0x3F) | (rd ? VIC_RW : 0);
        if (!rd) p = VIC_SET_D(p, val);          /* clobbers phase-1 data, as c64.c */
    }
    p = vic_tick(&b->v, p);
    if (reg >= 0 && rd) ret = VIC_GET_D(p) & 0xFF;
    b->a2[L][C] = (p & VIC_FETCH) ? (int32_t)(p & VIC_A_MASK) : -1;
    if (p & VIC_FETCH) p = VIC_SET_D(p, memread(b, p));
    b->pins = p & ~(VIC_CS | VIC_RW);
    return ret;
}

/* run until the next cycle to execute is (line, cycle) */
static void run_to(bench_t *b, int line, int cycle) {
    for (;;) {
        int nc = b->v.cycle % 63 + 1;
        int nl = b->v.cycle == 63 ? (b->v.line + 1) % VIC_LINES : b->v.line;
        if (nl == line && nc == cycle) return;
        step(b, -1, 0, 0);
    }
}
static int access_at(bench_t *b, int line, int cycle, int reg, int val, int rd) {
    run_to(b, line, cycle);
    return step(b, reg, val, rd);
}
static void wr(bench_t *b, int line, int cycle, int reg, int val) { access_at(b, line, cycle, reg, val, 0); }
static int rdreg(bench_t *b, int line, int cycle, int reg) { return access_at(b, line, cycle, reg, 0, 1); }
/* immediate write in the next cycle */
static void poke(bench_t *b, int reg, int val) { step(b, reg, val, 0); }
static void run_frames(bench_t *b, int n) {
    for (int i = 0; i < n; i++) { run_to(b, 0, 1); step(b, -1, 0, 0); }
    run_to(b, 0, 1);
}
/* full frame starting at line 0 cycle 1 (records the frame) */
static void run_one_frame(bench_t *b) {
    run_to(b, 0, 1);
    do step(b, -1, 0, 0); while (!(b->v.line == VIC_LINES - 1 && b->v.cycle == 63));
}

/* framebuffer pixel at display X (sprite coordinates), raster line y */
static int pix(const bench_t *b, int x, int y) {
    x = ((x % 504) + 504) % 504;
    int fx = x >= 0x1E0 ? x - 0x1E0 : x + 24;
    if (fx >= VIC_SCREEN_W || y < VIC_FIRST_VIS_LINE || y >= VIC_FIRST_VIS_LINE + VIC_SCREEN_H) return -1;
    return b->v.fb[y - VIC_FIRST_VIS_LINE][fx];
}

/* ------------------------------------------------------------- PNG out */
static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            crc_tab[i] = k;
        }
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc(crc(0xFFFFFFFFu, (const uint8_t *)t, 4), d, n) ^ 0xFFFFFFFFu;
    uint8_t cb[4]; be32(cb, c); fwrite(cb, 1, 4, f);
}
/* palettised PNG with stored deflate blocks; idx = w*h palette indices */
static void png_indexed(const char *path, const uint8_t *idx, int w, int h, int scale) {
    int W = w * scale, H = h * scale;
    size_t rawn = (size_t)H * (W + 1);
    uint8_t *raw = malloc(rawn);
    for (int y = 0; y < H; y++) {
        raw[(size_t)y * (W + 1)] = 0;
        for (int x = 0; x < W; x++) raw[(size_t)y * (W + 1) + 1 + x] = idx[(y / scale) * w + x / scale] & 15;
    }
    size_t nb = (rawn + 65534) / 65535, zn = 2 + rawn + nb * 5 + 4, o = 0;
    uint8_t *z = malloc(zn);
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, bb = 0;
    for (size_t pos = 0; pos < rawn;) {
        size_t n = rawn - pos > 65535 ? 65535 : rawn - pos;
        z[o++] = pos + n == rawn; z[o++] = n & 255; z[o++] = n >> 8; z[o++] = ~n & 255; z[o++] = (~n >> 8) & 255;
        memcpy(z + o, raw + pos, n);
        for (size_t i = 0; i < n; i++) { a = (a + raw[pos + i]) % 65521; bb = (bb + a) % 65521; }
        o += n; pos += n;
    }
    be32(z + o, (bb << 16) | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  cannot write %s\n", path); free(raw); free(z); return; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, W); be32(ih + 4, H); ih[8] = 8; ih[9] = 3; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    uint8_t pl[48];
    for (int i = 0; i < 16; i++) { pl[3*i] = vic_palette[i] >> 16; pl[3*i+1] = vic_palette[i] >> 8; pl[3*i+2] = vic_palette[i]; }
    chunk(f, "PLTE", pl, 48);
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
}
static void save_crop(const bench_t *b, const char *name) {
    static uint8_t buf[VIC_CROP_W * VIC_CROP_H];
    char path[256];
    vic_crop_normal(&b->v, buf);
    snprintf(path, sizeof path, "out/%s.png", name);
    png_indexed(path, buf, VIC_CROP_W, VIC_CROP_H, 2);
    printf("  wrote %s\n", path);
}

/* common screen setup: VM $0400, chars at $2000 (or bitmap at $2000) */
static uint8_t charpat(int ch, int l) { return (uint8_t)(((ch * 37) ^ (l * 0x5B) ^ (l << 5)) + l); }
static void setup_text_mem(bench_t *b) {
    for (int ch = 0; ch < 256; ch++)
        for (int l = 0; l < 8; l++) b->mem[0x2000 + ch * 8 + l] = charpat(ch, l);
    for (int p = 0; p < 1000; p++) {
        b->mem[0x0400 + p] = (uint8_t)((p * 7 + 3) & 0xFF);
        b->col[p] = (uint8_t)((p * 5 + 1) & 15);
    }
    b->mem[0x3FFF] = 0x81;   /* idle pattern */
    b->mem[0x39FF] = 0x18;
}
static void basic_regs(bench_t *b, int d011, int d016, int d018) {
    poke(b, 0x20, 14); poke(b, 0x21, 6); poke(b, 0x22, 2); poke(b, 0x23, 5); poke(b, 0x24, 7);
    poke(b, 0x11, d011); poke(b, 0x16, d016); poke(b, 0x18, d018);
}

/* ================================================================ T1 */
static void t1_badline_dma(void) {
    printf("T1 badline / sprite BA + AEC timing\n");
    bench_t *b = bench_new();
    setup_text_mem(b);
    basic_regs(b, 0x1B, 0x08, 0x18);
    run_frames(b, 2);
    run_one_frame(b);
    int nbad = 0;
    for (int L = 0; L < VIC_LINES; L++) {
        bool isbad = L >= 0x30 && L <= 0xF7 && (L & 7) == 3;
        int aeclow = 0, baonly = 0, errs = 0;
        for (int c = 1; c <= 63; c++) {
            bool eba = isbad && c >= 12 && c <= 54, eaec = isbad && c >= 15 && c <= 54;
            if (b->ba[L][c] == eba || b->aec[L][c] == eaec) errs++;
            aeclow += !b->aec[L][c]; baonly += !b->ba[L][c] && b->aec[L][c];
        }
        if (isbad) {
            nbad++;
            CHECK(aeclow == 40 && baonly == 3, "line %d: %d stolen + %d BA-only (want 40 + 3)", L, aeclow, baonly);
        }
        CHECK(errs == 0, "line %d: %d cycles with wrong BA/AEC", L, errs);
    }
    CHECK(nbad == 25 && b->v.last_badlines == 25, "badlines %d / stat %u", nbad, b->v.last_badlines);
    /* g-access, refresh, p-access addresses of a display line and of line 0 */
    {
        int L = 0x33, errs = 0;
        for (int c = 16; c <= 55; c++) {
            int ch = b->mem[0x400 + c - 16];
            if (b->a1[L][c] != 0x2000 + ch * 8 + 0) errs++;
            if (c <= 54 && b->a2[L][c] != 0x400 + c - 15) errs++;
        }
        CHECK(errs == 0, "badline g/c addresses (%d errors)", errs);
        CHECK(b->a1[0][11] == 0x3FFF && b->a1[0][15] == 0x3FFB && b->a1[1][11] == 0x3FFA,
              "refresh addresses %04X %04X %04X", b->a1[0][11], b->a1[0][15], b->a1[1][11]);
        CHECK(b->a1[0x20][20] == 0x3FFF && b->a1[0x20][57] == 0x3FFF, "idle g-access / idle access at $3fff");
        CHECK(b->a1[100][58] == 0x7F8 && b->a1[100][9] == 0x7FF, "p-access addresses %04X %04X",
              b->a1[100][58], b->a1[100][9]);
    }
    free(b);

    /* each sprite alone, then all 8, at Y=100 (line 100 is no badline) */
    for (int set = 0; set <= 8; set++) {
        uint8_t en = set < 8 ? (uint8_t)(1 << set) : 0xFF;
        b = bench_new();
        setup_text_mem(b);
        basic_regs(b, 0x1B, 0x08, 0x18);
        for (int n = 0; n < 8; n++) { poke(b, 2 * n, 40 + 30 * n); poke(b, 2 * n + 1, 100); b->mem[0x7F8 + n] = 0x80 + n; }
        poke(b, 0x15, en);
        run_frames(b, 1);
        run_one_frame(b);
        static const int P[8] = { 58, 60, 62, 1, 3, 5, 7, 9 };
        int errs = 0, stolen = 0, balow = 0;
        for (int L = 90; L < 130; L++)
            for (int c = 1; c <= 63; c++) {
                bool eba = false, eaec = false;
                for (int n = 0; n < 8; n++) {
                    if (!(en & (1 << n))) continue;
                    /* DMA on from line 100 cycle 55 to line 121 cycle 16 */
                    bool dma = (L > 100 || (L == 100 && c >= 55)) && (L < 121 || (L == 121 && c < 16));
                    for (int k = -3; k <= 1; k++) {
                        int wc = P[n] + k; if (wc < 1) wc += 63; if (wc > 63) wc -= 63;
                        if (wc == c && dma) { eba = true; if (k >= 0) eaec = true; }
                    }
                }
                bool isbad = (L & 7) == 3;
                if (isbad && c >= 12 && c <= 54) { eba = true; if (c >= 15) eaec = true; }
                if (b->ba[L][c] == eba || b->aec[L][c] == eaec) errs++;
                if (!isbad || c < 12 || c > 54) { stolen += !b->aec[L][c]; balow += !b->ba[L][c]; }
            }
        if (set < 8) {
            CHECK(errs == 0 && stolen == 21 * 2 && balow == 21 * 5,
                  "sprite %d: %d wrong cycles, %d stolen (want 42), %d BA low (want 105)", set, errs, stolen, balow);
            /* s-access addresses: pointer $80+n, MC 0..2 in the first line */
            int Ls = set < 3 ? 100 : 101, p = P[set], p1 = p % 63 + 1;
            int base = (0x80 + set) << 6;
            CHECK(b->a2[Ls][p] == base + 0 && b->a1[Ls][p1] == base + 1 && b->a2[Ls][p1] == base + 2,
                  "sprite %d s-access addresses %04X %04X %04X", set, b->a2[Ls][p], b->a1[Ls][p1], b->a2[Ls][p1]);
        } else {
            int line_stolen = 0, line_ba = 0;
            for (int c = 1; c <= 63; c++) { line_stolen += !b->aec[110][c]; line_ba += !b->ba[110][c]; }
            CHECK(errs == 0 && line_stolen == 16 && line_ba == 19,
                  "8 sprites: %d wrong cycles, line 110: %d stolen (want 16), BA low %d (want 19)",
                  errs, line_stolen, line_ba);
            /* badline + 8 sprites: line 107 ($6b) - CPU only gets cycle 11 */
            int high = 0, which = 0;
            for (int c = 1; c <= 63; c++) if (b->ba[107][c]) { high++; which = c; }
            CHECK(high == 1 && which == 11, "badline+8 sprites: BA high in %d cycles (cycle %d), want only 11", high, which);
        }
        free(b);
    }
}

/* ================================================================ T2 */
static void t2_raster_irq(void) {
    printf("T2 raster IRQ, $d011/$d012 reads\n");
    bench_t *b = bench_new();
    setup_text_mem(b);
    basic_regs(b, 0x1B, 0x08, 0x18);
    poke(b, 0x12, 100);
    poke(b, 0x1A, 0x01);
    run_frames(b, 1);
    wr(b, 50, 10, 0x19, 0x0F);       /* ack the IRQ of the previous frame */
    run_to(b, 99, 60);
    int r63 = rdreg(b, 99, 63, 0x12);
    CHECK(!b->irq[99][63], "IRQ already at line 99 cycle 63");
    int r1 = rdreg(b, 100, 1, 0x12);
    CHECK(b->irq[100][1], "IRQ not asserted in PHI1 of line 100 cycle 1");
    CHECK(r63 == 99 && r1 == 100, "$d012 reads %d/%d across line boundary (want 99/100)", r63, r1);
    int s = rdreg(b, 100, 5, 0x19);
    CHECK(s == 0xF1, "$d019 = %02X (want F1)", s);
    wr(b, 100, 20, 0x19, 0x01);
    step(b, -1, 0, 0);
    CHECK(!b->irq[100][21] && b->irq[100][20], "IRQ not released one cycle after $d019 ack");
    s = rdreg(b, 100, 22, 0x19);
    CHECK(s == 0x70, "$d019 after ack = %02X (want 70)", s);

    /* immediate trigger by writing the current line, only once per line */
    wr(b, 150, 30, 0x12, 150);
    step(b, -1, 0, 0);
    CHECK(b->irq[150][31] && !b->irq[150][30], "write of current line did not trigger IRQ in next cycle");
    wr(b, 150, 33, 0x19, 0x01);
    wr(b, 150, 35, 0x12, 151);
    wr(b, 150, 37, 0x12, 150);
    run_to(b, 150, 45);
    CHECK(!b->irq[150][40], "second IRQ in the same raster line");
    run_to(b, 151, 5);
    CHECK(!b->irq[151][1], "IRQ at line 151 (compare is 150)");

    /* line 0: compare and RASTER reset happen in cycle 2 */
    wr(b, 200, 10, 0x12, 0x00);
    run_to(b, 311, 60);
    int a = rdreg(b, 311, 63, 0x12), ah = rdreg(b, 0, 1, 0x11) & 0x80;
    int a1 = b->v.line == 0 ? rdreg(b, 0, 2, 0x12) : -1;
    CHECK(!b->irq[0][1] && b->irq[0][2], "line 0 IRQ not in cycle 2 (c1=%d c2=%d)", b->irq[0][1], b->irq[0][2]);
    int a1h = rdreg(b, 0, 3, 0x11) & 0x80;
    CHECK(a == 0x37 && ah == 0x80 && a1 == 0 && a1h == 0,
          "RASTER reads at 311/63=%02X, 0/1 bit8=%X, 0/2=%02X bit8=%X", a, ah, a1, a1h);
    wr(b, 0, 10, 0x19, 0x01);
    /* line 311 */
    wr(b, 0, 12, 0x11, 0x9B);
    wr(b, 0, 14, 0x12, 0x37);
    run_to(b, 311, 3);
    CHECK(b->irq[311][1] && !b->irq[310][63], "line 311 IRQ not at cycle 1");
    CHECK(b->bad_access == 0, "script accessed the bus while AEC low");
    free(b);
}

/* ================================================================ T3 */
/* reference model of the graphics, written from the article's tables */
static int ref_gfx(int mode, uint8_t g, int b, uint8_t ch, uint8_t cl, const uint8_t bg[4]) {
    int bit = (g >> (7 - b)) & 1, pair = (g >> (6 - (b & 6))) & 3;
    switch (mode) {
    case 0: return bit ? cl : bg[0];
    case 1: if (cl & 8) return pair == 0 ? bg[0] : pair == 1 ? bg[1] : pair == 2 ? bg[2] : (cl & 7);
            return bit ? (cl & 7) : bg[0];
    case 2: return bit ? ch >> 4 : ch & 15;
    case 3: return pair == 0 ? bg[0] : pair == 1 ? ch >> 4 : pair == 2 ? (ch & 15) : cl;
    case 4: return bit ? cl : bg[ch >> 6];
    default: return 0;
    }
}

/* full-frame compare against a static-screen model */
static int compare_static(bench_t *b, int mode, int d011, int d016, int d018) {
    const int xs = d016 & 7, ys = d011 & 7;
    const int top = (d011 & 8) ? 51 : 55, bot = (d011 & 8) ? 251 : 247;
    const int left = (d016 & 8) ? 24 : 31, right = (d016 & 8) ? 344 : 335;
    const uint8_t bg[4] = { 6, 2, 5, 7 };
    int first_bad = 0x30 + ((ys - 0x30) & 7);
    int errs = 0;
    for (int y = VIC_FIRST_VIS_LINE; y < VIC_FIRST_VIS_LINE + VIC_SCREEN_H; y++)
        for (int x = 0; x < 504; x++) {
            int got = pix(b, x, y);
            if (got < 0) continue;
            int want;
            if (y < top || y >= bot || x < left || x >= right) want = 14;
            else {
                /* display state from the first badline for 25 text rows,
                   RC = lines since the badline; idle state elsewhere */
                int dl = y - first_bad;
                int gx = x - 24 - xs;          /* >= 0 inside the window here */
                if (dl < 0 || dl >= 200) {
                    uint8_t g = b->mem[(d011 & 0x40) ? 0x39FF : 0x3FFF];
                    want = ref_gfx(mode, g, gx & 7, 0, 0, bg);   /* c-data = 0 */
                } else {
                    int row = dl >> 3, rc = dl & 7, colm = gx >> 3, bb = gx & 7;
                    int vc = row * 40 + colm;
                    uint8_t ch = b->mem[0x400 + vc], cl = b->col[vc];
                    uint16_t ga;
                    if (d011 & 0x20) ga = (uint16_t)(((d018 & 8) << 10) | (vc << 3) | rc);
                    else ga = (uint16_t)(((d018 & 0x0E) << 10) | (ch << 3) | rc);
                    if (d011 & 0x40) ga &= 0x39FF;
                    want = ref_gfx(mode, b->mem[ga], bb, ch, cl, bg);
                }
            }
            if (got != want) {
                if (errs < 5) printf("    mismatch x=%d y=%d got %d want %d\n", x, y, got, want);
                errs++;
            }
        }
    return errs;
}

static void t3_render(void) {
    printf("T3 text render and all graphics modes\n");
    static const struct { int d011, d016, d018; const char *name; } cfg[] = {
        { 0x1B, 0x08, 0x18, "mode0_text" },
        { 0x1B, 0x18, 0x18, "mode1_mctext" },
        { 0x3B, 0x08, 0x18, "mode2_bitmap" },
        { 0x3B, 0x18, 0x18, "mode3_mcbitmap" },
        { 0x5B, 0x08, 0x18, "mode4_ecm" },
        { 0x5B, 0x18, 0x18, "mode5_invalid_text" },
        { 0x7B, 0x08, 0x18, "mode6_invalid_bm1" },
        { 0x7B, 0x18, 0x18, "mode7_invalid_bm2" },
        { 0x17, 0x05, 0x18, "text_xs5_ys7_38x24" },
        { 0x18, 0x08, 0x18, "text_ys0_idle_stripes" },
    };
    for (unsigned i = 0; i < sizeof cfg / sizeof cfg[0]; i++) {
        bench_t *b = bench_new();
        setup_text_mem(b);
        if (cfg[i].d011 & 0x20)   /* bitmap data */
            for (int a = 0; a < 8000; a++) b->mem[0x2000 + a] = (uint8_t)(a * 13 + (a >> 3));
        basic_regs(b, cfg[i].d011, cfg[i].d016, cfg[i].d018);
        run_frames(b, 2);
        int mode = ((cfg[i].d011 & 0x60) >> 4) | ((cfg[i].d016 & 0x10) >> 4);
        int errs = compare_static(b, mode, cfg[i].d011, cfg[i].d016, cfg[i].d018);
        CHECK(errs == 0, "%s: %d pixel mismatches", cfg[i].name, errs);
        if (mode >= 5) {
            int nonblack = 0;
            for (int y = 60; y < 240; y++) for (int x = 40; x < 330; x++) nonblack += pix(b, x, y) != 0;
            CHECK(nonblack == 0, "%s: %d non-black pixels in the window", cfg[i].name, nonblack);
        }
        save_crop(b, cfg[i].name);
        free(b);
    }
    /* known glyph check by hand: char 0 of row 0 at X 24..31, line 51 */
    {
        bench_t *b = bench_new();
        setup_text_mem(b);
        b->mem[0x400] = 0x41;
        b->col[0] = 1;
        for (int l = 0; l < 8; l++) b->mem[0x2000 + 0x41 * 8 + l] = (uint8_t)(0x80 >> l);
        basic_regs(b, 0x1B, 0x08, 0x18);
        run_frames(b, 2);
        int ok = 1;
        for (int l = 0; l < 8; l++)
            for (int k = 0; k < 8; k++)
                if (pix(b, 24 + k, 51 + l) != (k == l ? 1 : 6)) ok = 0;
        CHECK(ok && pix(b, 23, 51) == 14 && pix(b, 24, 50) == 14, "diagonal glyph not at X 24 / line 51");
        free(b);
    }
}

/* ================================================================ T4 */
static int fpp_mode;
static void fpp_cpu(void *bp, int *reg, int *val, int *rd) {
    bench_t *b = bp;
    const int L = b->v.line, C = b->v.cycle;
    *rd = 0;
    if (fpp_mode == 0) {           /* classic: every line a full badline */
        if (L >= 0x30 && L < 200) {
            if (C == 2) { *reg = 0x11; *val = 0x18 | (L & 7); }
            if (C == 3) { *reg = 0x18; *val = (L & 1) ? 0x1A : 0x18; }
        }
        if (L == 200 && C == 2) { *reg = 0x11; *val = 0x1B; }
    } else {                       /* intro style: $d011 in cycle 55, no DMA */
        if (L >= 55 && L < 219) {
            if (C == 55) { *reg = 0x11; *val = 0x10 | (L & 7); }
            if (C == 57) { *reg = 0x18; *val = ((L + 1) & 1) ? 0x1A : 0x18; }
        }
        if (L == 219 && C == 55) { *reg = 0x11; *val = 0x13; }
    }
}
static void t4_fpp(void) {
    printf("T4 FPP (per-line $d011 with YSCROLL = raster & 7, $d018 per line)\n");
    for (int m = 0; m < 2; m++) {
        bench_t *b = bench_new();
        /* charset A at $2000, B at $2800: char c line l = distinct patterns */
        for (int c = 0; c < 256; c++)
            for (int l = 0; l < 8; l++) {
                b->mem[0x2000 + c * 8 + l] = (uint8_t)(c ^ (l * 0x11));
                b->mem[0x2800 + c * 8 + l] = (uint8_t)~(c ^ (l * 0x11));
            }
        for (int p = 0; p < 1000; p++) { b->mem[0x400 + p] = (uint8_t)(p < 40 ? p * 6 : 0x55); b->col[p] = (uint8_t)(p < 40 ? 1 + p % 15 : 2); }
        basic_regs(b, m == 0 ? 0x18 : 0x13, 0x08, 0x18);
        run_frames(b, 1);
        fpp_mode = m;
        b->cpu = fpp_cpu;
        run_one_frame(b);
        run_to(b, 0, 1);
        int errs = 0, dma_lines = 0;
        int first = m == 0 ? 51 : 55, last = m == 0 ? 199 : 218;
        for (int y = 0x30; y <= last; y++) {
            int balow = 0;
            for (int c = 1; c <= 63; c++) balow += !b->ba[y][c];
            dma_lines += balow == 43;
            if (m == 1 && y > 51 && balow) errs++;     /* only line 51 may do DMA */
        }
        for (int y = first; y <= last; y++) {
            /* classic: RC reset every line; intro style: RC keeps counting
               from the one real badline (51), chars stay those of row 0 */
            int rc = m == 0 ? 0 : (y - 51) & 7;
            int cs = (m == 1 && y <= 55) ? 0x2000 : ((y & 1) ? 0x2800 : 0x2000);
            for (int x = 24; x < 344; x++) {
                int k = (x - 24) >> 3, bb = (x - 24) & 7;
                uint8_t g = b->mem[cs + b->mem[0x400 + k] * 8 + rc];
                int want = ((g >> (7 - bb)) & 1) ? b->col[k] : 6;
                if (pix(b, x, y) != want) errs++;
            }
        }
        if (m == 0)
            CHECK(errs == 0 && dma_lines == last - 0x30 + 1,
                  "classic FPP: %d pixel errors, %d DMA lines (want %d)", errs, dma_lines, last - 0x30 + 1);
        else
            CHECK(errs == 0 && dma_lines == 1, "intro FPP: %d errors (pixel or BA low), %d DMA lines (want 1)",
                  errs, dma_lines);
        CHECK(b->bad_access == 0, "script accessed the bus while AEC low");
        save_crop(b, m == 0 ? "fpp_classic" : "fpp_intro_style");
        free(b);
    }
}

/* ================================================================ T5 */
static int sprite_mc_at(bench_t *b, int x, int y) {
    /* first byte of the displayed sprite line (bit 7 marker) -> its MC */
    int v = 0;
    for (int k = 0; k < 8; k++) {
        int p = pix(b, x + k, y);
        v = (v << 1) | (p == 1);
    }
    return (v & 0x80) ? (v & 0x3F) : -1;
}
static int t5_phase;
static void stretch_cpu(void *bp, int *reg, int *val, int *rd) {
    bench_t *b = bp;
    const int L = b->v.line, C = b->v.cycle;
    *rd = 0;
    /* no badlines (like the intro's FPP area): DEN clear in line $30, set
       again before the top border compare -> border open, idle graphics */
    if (L == 0x2F && C == 10) { *reg = 0x11; *val = 0x0B; return; }
    if (L == 0x31 && C == 1) { *reg = 0x11; *val = 0x1B; return; }
    if (t5_phase == 0) {
        /* sprite 0: intro-style $d017 toggling: clear at 30, set at 44 */
        if (L >= 70 && L < 90) {
            if (C == 30) { *reg = 0x17; *val = 0x00; }
            if (C == 44) { *reg = 0x17; *val = 0x01; }
        }
    } else if (t5_phase == 1) {
        /* sprite 1 expanded; crunch: clear M1YE in cycle 15 of line 151 */
        if (L == 151 && C == 15) { *reg = 0x17; *val = 0x00; }
        if (L == 151 && C == 17) { *reg = 0x17; *val = 0x02; }
    }
}
static void t5_sprite_stretch_crunch(void) {
    printf("T5 sprite Y-expansion stretch and crunch\n");
    for (int ph = 0; ph < 3; ph++) {
        bench_t *b = bench_new();
        setup_text_mem(b);
        for (int i = 0; i < 64; i++) b->mem[0x3000 + i] = (uint8_t)(0x80 | i);   /* byte i = $80|i */
        b->mem[0x7F8] = b->mem[0x7F9] = 0xC0;
        basic_regs(b, 0x0B, 0x08, 0x18);   /* DEN off: border, sprites still shown? no: border covers */
        poke(b, 0x11, 0x1B);
        for (int a = 0; a < 1000; a++) b->mem[0x400 + a] = 0x20;
        for (int a = 0; a < 8; a++) b->mem[0x2000 + 0x20 * 8 + a] = 0;
        poke(b, 0x27, 1); poke(b, 0x28, 1);
        poke(b, 0x00, 100); poke(b, 0x01, 60);
        poke(b, 0x02, 200); poke(b, 0x03, 150);
        poke(b, 0x15, ph == 0 ? 0x01 : 0x02);
        poke(b, 0x17, ph == 0 ? 0x00 : 0x02);
        run_frames(b, 1);
        t5_phase = ph;
        b->cpu = stretch_cpu;
        run_one_frame(b);
        run_to(b, 0, 1);
        if (ph == 0) {
            /* model of 3.8.1 rules 1/3/7 for this script.  MCBASE after
               cycle 16 of line L is displayed in line L+1 (sprite 0 fetches
               at the end of the line).  YE: 0 until line 70 cycle 30; lines
               70..89: 0 in cycles 30..44, else 1; 1 from line 89 cycle 44 on */
            int errs = 0, mcb = 0, ff = 1;
            for (int L = 61; L < 140; L++) {
                int ye_early = L >= 71, mid0 = L <= 89, ye56 = L >= 70;
                if (!ye_early) ff = 1;              /* rule 1, cycles 1..15 */
                if (ff) mcb = (mcb + 3) & 63;       /* rule 7, cycle 16 */
                if (mid0) ff = 1;                   /* rule 1, cycles 17..55 */
                if (ye56) ff = !ff;                 /* rule 3, cycle 56 */
                int want = mcb == 63 ? -1 : mcb, got = sprite_mc_at(b, 100, L + 1);
                if (got != want) { if (errs < 6) printf("    line %d: displayed MC %d, model %d\n", L + 1, got, want); errs++; }
                if (mcb == 63) break;
            }
            CHECK(errs == 0, "stretch: %d lines differ from the article model", errs);
            /* explicit expectations: row 10 (MC 30) frozen on lines 71..91 */
            int frozen = 0;
            for (int y = 71; y <= 91; y++) frozen += sprite_mc_at(b, 100, y) == 30;
            CHECK(frozen == 21, "stretch: row 10 shown on %d of lines 71..91 (want 21)", frozen);
            CHECK(sprite_mc_at(b, 100, 92) == 33 && sprite_mc_at(b, 100, 93) == 33 && sprite_mc_at(b, 100, 94) == 36,
                  "stretch: rows not doubled after the toggling (%d %d %d)",
                  sprite_mc_at(b, 100, 92), sprite_mc_at(b, 100, 93), sprite_mc_at(b, 100, 94));
            save_crop(b, "sprite_stretch");
        } else {
            /* expanded sprite 1 at Y=150. ph 1: crunch in line 151, ph 2: control */
            int lines = 0, errs = 0;
            for (int y = 151; y < 312 - 15; y++) if (sprite_mc_at(b, 200, y) >= 0) lines++;
            if (ph == 1) {
                CHECK(sprite_mc_at(b, 200, 151) == 0 && sprite_mc_at(b, 200, 152) == 1 &&
                      sprite_mc_at(b, 200, 153) == 1 && sprite_mc_at(b, 200, 154) == 4,
                      "crunch: MCBASE not 0,1,1,4 (%d %d %d %d)", sprite_mc_at(b, 200, 151),
                      sprite_mc_at(b, 200, 152), sprite_mc_at(b, 200, 153), sprite_mc_at(b, 200, 154));
                /* model: MCBASE 1,4,...,61,0 (6-bit wrap),3,...,60 for 2 lines
                   each, then 63 -> DMA off */
                int mcb = 1, y = 152;
                while (y < 300) {
                    for (int k = 0; k < 2; k++, y++) {
                        int got = sprite_mc_at(b, 200, y);
                        if (got != mcb) { if (errs < 4) printf("    line %d: MC %d want %d\n", y, got, mcb); errs++; }
                    }
                    mcb = (mcb + 3) & 63;
                    if (mcb == 63) { errs += sprite_mc_at(b, 200, y) != -1; break; }
                }
                CHECK(errs == 0, "crunch: %d lines differ from MCBASE model", errs);
                printf("    crunched sprite shown on %d lines (control: 42)\n", lines);
                CHECK(lines == 85, "crunch: sprite shown on %d lines (want 1 + 2*42)", lines);
                save_crop(b, "sprite_crunch");
            } else {
                CHECK(lines == 42, "expanded control sprite shown on %d lines (want 42)", lines);
            }
        }
        CHECK(b->bad_access == 0, "script accessed the bus while AEC low");
        free(b);
    }
}

/* ================================================================ T6 */
static int t6_mode, t6_cyc;
static void border_cpu(void *bp, int *reg, int *val, int *rd) {
    bench_t *b = bp;
    const int L = b->v.line, C = b->v.cycle;
    *rd = 0;
    if (t6_mode == 1) {                 /* open upper/lower border */
        if (L == 249 && C == 20) { *reg = 0x11; *val = 0x13; }
        if (L == 252 && C == 20) { *reg = 0x11; *val = 0x1B; }
    } else if (t6_mode == 2) {          /* open side border on lines 95..130 */
        if (L >= 95 && L < 130 && C == t6_cyc) { *reg = 0x16; *val = 0x00; }
        if (L >= 95 && L < 130 && C == 12) { *reg = 0x16; *val = 0x08; }
    }
}
static void t6_border(void) {
    printf("T6 border opening (RSEL switch at line 249, CSEL switch in cycle 56)\n");
    for (int m = 0; m < 2; m++) {
        bench_t *b = bench_new();
        setup_text_mem(b);
        for (int i = 0; i < 63; i++) b->mem[0x3000 + i] = 0xFF;
        for (int n = 0; n < 8; n++) b->mem[0x7F8 + n] = 0xC0;
        basic_regs(b, 0x1B, 0x08, 0x18);
        poke(b, 0x29, 7); poke(b, 0x04, 150); poke(b, 0x05, 245);        /* sprite 2 */
        poke(b, 0x2A, 3); poke(b, 0x06, 0x20); poke(b, 0x07, 20);        /* sprite 3 in top border */
        poke(b, 0x10, 0x08);
        poke(b, 0x15, 0x0C);
        run_frames(b, 1);
        t6_mode = m;
        b->cpu = border_cpu;
        run_one_frame(b);
        run_one_frame(b);
        run_to(b, 0, 1);
        int vis = 0;
        for (int y = 251; y < 267; y++) vis += pix(b, 160, y) == 7;
        int side = pix(b, 10, 260) == 14 && pix(b, 360, 260) == 14;
        int topv = pix(b, 0x125, 30) == 3;
        if (m == 0) CHECK(vis == 0 && !topv, "closed: sprite visible in lower (%d lines) / upper (%d) border", vis, topv);
        else CHECK(vis == 16 && side && topv, "open: sprite on %d of 16 lower border lines, side border %d, top %d",
                   vis, side, topv);
        save_crop(b, m == 0 ? "border_closed" : "border_open_rsel");
        free(b);
    }
    /* side border: CSEL=0 must be written exactly in cycle 56 */
    for (int cyc = 55; cyc <= 57; cyc++) {
        bench_t *b = bench_new();
        setup_text_mem(b);
        for (int i = 0; i < 63; i++) b->mem[0x3000 + i] = 0xFF;
        b->mem[0x7F8 + 4] = 0xC0;
        basic_regs(b, 0x1B, 0x08, 0x18);
        poke(b, 0x2B, 7); poke(b, 0x08, 0x58); poke(b, 0x09, 100); poke(b, 0x10, 0x10);  /* X=$158 */
        poke(b, 0x1D, 0x10);
        poke(b, 0x15, 0x10);
        run_frames(b, 1);
        t6_mode = 2; t6_cyc = cyc;
        b->cpu = border_cpu;
        run_one_frame(b);
        run_to(b, 0, 1);
        int vis = 0;
        for (int y = 101; y < 122; y++) vis += pix(b, 0x160, y) == 7;
        if (cyc == 56) CHECK(vis == 21, "CSEL write in cycle 56: sprite in right border on %d/21 lines", vis);
        else CHECK(vis == 0, "CSEL write in cycle %d opened the border (%d lines)", cyc, vis);
        if (cyc == 56) save_crop(b, "border_open_csel");
        CHECK(b->bad_access == 0, "script accessed the bus while AEC low");
        free(b);
    }
}

/* ================================================================ T7 */
static void t7_collisions(void) {
    printf("T7 sprite collisions, priority, multicolor, X expansion\n");
    bench_t *b = bench_new();
    setup_text_mem(b);
    for (int p = 0; p < 1000; p++) b->mem[0x400 + p] = 0x20;
    for (int l = 0; l < 8; l++) b->mem[0x2000 + 0x20 * 8 + l] = 0;
    b->mem[0x400 + 10 * 40 + 20] = 0x21;           /* one solid char at col 20 row 10 */
    for (int l = 0; l < 8; l++) b->mem[0x2000 + 0x21 * 8 + l] = 0xFF;
    b->col[10 * 40 + 20] = 2;
    for (int i = 0; i < 63; i++) { b->mem[0x3000 + i] = 0xFF; b->mem[0x3040 + i] = 0x1B; } /* mc: 00 01 10 11 */
    b->mem[0x7F8] = 0xC0; b->mem[0x7F9] = 0xC0; b->mem[0x7FA] = 0xC1;
    basic_regs(b, 0x1B, 0x08, 0x18);
    poke(b, 0x27, 1); poke(b, 0x28, 5); poke(b, 0x29, 3); poke(b, 0x25, 9); poke(b, 0x26, 10);
    /* Y >= 56: smaller Y also match raster 256+Y (8-bit compare, 312 lines) */
    poke(b, 0x00, 50); poke(b, 0x01, 60);          /* sprite 0 */
    poke(b, 0x02, 60); poke(b, 0x03, 65);          /* sprite 1 overlaps sprite 0 */
    poke(b, 0x04, 100); poke(b, 0x05, 60);         /* sprite 2: multicolor, x-expanded */
    poke(b, 0x1C, 0x04); poke(b, 0x1D, 0x04);
    poke(b, 0x15, 0x07);
    poke(b, 0x1A, 0x06);
    run_frames(b, 2);
    int ss = rdreg(b, 10, 20, 0x1E), sb = rdreg(b, 10, 22, 0x1F);
    int ss2 = rdreg(b, 10, 24, 0x1E);
    CHECK(ss == 0x03 && ss2 == 0x00, "sprite-sprite collision $d01e=%02X then %02X (want 03, 00)", ss, ss2);
    CHECK(sb == 0x00, "sprite-data collision without overlap: %02X", sb);
    int irqst = rdreg(b, 10, 26, 0x19);
    CHECK((irqst & 0x84) == 0x84, "MMC IRQ not flagged ($d019=%02X)", irqst);
    /* priority: sprite 0 in front (pixel colour 1), sprite 1 behind sprite 0 */
    CHECK(pix(b, 62, 70) == 1 && pix(b, 80, 70) == 5, "sprite-sprite priority (%d %d)", pix(b, 62, 70), pix(b, 80, 70));
    /* multicolor x-expanded sprite 2: pairs 00 01 10 11 each 4 pixels wide */
    CHECK(pix(b, 100, 70) == 6 && pix(b, 104, 70) == 9 && pix(b, 108, 70) == 3 && pix(b, 112, 70) == 10 &&
          pix(b, 115, 70) == 10 && pix(b, 116, 70) == 6,
          "mc/x-exp sprite pixels %d %d %d %d", pix(b, 100, 70), pix(b, 104, 70), pix(b, 108, 70), pix(b, 112, 70));
    /* move sprite 0 over the solid char (X 24+160=184, line 51+80=131), behind it */
    wr(b, 10, 30, 0x19, 0x0F);
    wr(b, 10, 32, 0x00, 180); wr(b, 10, 34, 0x01, 128);
    wr(b, 10, 36, 0x1B, 0x01);
    run_frames(b, 2);
    sb = rdreg(b, 10, 20, 0x1F);
    CHECK(sb == 0x01, "sprite-data collision $d01f=%02X (want 01)", sb);
    CHECK(pix(b, 184, 131) == 2 && pix(b, 181, 131) == 1, "MxDP=1: foreground over sprite (%d), sprite over bg (%d)",
          pix(b, 184, 131), pix(b, 181, 131));
    save_crop(b, "sprites_collision");
    free(b);
}

/* ================================================================ T8 */
static void t8_misc(void) {
    printf("T8 DEN latch, idle state, DMA delay\n");
    /* DEN clear during line $30: no badline the whole frame */
    bench_t *b = bench_new();
    setup_text_mem(b);
    basic_regs(b, 0x0B, 0x08, 0x18);
    run_frames(b, 1);
    wr(b, 0x31, 5, 0x11, 0x1B);
    run_to(b, 0, 1);
    CHECK(b->v.last_badlines == 0, "DEN set after line $30 still gave %u badlines", b->v.last_badlines);
    free(b);
    /* DMA delay: badline condition created in cycle 20 of an idle line -> 3 $ff
       c-accesses (AEC high), VC misaligned by 20-14 = 6 characters */
    b = bench_new();
    setup_text_mem(b);
    basic_regs(b, 0x1B, 0x08, 0x18);
    run_frames(b, 1);
    wr(b, 0x30, 1, 0x11, 0x1F);    /* YSCROLL 7: first badline would be $37 */
    wr(b, 0x36, 10, 0x11, 0x18);   /* YSCROLL 0: neither $36 nor $37 match */
    wr(b, 0x37, 19, 0x11, 0x1F);   /* condition from cycle 20 of idle line $37 on */
    run_to(b, 0x38, 1);
    int ba1 = 0, aec1 = 0;
    for (int c = 1; c <= 63; c++) { ba1 += !b->ba[0x37][c]; aec1 += !b->aec[0x37][c]; }
    CHECK(ba1 == 35 && aec1 == 32 && b->ba[0x37][19] && !b->ba[0x37][20] && b->aec[0x37][22] && !b->aec[0x37][23],
          "DMA delay: BA low %d (want 35, from 20), AEC low %d (want 32, from 23)", ba1, aec1);
    CHECK(b->v.cbuf[0] == 0xFF && b->v.cbuf[2] == 0xFF && b->v.cbuf[3] == b->mem[0x400 + 3],
          "DMA delay: first c-accesses %02X %02X %02X (want FF FF ch3)", b->v.cbuf[0], b->v.cbuf[2], b->v.cbuf[3]);
    run_to(b, 0, 1);
    save_crop(b, "dma_delay");
    free(b);
}

/* ================================================================ T9 */
static void perf_cpu(void *bp, int *reg, int *val, int *rd) {
    bench_t *b = bp;
    *rd = 0;
    if (b->v.cycle == 20 && (b->v.line & 3) == 0) { *reg = 0x21; *val = b->v.line & 15; }
}
static void t9_perf(void) {
    printf("T9 performance\n");
    bench_t *b = bench_new();
    setup_text_mem(b);
    basic_regs(b, 0x1B, 0x08, 0x18);
    for (int n = 0; n < 8; n++) { poke(b, 2 * n, 30 + 35 * n); poke(b, 2 * n + 1, 60 + 10 * n); b->mem[0x7F8 + n] = 0xC0; }
    for (int i = 0; i < 63; i++) b->mem[0x3000 + i] = (uint8_t)(i * 29);
    poke(b, 0x15, 0xFF); poke(b, 0x1D, 0xF0); poke(b, 0x17, 0x0F);
    b->cpu = perf_cpu;
    const int frames = 300;
    clock_t t0 = clock();
    run_frames(b, frames);
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    double cps = frames * 19656.0 / s;
    printf("  %d frames (%d cycles, 8 sprites, text, $d021 writes) in %.3f s = %.1f M VIC cycles/s (%.1fx real time)\n",
           frames, frames * 19656, s, cps / 1e6, cps / 985248.0);
    CHECK(cps >= 10e6, "only %.1f M cycles/s", cps / 1e6);
    save_crop(b, "perf_frame");
    free(b);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    t1_badline_dma();
    t2_raster_irq();
    t3_render();
    t4_fpp();
    t5_sprite_stretch_crunch();
    t6_border();
    t7_collisions();
    t8_misc();
    t9_perf();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
