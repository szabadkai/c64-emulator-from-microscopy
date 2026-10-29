/* c64.c - C64 board.  All chip interaction goes through pin masks.
 *
 * One c64_tick() = one PHI2 cycle (1/985248 s):
 *
 *   1. VIC, PHI1 half:   vic_tick(PHI0=0) -> BA, AEC, /IRQ, optional fetch
 *                        address.  Board resolves the VIC address through the
 *                        PLA (_AEC=1, VA12/VA13, _VA14 = CIA2 PA0) to RAM or
 *                        CHAROM, adds the colour-RAM nybble on D8..D11.
 *   2. CPU (6510):       RDY = BA, AEC, /IRQ = VIC|CIA1, /NMI = CIA2|RESTORE,
 *                        /RES, P0..P5 pull-ups, D0..D7 = data of the previous
 *                        read -> m6510_tick -> A, RW, SYNC, D (write), P0..P5.
 *   3. PHI2 bus cycle:   if AEC high the CPU owns the bus: PLA(A12..A15, RW,
 *                        BA, _LORAM/_HIRAM/_CHAREN = P0..P2, _GAME/_EXROM)
 *                        selects RAM / BASIC / KERNAL / CHAROM / I/O; the
 *                        74LS139 splits I/O into VIC, SID, colour RAM, CIA1,
 *                        CIA2, I/O1, I/O2.
 *   4. CIA1, CIA2, SID:  ticked every cycle with /CS, RW, RS, D and port pins.
 *                        CIA1 PA drives keyboard columns, PB reads rows;
 *                        CIA2 PA0/PA1 select the VIC bank.  The SID pin
 *                        wrapper clocks the SID model up to the access cycle.
 *   5. VIC, PHI2 half:   vic_tick(PHI0=1, CS/RW/A0..A5/D, CPUD = CPU data byte
 *                        of this PHI2) -> register data, optional PHI2 fetch
 *                        (c-/s-access, or the re-issued phase-1 address on a
 *                        register-write cycle); the board answers as in 1.
 *   6. The read byte is put on the CPU's D0..D7 for its next tick.
 *
 * A netlist-derived chip replaces one of these *_tick() calls; nothing else
 * in the board needs to change as long as it honours the same pins. */
#include "c64.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ init */
int c64_init(c64_t *m, sid_model model) {
    memset(m, 0, sizeof(*m));
    m6510_init(&m->cpu);
    vic_init(&m->vic);
    cia_init(&m->cia1);
    cia_init(&m->cia2);
    if (sidpins_init(&m->sid, model) != 0) return -1;
    /* typical C64 DRAM power-up pattern: 64 x $00, 64 x $FF, ... */
    for (int i = 0; i < 65536; i++) m->ram[i] = (i & 0x40) ? 0xFF : 0x00;
    for (int i = 0; i < 1024; i++) m->color[i] = (uint8_t)((i * 7) & 0x0F);
    m->cpu_pins = CPU_RDY | CPU_AEC | CPU_RW;
    m->cia1_pins = CIA_PA_MASK | CIA_PB_MASK;
    m->cia2_pins = CIA_PA_MASK | CIA_PB_MASK;
    m->reset_cycles = 8;
    m->first_irq_cycle = m->first_nmi_cycle = -1;
    return 0;
}

void c64_free(c64_t *m) {
    m6510_free(&m->cpu);
    sidpins_free(&m->sid);
}

void c64_use_fast_cpu(c64_t *m) {
    m6510_free(&m->cpu);
    m6510_init_fast(&m->cpu);
}

void c64_use_compiled_cpu(c64_t *m) {
    m6510_free(&m->cpu);
    m6510_init_compiled(&m->cpu);
}

static int load_one(const char *dir, const char *const *names, uint8_t *dst, size_t size,
                    char *found, size_t foundlen) {
    char path[1024];
    for (int i = 0; names[i]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t n = fread(dst, 1, size, f);
        int extra = fgetc(f);
        fclose(f);
        if (n == size && extra == EOF) {
            snprintf(found, foundlen, "%s", path);
            return 0;
        }
    }
    return -1;
}

int c64_load_roms(c64_t *m, const char *dir, char *err, size_t errlen) {
    static const char *const kn[] = { "kernal-901227-03.bin", "kernal.901227-03.bin",
        "kernal-901227-02.bin", "kernal.901227-02.bin", "kernal-901227-01.bin",
        "kernal.901227-01.bin", "kernal.bin", "kernal.rom", "kernal", NULL };
    static const char *const bn[] = { "basic-901226-01.bin", "basic.901226-01.bin",
        "basic.bin", "basic.rom", "basic", NULL };
    static const char *const cn[] = { "chargen-901225-01.bin", "chargen.901225-01.bin",
        "characters-901225-01.bin", "characters.901225-01.bin", "chargen.bin",
        "chargen.rom", "characters.bin", "characters.rom", "chargen", NULL };
    char fk[1100], fb[1100], fc[1100];
    int ek = load_one(dir, kn, m->kernal, 8192, fk, sizeof fk);
    int eb = load_one(dir, bn, m->basic, 8192, fb, sizeof fb);
    int ec = load_one(dir, cn, m->chargen, 4096, fc, sizeof fc);
    if (ek || eb || ec) {
        snprintf(err, errlen,
                 "missing ROM(s) in '%s':%s%s%s\n"
                 "  KERNAL  (8192 bytes): kernal-901227-03.bin | kernal.901227-03.bin | kernal.bin | kernal\n"
                 "  BASIC   (8192 bytes): basic-901226-01.bin | basic.901226-01.bin | basic.bin | basic\n"
                 "  CHARGEN (4096 bytes): chargen-901225-01.bin | characters.901225-01.bin | chargen.bin | chargen",
                 dir, ek ? " KERNAL" : "", eb ? " BASIC" : "", ec ? " CHARGEN" : "");
        return -1;
    }
    snprintf(err, errlen, "ROMs: %s, %s, %s", fk, fb, fc);
    return 0;
}

/* --------------------------------------------------------- memory paths */
static uint8_t cia2_pa(const c64_t *m) { return CIA_GET_PA(m->cia2_pins); }

uint8_t c64_vic_read(c64_t *m, uint16_t va, uint8_t *color) {
    va &= 0x3FFF;
    uint8_t pa = cia2_pa(m);
    uint32_t pin = PLA__AEC | PLA_R__W | PLA__GAME | PLA__EXROM |
                   PLA_A12 | PLA_A13 | PLA_A14 | PLA_A15 |   /* CPU address bus floats high */
                   ((m->vic_pins & VIC_BA) ? PLA_BA : 0) |
                   ((va & 0x1000) ? PLA_VA12 : 0) | ((va & 0x2000) ? PLA_VA13 : 0) |
                   ((pa & 1) ? PLA__VA14 : 0) |
                   PLA__LORAM | PLA__HIRAM | PLA__CHAREN;
    pin = pla_tick(pin);
    uint8_t d;
    if (!(pin & PLA_CHAROM)) d = m->chargen[va & 0x0FFF];
    else if (!(pin & PLA_ROMH)) d = 0xFF;                       /* no cartridge */
    else if (!(pin & PLA_CASRAM)) {
        uint16_t bank = (uint16_t)((~pa & 3) << 14);            /* VA14 = !PA0, VA15 = !PA1 */
        d = m->ram[bank | va];
    } else d = 0xFF;
    if (color) *color = m->color[va & 0x3FF] & 0x0F;
    return d;
}

static uint64_t vic_fetch(c64_t *m, uint64_t vp) {
    uint8_t col;
    uint8_t d = c64_vic_read(m, (uint16_t)(vp & VIC_A_MASK), &col);
    m->open_bus = d;
    return VIC_SET_D(vp, d | (col << 8));
}

/* ------------------------------------------------------------ keyboard */
void c64_key(c64_t *m, int col, int row, bool down) {
    if (down) m->keys[col] |= (uint8_t)(1 << row);
    else m->keys[col] &= (uint8_t)~(1 << row);
}

static uint8_t keyboard_rows(const c64_t *m, uint8_t pa_level) {
    uint8_t rows = 0xFF;
    for (int c = 0; c < 8; c++)
        if (!(pa_level & (1 << c))) rows &= (uint8_t)~m->keys[c];
    return rows;
}

/* (column, row, shift) for ASCII characters; standard C64 matrix
   (column = CIA1 PA bit, row = CIA1 PB bit) */
int c64_char_key(int ch, int *col, int *row, int *shift) {
    static const char *const matrix[8] = {
        /* row:  0    1    2    3    4    5    6    7 */
        "\x08\n\x01\x02\x03\x04\x05\x06",   /* col 0: DEL RETURN CRSR-R F7 F1 F3 F5 CRSR-D */
        "3WA4ZSE\x07",                        /* col 1: ... LSHIFT */
        "5RD6CFTX",
        "7YG8BHUV",
        "9IJ0MKON",
        "+PL-.:@,",
        "\x09*;\x0B\x0C=^/",                  /* col 6: POUND * ; HOME RSHIFT = ^ / */
        ("1\x0E\x0F" "2 \x10Q\x11"),          /* col 7: 1 <- CTRL 2 SPACE C= Q STOP */
    };
    static const struct { char c, base; } shifted[] = {
        { '!', '1' }, { '"', '2' }, { '#', '3' }, { '$', '4' }, { '%', '5' }, { '&', '6' },
        { '\'', '7' }, { '(', '8' }, { ')', '9' }, { '<', ',' }, { '>', '.' }, { '?', '/' },
        { '[', ':' }, { ']', ';' }, { 0, 0 } };
    *shift = 0;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    for (int i = 0; shifted[i].c; i++)
        if (shifted[i].c == ch) { ch = shifted[i].base; *shift = 1; break; }
    for (int c = 0; c < 8; c++)
        for (int r = 0; r < 8; r++)
            if (matrix[c][r] == ch) { *col = c; *row = r; return 0; }
    return -1;
}

void c64_type(c64_t *m, const char *text) {
    int n = 0;
    for (const char *p = text; *p && n < (int)sizeof(m->type_buf) - 1; p++) {
        if (p[0] == '\\' && p[1] == 'n') { m->type_buf[n++] = '\n'; p++; }
        else m->type_buf[n++] = *p;
    }
    m->type_buf[n] = 0;
    m->type_len = n;
    m->type_pos = 0;
    m->type_phase = 0;
    m->type_timer = 0;
}

bool c64_typing(const c64_t *m) { return m->type_pos < m->type_len; }

/* called once per frame: hold each key 3 frames, release 3 frames */
static void typing_frame(c64_t *m) {
    if (!c64_typing(m)) return;
    if (m->type_timer > 0) { m->type_timer--; return; }
    int col, row, shift;
    char ch = m->type_buf[m->type_pos];
    if (c64_char_key(ch, &col, &row, &shift) != 0) { m->type_pos++; return; }
    if (m->type_phase == 0) {
        c64_key(m, col, row, true);
        if (shift) c64_key(m, 1, 7, true);
        m->type_phase = 1;
        m->type_timer = 2;
    } else {
        c64_key(m, col, row, false);
        c64_key(m, 1, 7, false);
        m->type_phase = 0;
        m->type_timer = 2;
        m->type_pos++;
    }
}

/* ----------------------------------------------------------- the cycle */
void c64_tick(c64_t *m) {
    /* ---- 1. VIC PHI1 */
    uint64_t vp = m->vic_pins & ~(VIC_PHI0 | VIC_CS | VIC_RW);
    vp = vic_tick(&m->vic, vp);
    if (vp & VIC_FETCH) vp = vic_fetch(m, vp);
    const bool ba = (vp & VIC_BA) != 0;
    const bool aec = (vp & VIC_AEC) != 0;

    /* ---- 2. CPU */
    uint64_t cp = m->cpu_pins & ~(CPU_RDY | CPU_AEC | CPU_IRQ | CPU_NMI | CPU_RES | CPU_P_MASK);
    if (ba) cp |= CPU_RDY;
    if (aec) cp |= CPU_AEC;
    const bool irq = (vp & VIC_IRQ) || (m->cia1_pins & CIA_IRQ);
    if (irq) cp |= CPU_IRQ;
    const bool nmi = (m->cia2_pins & CIA_IRQ) || m->restore;
    if (nmi) cp |= CPU_NMI;
    if (m->reset_cycles > 0) { cp |= CPU_RES; m->reset_cycles--; }
    cp |= 0x3FULL << CPU_P_SHIFT;   /* pull-ups on P0..P5; P4 = cassette sense (no button) */
    cp = m6510_tick(&m->cpu, cp);

    const uint16_t addr = CPU_GET_ADDR(cp);
    const bool rw = (cp & CPU_RW) != 0;
    const uint8_t port = (uint8_t)((cp >> CPU_P_SHIFT) & 0x3F);

    /* ---- 3. PHI2 bus cycle (CPU side) */
    uint64_t c1 = m->cia1_pins & ~(CIA_CS | CIA_RW | CIA_RS_MASK | CIA_D_MASK);
    uint64_t c2 = m->cia2_pins & ~(CIA_CS | CIA_RW | CIA_RS_MASK | CIA_D_MASK);
    uint64_t sp = 0;
    bool vic_cs = false;
    int dev = -1;                     /* which I/O chip answers a read */
    uint8_t data = m->open_bus;
    const uint8_t wdata = CPU_GET_DATA(cp);

    if (aec) {
        /* _CAS is low (bit clear) while the access is in progress */
        uint32_t pin = (addr & 0x1000 ? PLA_A12 : 0) | (addr & 0x2000 ? PLA_A13 : 0) |
              (addr & 0x4000 ? PLA_A14 : 0) | (addr & 0x8000 ? PLA_A15 : 0) |
              (rw ? PLA_R__W : 0) | (ba ? PLA_BA : 0) |
              (port & 1 ? PLA__LORAM : 0) | (port & 2 ? PLA__HIRAM : 0) |
              (port & 4 ? PLA__CHAREN : 0) | PLA__GAME | PLA__EXROM |
              ((cia2_pa(m) & 1) ? PLA__VA14 : 0);
        pin = pla_tick(pin);

        if (!(pin & PLA_I_O)) {
            dev = io_decode(addr);
            switch (dev) {
            case IO_VIC:
                vic_cs = true;
                break;
            case IO_SID:
                sp |= SIDP_CS | (addr & SIDP_A_MASK) | (rw ? SIDP_RW : 0) | ((uint64_t)wdata << SIDP_D_SHIFT);
                if (!rw) {
                    m->fcur.sid_writes++;
                    if (m->sid_log)
                        fprintf(m->sid_log, "%llu %llu %d %d %02X %02X\n", (unsigned long long)m->cycle,
                                (unsigned long long)m->vic.frame, m->vic.line, m->vic.cycle,
                                (unsigned)(addr & 0x1F), wdata);
                }
                break;
            case IO_COLOR:
                if (!rw && !(pin & PLA_GR_W)) m->color[addr & 0x3FF] = wdata & 0x0F;
                data = (uint8_t)((m->open_bus & 0xF0) | (m->color[addr & 0x3FF] & 0x0F));
                break;
            case IO_CIA1:
                c1 |= CIA_CS | (addr & CIA_RS_MASK) | (rw ? CIA_RW : 0) | ((uint64_t)wdata << CIA_D_SHIFT);
                break;
            case IO_CIA2:
                c2 |= CIA_CS | (addr & CIA_RS_MASK) | (rw ? CIA_RW : 0) | ((uint64_t)wdata << CIA_D_SHIFT);
                break;
            default:  /* I/O1, I/O2: nothing connected, open bus */
                break;
            }
        } else if (!(pin & PLA_KERNAL)) data = m->kernal[addr & 0x1FFF];
        else if (!(pin & PLA_BASIC)) data = m->basic[addr & 0x1FFF];
        else if (!(pin & PLA_CHAROM)) data = m->chargen[addr & 0x0FFF];
        else if (!(pin & (PLA_ROML | PLA_ROMH))) data = 0xFF;   /* no cartridge */
        else if (!(pin & PLA_CASRAM)) {
            if (rw) data = m->ram[addr];
            else m->ram[addr] = wdata;
        }
        /* else: unmapped (Ultimax holes) -> open bus */
    }

    /* ---- 4. CIAs and SID (every cycle) */
    /* CIA1: keyboard.  Rows seen on PB = keys in columns driven low on PA */
    uint8_t pa1 = CIA_GET_PA(m->cia1_pins);
    c1 = (c1 & ~(CIA_PA_MASK | CIA_PB_MASK)) | CIA_PA_MASK |
         ((uint64_t)keyboard_rows(m, pa1) << CIA_PB_SHIFT);
    c1 = cia_tick(&m->cia1, c1);
    if (dev == IO_CIA1 && rw) data = CIA_GET_D(c1);
    m->cia1_pins = c1;

    /* CIA2: PA0/1 VIC bank, PA3..5 IEC outputs (via 7406 inverters),
       PA6/PA7 read the CLK/DATA lines: no devices, so a line is low only
       while the C64 itself pulls it */
    uint8_t pa2 = CIA_GET_PA(m->cia2_pins);
    uint8_t iec_in = 0xFF;
    if (pa2 & 0x10) iec_in &= (uint8_t)~0x40;   /* CLK OUT -> CLK IN */
    if (pa2 & 0x20) iec_in &= (uint8_t)~0x80;   /* DATA OUT -> DATA IN */
    c2 = (c2 & ~(CIA_PA_MASK | CIA_PB_MASK)) | ((uint64_t)iec_in << CIA_PA_SHIFT) | CIA_PB_MASK;
    c2 = cia_tick(&m->cia2, c2);
    if (dev == IO_CIA2 && rw) data = CIA_GET_D(c2);
    m->cia2_pins = c2;

    sp = sidpins_tick(&m->sid, sp);
    if (dev == IO_SID && rw) data = (uint8_t)((sp >> SIDP_D_SHIFT) & 0xFF);

    /* ---- 5. VIC PHI2 */
    vp |= VIC_PHI0;
    if (aec)   /* CPU data byte on the bus in this PHI2 (VIC sees D0..D3 via the 4066) */
        vp |= VIC_CPUD | ((uint64_t)(rw ? data : wdata) << VIC_CPUD_SHIFT);
    if (vic_cs) {
        vp = (vp & ~VIC_A_MASK) | VIC_CS | (addr & 0x3F) | (rw ? VIC_RW : 0);
        if (!rw) vp = VIC_SET_D(vp, wdata);
    }
    vp = vic_tick(&m->vic, vp);
    if (vic_cs && rw) data = (uint8_t)VIC_GET_D(vp);
    if (vp & VIC_FETCH) vp = vic_fetch(m, vp);
    m->vic_pins = vp & ~(VIC_CS | VIC_RW | VIC_CPUD | (0xFFULL << VIC_CPUD_SHIFT));

    /* ---- 6. read data back to the CPU for its next tick */
    if (aec && rw) cp = CPU_SET_DATA(cp, data);
    m->cpu_pins = cp;

    /* ---- statistics, interrupt / watch tracking, trace */
    /* interrupt sequences: a vector fetch right after the stack pushes */
    if (aec && rw && !(cp & CPU_SYNC) && m->last_was_push) {
        if (addr == 0xFFFE) {
            m->irqs_taken++; m->fcur.irqs++;
            if (m->first_irq_cycle < 0) m->first_irq_cycle = (int64_t)m->cycle;
        } else if (addr == 0xFFFA) {
            m->nmis_taken++; m->fcur.nmis++;
            if (m->first_nmi_cycle < 0) m->first_nmi_cycle = (int64_t)m->cycle;
        }
    }
    if (cp & CPU_SYNC) {
        for (int i = 0; i < m->nwatch; i++)
            if (m->watch_cycle[i] < 0 && addr == m->watch_addr[i]) {
                m->watch_cycle[i] = (int64_t)m->cycle;
                m->watch_frame[i] = m->vic.frame;
                m->watch_line[i] = m->vic.line;
                m->watch_lcycle[i] = m->vic.cycle;
            }
    }
    m->last_addr = addr;
    m->last_was_push = aec && !rw && (addr & 0xFF00) == 0x0100;
    m->nmi_prev = nmi;
    if (!ba) { m->fcur.ba_low++; if (rw) { m->fcur.cpu_stalled++; m->total_stalled++; } }
    if (!aec) { m->fcur.vic_stolen++; m->total_stolen++; }
    if (irq) m->fcur.irq_cycles++;
    if (m->trace_left > 0) {
        m->trace_left--;
        fprintf(m->trace_out ? m->trace_out : stdout,
                "%8llu %04X %c %02X %s %s%s%s line=%3d cyc=%2d A=%02X X=%02X Y=%02X S=%02X P=%02X\n",
                (unsigned long long)m->cycle, addr, rw ? 'R' : 'W', CPU_GET_DATA(cp),
                (cp & CPU_SYNC) ? "SYNC" : "    ", ba ? "  " : "BA", aec ? "   " : "AEC",
                irq ? " IRQ" : "    ", m->vic.line, m->vic.cycle,
                m6510_a(&m->cpu), m6510_x(&m->cpu), m6510_y(&m->cpu),
                m6510_s(&m->cpu), m6510_p(&m->cpu));
    }
    m->cycle++;
    if (m->vic.line == 0 && m->vic.cycle == 1) {   /* frame boundary */
        sidpins_sync(&m->sid);                     /* clock the SID to frame end */
        m->fcur.badlines = m->vic.last_badlines;
        m->flast = m->fcur;
        memset(&m->fcur, 0, sizeof m->fcur);
        typing_frame(m);
    }
}

void c64_run_cycles(c64_t *m, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) c64_tick(m);
}

void c64_run_frames(c64_t *m, uint64_t n) {
    uint64_t target = m->vic.frame + n;
    while (m->vic.frame < target) c64_tick(m);
}

/* --------------------------------------------------------- screen text */
static char screencode_to_ascii(uint8_t c) {
    c &= 0x7F;   /* reverse video -> same character */
    if (c == 0) return '@';
    if (c >= 1 && c <= 26) return (char)('A' + c - 1);
    if (c == 27) return '[';
    if (c == 28) return '#';   /* pound sign */
    if (c == 29) return ']';
    if (c == 30) return '^';
    if (c == 31) return '<';   /* left arrow */
    if (c >= 32 && c <= 63) return (char)c;
    return '#';                 /* graphics characters */
}

void c64_screen_text(c64_t *m, char out[25][41]) {
    uint16_t vm = vic_screen_addr(&m->vic);
    for (int r = 0; r < 25; r++) {
        for (int c = 0; c < 40; c++)
            out[r][c] = screencode_to_ascii(c64_vic_read(m, (uint16_t)(vm + r * 40 + c), NULL));
        out[r][40] = 0;
    }
}

bool c64_screen_contains(c64_t *m, const char *s) {
    char t[25][41];
    c64_screen_text(m, t);
    for (int r = 0; r < 25; r++)
        if (strstr(t[r], s)) return true;
    return false;
}
