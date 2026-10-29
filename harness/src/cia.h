/* cia.h - MOS 6526 CIA, pin interface, one tick per PHI2 cycle.
 *
 *     pins = cia_tick(&cia, pins);
 *
 * In:  RS0..RS3 (A0..A3), D0..D7 (write data), CIA_CS (1 = /CS low, i.e.
 *      selected), CIA_RW (1 = read), CIA_FLAG (1 = /FLAG low),
 *      PA0..PA7 / PB0..PB7 = level the outside world pulls each port pin to
 *      (1 when nothing pulls it low).
 * Out: D0..D7 (read data when CS & RW), CIA_IRQ (1 = /IRQ asserted during
 *      the NEXT cycle - the output of tick t is the line level in cycle t+1),
 *      PA/PB = the level the CIA drives (output bits, incl. PB6/PB7 timer
 *      outputs) or 1 (inputs, internal pull-up).  PRA/PRB reads return pin levels.
 *
 * Timer pipeline (see cia.c and README "CIA timing" for the sources). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CIA_RS_MASK   0xFULL
#define CIA_D_SHIFT   16
#define CIA_D_MASK    (0xFFULL << CIA_D_SHIFT)
#define CIA_CS        (1ULL << 24)
#define CIA_RW        (1ULL << 25)
#define CIA_IRQ       (1ULL << 26)
#define CIA_FLAG      (1ULL << 27)
#define CIA_PA_SHIFT  32
#define CIA_PB_SHIFT  40
#define CIA_PA_MASK   (0xFFULL << CIA_PA_SHIFT)
#define CIA_PB_MASK   (0xFFULL << CIA_PB_SHIFT)

#define CIA_GET_D(p)  ((uint8_t)(((p) >> CIA_D_SHIFT) & 0xFF))
#define CIA_SET_D(p, d) (((p) & ~CIA_D_MASK) | ((uint64_t)(uint8_t)(d) << CIA_D_SHIFT))
#define CIA_GET_PA(p) ((uint8_t)(((p) >> CIA_PA_SHIFT) & 0xFF))
#define CIA_GET_PB(p) ((uint8_t)(((p) >> CIA_PB_SHIFT) & 0xFF))

typedef struct {
    uint16_t counter, latch;
    uint8_t cr;           /* control register (CRA / CRB), force-load bit never stored */
    bool d1, d2;          /* count pipeline: d1 = clock waiting, d2 = clock decrementing now */
    bool force_load;      /* force-load strobe written this cycle */
    bool oneshot_prev;    /* one-shot bit at the end of the previous cycle */
    bool pb_toggle;       /* PB6/PB7 toggle flip-flop */
    bool pb_pulse;        /* PB6/PB7 one-cycle underflow pulse */
    bool start_prev;      /* start bit at the end of the previous cycle (edge detect) */
    uint64_t underflows;
} cia_timer_t;

typedef struct {
    uint8_t pra, prb, ddra, ddrb;
    uint8_t pa_ext, pb_ext;
    cia_timer_t ta, tb;
    uint8_t icr, icr_mask;    /* latched interrupt sources / enable mask */
    bool irq;                 /* interrupt flip-flop (ICR bit 7, /IRQ line) */
    bool flag_prev;
    uint8_t sdr;
    uint8_t tod[4], alarm[4];
    uint64_t irqs;
} cia_t;

void     cia_init(cia_t *c);
uint64_t cia_tick(cia_t *c, uint64_t pins);
