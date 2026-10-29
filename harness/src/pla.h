/* pla.h - C64 PLA (906114-01 / 82S100) and the I/O decoder (74LS139).
 *
 * The PLA logic is a direct transcription of the ABEL equations produced from
 * a dump of a real 906114 PLA (roms/c64pla.txt, Mark Smith / Marko Makela /
 * Jens Schoenfeld).  Every bit of the pin mask is the ELECTRICAL LEVEL on the
 * pin of that name in the equations (1 = high).  Outputs are active low:
 * a cleared output bit means "selected".
 *
 *     pins = pla_tick(pins);     (purely combinational, no state)
 */
#pragma once
#include <stdint.h>

/* inputs */
#define PLA_A12      (1u << 0)
#define PLA_A13      (1u << 1)
#define PLA_A14      (1u << 2)
#define PLA_A15      (1u << 3)
#define PLA__VA14    (1u << 4)    /* CIA2 PA0 level (inverted VA14) */
#define PLA__CHAREN  (1u << 5)    /* 6510 P2 */
#define PLA__HIRAM   (1u << 6)    /* 6510 P1 */
#define PLA__LORAM   (1u << 7)    /* 6510 P0 */
#define PLA__CAS     (1u << 8)    /* RAM CAS strobe, low = access in progress */
#define PLA_VA12     (1u << 9)
#define PLA_VA13     (1u << 10)
#define PLA__GAME    (1u << 11)   /* expansion port, high = no cartridge */
#define PLA__EXROM   (1u << 12)
#define PLA_R__W     (1u << 13)   /* high = read */
#define PLA__AEC     (1u << 14)   /* high = VIC owns the bus (inverted AEC) */
#define PLA_BA       (1u << 15)
/* outputs (active low) */
#define PLA_ROMH     (1u << 16)
#define PLA_ROML     (1u << 17)
#define PLA_I_O      (1u << 18)
#define PLA_GR_W     (1u << 19)
#define PLA_CHAROM   (1u << 20)
#define PLA_KERNAL   (1u << 21)
#define PLA_BASIC    (1u << 22)
#define PLA_CASRAM   (1u << 23)
#define PLA_OUT_MASK (0xFFu << 16)

uint32_t pla_tick(uint32_t pins);

/* 74LS139 I/O decode of $D000-$DFFF (A8..A11), valid when PLA I_O is low */
enum { IO_VIC, IO_SID, IO_COLOR, IO_CIA1, IO_CIA2, IO_IO1, IO_IO2 };
static inline int io_decode(uint16_t addr) {
    switch ((addr >> 10) & 3) {
    case 0: return IO_VIC;
    case 1: return IO_SID;
    case 2: return IO_COLOR;
    default: {
        static const int hi[4] = { IO_CIA1, IO_CIA2, IO_IO1, IO_IO2 };
        return hi[(addr >> 8) & 3];
    }
    }
}
