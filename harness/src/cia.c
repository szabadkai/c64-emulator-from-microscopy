/* cia.c - MOS 6526 CIA: ports, timers A/B with the documented pipeline,
 * ICR/IRQ timing, FLAG edge, PB6/PB7 timer outputs.  TOD registers are
 * stored but do not count; the serial register is stored only.
 *
 * Timer model (sources: W. Lorenz, "A Software Model of the CIA6526",
 * v2.15, 1997; MOS 6526 data sheet).  One cia_tick() = one PHI2 cycle c:
 *
 *   1. register access of cycle c (reads see the counter before this cycle's
 *      clock edge; writes take effect at the end of c),
 *   2. interrupt flip-flop: an ICR flag that was set at the end of c-1 (and
 *      not cleared by a read in c) and is enabled sets ICR bit 7 and pulls
 *      /IRQ low for cycle c+1 ("raised with a delay of one PHI2 clock";
 *      reading the ICR in the cycle where the flag appears returns it without
 *      bit 7 and prevents the interrupt - Lorenz test ICR01),
 *   3. the clock edge at the end of c: counter pipeline.
 *
 * Counter pipeline: a count pulse (PHI2 while started, or a timer-A
 * underflow for timer B in cascade mode) enters stage d1 at the end of the
 * cycle in which it is generated, moves to d2, and decrements the counter at
 * the next edge - "two clocks after bit 0 in CRA has been set the timer will
 * start counting" (reads after STA CRx: keep, keep, count, ...; after a stop:
 * count, count, keep, ...).  When the counter is 0 and another pulse is
 * waiting (always so in PHI2 mode) it underflows: reload from the latch and
 * the waiting pulse is removed.  Hence in PHI2 mode a zero is never read, the
 * latch value is read twice, and the period is latch+1 cycles ($4CC7 -> 19656
 * = one PAL frame).  A force load also reloads and removes the next pulse
 * ("2-1-2-2-1-2").  One-shot: an underflow while the one-shot bit is set, or
 * was set in the previous cycle, clears the start bit and the pipeline.
 * Writing the latch high byte loads the counter only while stopped. */
#include "cia.h"
#include <string.h>

static void timer_init(cia_timer_t *t) {
    memset(t, 0, sizeof(*t));
    t->counter = t->latch = 0xFFFF;
}

void cia_init(cia_t *c) {
    memset(c, 0, sizeof(*c));
    timer_init(&c->ta);
    timer_init(&c->tb);
    c->pa_ext = c->pb_ext = 0xFF;
    c->tod[3] = 0x01;   /* 1:00:00.0 AM */
}

static uint8_t pb_out(const cia_t *c) {
    uint8_t v = (uint8_t)(c->prb | ~c->ddrb);
    if (c->ta.cr & 0x02) {   /* PBON: timer A on PB6 */
        bool o = (c->ta.cr & 0x04) ? c->ta.pb_toggle : c->ta.pb_pulse;
        v = (uint8_t)((v & ~0x40) | (o ? 0x40 : 0));
    }
    if (c->tb.cr & 0x02) {
        bool o = (c->tb.cr & 0x04) ? c->tb.pb_toggle : c->tb.pb_pulse;
        v = (uint8_t)((v & ~0x80) | (o ? 0x80 : 0));
    }
    return v;
}

static uint8_t pa_level(const cia_t *c) { return (uint8_t)((c->pra | ~c->ddra) & c->pa_ext); }
static uint8_t pb_level(const cia_t *c) { return (uint8_t)(pb_out(c) & c->pb_ext); }

static uint8_t reg_read(cia_t *c, int r) {
    switch (r) {
    case 0x0: return pa_level(c);
    case 0x1: return pb_level(c);
    case 0x2: return c->ddra;
    case 0x3: return c->ddrb;
    case 0x4: return c->ta.counter & 0xFF;
    case 0x5: return c->ta.counter >> 8;
    case 0x6: return c->tb.counter & 0xFF;
    case 0x7: return c->tb.counter >> 8;
    case 0x8: case 0x9: case 0xA: case 0xB: return c->tod[r - 8];
    case 0xC: return c->sdr;
    case 0xD: {
        uint8_t v = (uint8_t)(c->icr | (c->irq ? 0x80 : 0));
        c->icr = 0;       /* reading clears all flags and the interrupt flip-flop */
        c->irq = false;
        return v;
    }
    case 0xE: return c->ta.cr;
    default:  return c->tb.cr;
    }
}

static void write_cr(cia_timer_t *t, uint8_t d) {
    if (d & 0x10) t->force_load = true;          /* strobe, not stored */
    if ((d & 0x01) && !(t->cr & 0x01)) t->pb_toggle = true;   /* rising edge of START */
    t->cr = d & 0xEF;
}

static void reg_write(cia_t *c, int r, uint8_t d) {
    switch (r) {
    case 0x0: c->pra = d; break;
    case 0x1: c->prb = d; break;
    case 0x2: c->ddra = d; break;
    case 0x3: c->ddrb = d; break;
    case 0x4: c->ta.latch = (c->ta.latch & 0xFF00) | d; break;
    case 0x5:
        c->ta.latch = (uint16_t)((c->ta.latch & 0x00FF) | (d << 8));
        if (!(c->ta.cr & 1)) c->ta.counter = c->ta.latch;
        break;
    case 0x6: c->tb.latch = (c->tb.latch & 0xFF00) | d; break;
    case 0x7:
        c->tb.latch = (uint16_t)((c->tb.latch & 0x00FF) | (d << 8));
        if (!(c->tb.cr & 1)) c->tb.counter = c->tb.latch;
        break;
    case 0x8: case 0x9: case 0xA: case 0xB:
        if (c->tb.cr & 0x80) c->alarm[r - 8] = d; else c->tod[r - 8] = d;
        break;
    case 0xC: c->sdr = d; break;
    case 0xD:
        if (d & 0x80) c->icr_mask |= d & 0x1F; else c->icr_mask &= (uint8_t)~(d & 0x1F);
        break;
    case 0xE: write_cr(&c->ta, d); break;
    default:  write_cr(&c->tb, d); break;
    }
}

/* clock edge at the end of the cycle; 'pulse' = count pulse generated in this
   cycle (enters the pipeline).  Returns true on underflow. */
static bool timer_edge(cia_timer_t *t, bool pulse) {
    bool P = t->d2, W = t->d1, uf = false;
    t->pb_pulse = false;
    if (t->force_load) {
        t->force_load = false;
        t->counter = t->latch;
        W = false;                         /* the next clock is removed */
    } else {
        if (P) {
            if (t->counter == 0) uf = true;   /* pulse arriving while at zero */
            else t->counter--;
        }
        if (!uf && t->counter == 0 && W) uf = true;   /* reached zero, another clock waiting */
        if (uf) {
            t->counter = t->latch;
            W = false;
        }
    }
    if (uf) {
        t->underflows++;
        t->pb_pulse = true;
        t->pb_toggle = !t->pb_toggle;
        if ((t->cr & 0x08) || t->oneshot_prev) {   /* one-shot: stop immediately */
            t->cr &= (uint8_t)~0x01;
            W = false;
            pulse = false;
        }
    }
    t->d2 = W;
    t->d1 = pulse;
    t->oneshot_prev = (t->cr & 0x08) != 0;
    return uf;
}

uint64_t cia_tick(cia_t *c, uint64_t pins) {
    c->pa_ext = CIA_GET_PA(pins);
    c->pb_ext = CIA_GET_PB(pins);

    /* 1. register access in PHI2 of this cycle */
    if (pins & CIA_CS) {
        int r = (int)(pins & CIA_RS_MASK);
        if (pins & CIA_RW) pins = CIA_SET_D(pins, reg_read(c, r));
        else reg_write(c, r, CIA_GET_D(pins));
    }

    /* 2. interrupt flip-flop from flags set before this cycle's edge */
    if ((c->icr & c->icr_mask & 0x1F) && !c->irq) { c->irq = true; c->irqs++; }

    /* 3. clock edge: timers (timer B may count timer A underflows) */
    bool ta_pulse = (c->ta.cr & 0x01) && !(c->ta.cr & 0x20);          /* PHI2 mode */
    bool ta_uf = timer_edge(&c->ta, ta_pulse);
    int bmode = (c->tb.cr >> 5) & 3;     /* 0 PHI2, 1 CNT, 2 TA underflow, 3 TA uf & CNT (CNT idles high) */
    bool tb_pulse = (c->tb.cr & 0x01) && (bmode == 0 || ((bmode & 2) && ta_uf));
    bool tb_uf = timer_edge(&c->tb, tb_pulse);
    if (ta_uf) c->icr |= 0x01;
    if (tb_uf) c->icr |= 0x02;

    /* FLAG: falling edge of /FLAG sets ICR bit 4 */
    bool flag = (pins & CIA_FLAG) != 0;
    if (flag && !c->flag_prev) c->icr |= 0x10;
    c->flag_prev = flag;

    pins &= ~(CIA_PA_MASK | CIA_PB_MASK | CIA_IRQ);
    pins |= (uint64_t)(uint8_t)(c->pra | ~c->ddra) << CIA_PA_SHIFT;
    pins |= (uint64_t)pb_out(c) << CIA_PB_SHIFT;
    if (c->irq) pins |= CIA_IRQ;
    return pins;
}
