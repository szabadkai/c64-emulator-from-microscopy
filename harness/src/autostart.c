/* autostart.c - PRG injection (like VICE "-autostartprgmode 1") and a PSID
 * player: a tiny self-written driver placed in RAM, started by hijacking the
 * KERNAL IRQ vector ($0314) once BASIC is at READY.  Both run on the real
 * machine (netlist CPU, KERNAL banked in); the board only writes RAM. */
#include <stdio.h>
#include <string.h>
#include "c64.h"

static void put16(uint8_t *ram, uint16_t a, uint16_t v) {
    ram[a] = (uint8_t)v;
    ram[(uint16_t)(a + 1)] = (uint8_t)(v >> 8);
}

int c64_inject_prg(c64_t *m, const uint8_t *prg, size_t len, uint16_t *start, uint16_t *end,
                   char *msg, size_t msglen) {
    if (len < 3) { snprintf(msg, msglen, "PRG too short (%zu bytes)", len); return -1; }
    uint16_t load = (uint16_t)(prg[0] | (prg[1] << 8));
    size_t n = len - 2;
    if (load + n > 0x10000) { snprintf(msg, msglen, "PRG $%04X+%zu runs past $FFFF", load, n); return -1; }
    memcpy(m->ram + load, prg + 2, n);
    uint16_t e = (uint16_t)(load + n);
    /* BASIC pointers exactly as LOAD leaves them: TXTTAB ($2B/$2C) unchanged,
       VARTAB/ARYTAB/STREND ($2D/$2F/$31) = end, $AE/$AF = end address */
    put16(m->ram, 0x2D, e);
    put16(m->ram, 0x2F, e);
    put16(m->ram, 0x31, e);
    put16(m->ram, 0xAE, e);
    *start = load;
    *end = e;
    snprintf(msg, msglen, "PRG $%04X-$%04X (%zu bytes) injected, $2D-$32/$AE = $%04X", load,
             (uint16_t)(e - 1), n, e);
    return 0;
}

/* ------------------------------------------------------------------ PSID */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

typedef struct { uint8_t *ram; uint16_t pc; } emit_t;
static void e8(emit_t *e, uint8_t b) { e->ram[e->pc++] = b; }
static void e16(emit_t *e, uint8_t op, uint16_t a) { e8(e, op); e8(e, (uint8_t)a); e8(e, (uint8_t)(a >> 8)); }

#define PSID_RASTER_LINE 0x30   /* VBI mode: play is called at raster line 48 */

int c64_install_psid(c64_t *m, const uint8_t *f, size_t len, int song_req, char *msg, size_t msglen) {
    if (len < 0x76 || (memcmp(f, "PSID", 4) && memcmp(f, "RSID", 4))) {
        snprintf(msg, msglen, "not a PSID/RSID file");
        return -1;
    }
    if (!memcmp(f, "RSID", 4)) {
        snprintf(msg, msglen, "RSID (real C64 environment) tunes are not supported by this driver");
        return -1;
    }
    uint16_t version = be16(f + 4), off = be16(f + 6), load = be16(f + 8), init = be16(f + 10);
    uint16_t play = be16(f + 12), songs = be16(f + 14), start = be16(f + 16);
    uint32_t speed = be32(f + 18);
    uint16_t flags = (version >= 2 && len >= 0x78) ? be16(f + 0x76) : 0;
    if (off > len) { snprintf(msg, msglen, "bad data offset"); return -1; }
    const uint8_t *data = f + off;
    size_t dlen = len - off;
    if (load == 0) {
        if (dlen < 2) { snprintf(msg, msglen, "no load address"); return -1; }
        load = (uint16_t)(data[0] | (data[1] << 8));
        data += 2;
        dlen -= 2;
    }
    if (init == 0) init = load;
    int song = song_req > 0 ? song_req : (start ? start : 1);
    if (songs && song > songs) song = songs;
    int cia = (speed >> ((song - 1) > 31 ? 31 : (song - 1))) & 1;
    uint32_t tend = (uint32_t)load + (uint32_t)dlen;
    if (tend > 0xA000) {
        snprintf(msg, msglen, "tune $%04X-$%04X reaches into BASIC/I/O/KERNAL; this driver keeps the "
                 "ROMs banked in and only supports tunes below $A000", load, (unsigned)(tend - 1));
        return -1;
    }
    /* driver location: cassette buffer, else $C000 */
    uint16_t base = 0x0334;
    if (load < 0x0400 && tend > 0x0334) base = 0xC000;
    if (base == 0xC000 && load < 0xC100 && tend > 0xC000) {
        snprintf(msg, msglen, "no free RAM for the driver");
        return -1;
    }
    memcpy(m->ram + load, data, dlen);

    emit_t e = { m->ram, base };
    /* boot: entered through ($0314) from the KERNAL IRQ entry (A/X/Y pushed) */
    uint16_t pirq_lo_at, pirq_hi_at;
    e8(&e, 0xA9); pirq_lo_at = e.pc; e8(&e, 0);          /* LDA #<pirq */
    e16(&e, 0x8D, 0x0314);                                /* STA $0314  */
    e8(&e, 0xA9); pirq_hi_at = e.pc; e8(&e, 0);          /* LDA #>pirq */
    e16(&e, 0x8D, 0x0315);                                /* STA $0315  */
    if (!play) {                                          /* tune installs its own IRQ */
        e8(&e, 0xA9); e8(&e, 0x31); e16(&e, 0x8D, 0x0314);   /* restore $EA31 */
        e8(&e, 0xA9); e8(&e, 0xEA); e16(&e, 0x8D, 0x0315);
    } else if (!cia) {                                    /* VBI: raster IRQ, CIA1 IRQs off */
        e8(&e, 0xA9); e8(&e, 0x7F); e16(&e, 0x8D, 0xDC0D);   /* LDA #$7F, STA $DC0D */
        e16(&e, 0xAD, 0xDC0D);                                /* LDA $DC0D (ack)     */
        e8(&e, 0xA9); e8(&e, PSID_RASTER_LINE); e16(&e, 0x8D, 0xD012);
        e16(&e, 0xAD, 0xD011); e8(&e, 0x29); e8(&e, 0x7F); e16(&e, 0x8D, 0xD011);
        e8(&e, 0xA9); e8(&e, 0x01); e16(&e, 0x8D, 0xD01A); e16(&e, 0x8D, 0xD019);
    }
    e8(&e, 0xA9); e8(&e, (uint8_t)(song - 1));          /* LDA #song-1 */
    e8(&e, 0xA2); e8(&e, 0x00);                           /* LDX #0      */
    e8(&e, 0xA0); e8(&e, 0x00);                           /* LDY #0      */
    e16(&e, 0x20, init);                                  /* JSR init    */
    e16(&e, 0x4C, (play && !cia) ? 0xEA81 : 0xEA31);      /* finish this IRQ */
    uint16_t pirq = e.pc;
    if (play && !cia) {
        e8(&e, 0xA9); e8(&e, 0x01); e16(&e, 0x8D, 0xD019);   /* ack raster IRQ */
        e16(&e, 0x20, play);
        e16(&e, 0x4C, 0xEA81);                                /* PLA/TAY/PLA/TAX/PLA/RTI */
    } else if (play) {
        e16(&e, 0x20, play);
        e16(&e, 0x4C, 0xEA31);                                /* KERNAL: keys, ack CIA1, RTI */
    } else {
        e16(&e, 0x4C, 0xEA31);
    }
    m->ram[pirq_lo_at] = (uint8_t)pirq;
    m->ram[pirq_hi_at] = (uint8_t)(pirq >> 8);
    put16(m->ram, 0x0314, base);                          /* next KERNAL IRQ enters the driver */

    char name[33] = { 0 }, author[33] = { 0 };
    memcpy(name, f + 0x16, 32);
    memcpy(author, f + 0x36, 32);
    snprintf(msg, msglen, "PSID v%u \"%s\" by %s: data $%04X-$%04X, init $%04X, play $%04X, song %d/%u, "
             "speed %s, flags $%04X (%s, %s); driver at $%04X-$%04X via ($0314)",
             version, name, author, load, (unsigned)(tend - 1), init, play, song, songs,
             cia ? "CIA1 timer (KERNAL 60 Hz)" : "VBI (raster IRQ line 48, 50 Hz)", flags,
             ((flags >> 2) & 3) == 1 ? "PAL" : ((flags >> 2) & 3) == 2 ? "NTSC" : "any video",
             ((flags >> 4) & 3) == 1 ? "6581" : ((flags >> 4) & 3) == 2 ? "8580" : "any SID",
             base, (uint16_t)(e.pc - 1));
    return 0;
}
