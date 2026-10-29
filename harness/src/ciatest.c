/* ciatest.c - checks of the 6526 timer / ICR pipeline against the behaviour
 * documented by W. Lorenz ("A Software Model of the CIA6526", 1997: start
 * delay, PHI2 read pattern "2-1-2-2-1-2", ICR01, ONESHOT, LOADTH, IMR,
 * cascade "2-2-2-1-1-1-0-0-2") and the data sheet.  Drives cia_tick()
 * through its pins, one call per cycle. */
#include "ciatest.h"
#include <stdio.h>
#include <string.h>
#include "cia.h"

static int fails, checks;
static uint64_t last_pins;

static uint64_t cyc(cia_t *c, uint64_t in) {
    last_pins = cia_tick(c, in | CIA_PA_MASK | CIA_PB_MASK);
    return last_pins;
}
static uint8_t rd(cia_t *c, int r) { return CIA_GET_D(cyc(c, CIA_CS | CIA_RW | (uint64_t)r)); }
static void wr(cia_t *c, int r, uint8_t v) { cyc(c, CIA_CS | (uint64_t)r | ((uint64_t)v << CIA_D_SHIFT)); }
static void idle(cia_t *c, int n) { while (n-- > 0) cyc(c, 0); }
static bool irq(void) { return (last_pins & CIA_IRQ) != 0; }

static void check(int verbose, const char *what, long got, long want) {
    checks++;
    if (got != want) fails++;
    if (verbose || got != want)
        printf("    %s %-58s want %5ld got %5ld\n", got == want ? "ok  " : "FAIL", what, want, got);
}

static void setup_ta(cia_t *c, uint16_t latch) {
    cia_init(c);
    wr(c, 4, latch & 0xFF);
    wr(c, 5, latch >> 8);      /* stopped: high byte also loads the counter */
}

int ciatest_run(int verbose) {
    cia_t c;
    fails = checks = 0;

    printf("[start delay] STA CRA ($01) then read TA every cycle\n");
    setup_ta(&c, 100);
    wr(&c, 0xE, 0x01);
    check(verbose, "read 1 cycle after the write (keep)", rd(&c, 4), 100);
    check(verbose, "read 2 cycles after (keep)", rd(&c, 4), 100);
    check(verbose, "read 3 cycles after (count)", rd(&c, 4), 99);
    check(verbose, "read 4 cycles after (count)", rd(&c, 4), 98);

    printf("[stop delay] STA CRA ($00) on a running timer\n");
    { uint8_t v = rd(&c, 4); wr(&c, 0xE, 0x00);
      check(verbose, "read 1 after stop (count)", rd(&c, 4), v - 2);
      check(verbose, "read 2 after stop (count)", rd(&c, 4), v - 3);
      check(verbose, "read 3 after stop (keep)", rd(&c, 4), v - 3);
      check(verbose, "read 4 after stop (keep)", rd(&c, 4), v - 3); }

    printf("[PHI2 continuous, latch 2] read pattern 2-2-1 (no zero), period latch+1\n");
    setup_ta(&c, 2);
    wr(&c, 0xE, 0x01);
    idle(&c, 3);
    { int seq[9]; for (int i = 0; i < 9; i++) seq[i] = rd(&c, 4);
      int zeros = 0, ones = 0; for (int i = 0; i < 9; i++) { zeros += seq[i] == 0; ones += seq[i] == 1; }
      check(verbose, "zeros read in 9 cycles", zeros, 0);
      check(verbose, "ones read in 9 cycles (one per period of 3)", ones, 3); }

    printf("[period $4CC7] underflows of a continuous PHI2 timer\n");
    setup_ta(&c, 0x4CC7);
    wr(&c, 0xE, 0x11);   /* start + force load (as the intro does) */
    { uint64_t u0 = c.ta.underflows; long first = -1, second = -1;
      for (long i = 1; i < 3 * 19656 && second < 0; i++) {
          idle(&c, 1);
          if (c.ta.underflows != u0) { if (first < 0) first = i; else second = i; u0 = c.ta.underflows; }
      }
      check(verbose, "cycles between underflows ($4CC7 + 1 = one PAL frame)", second - first, 19656);
      check(verbose, "first underflow L+1 cycles after the STA (start delay 2)", first, 0x4CC7 + 1); }

    printf("[ICR01] reading the ICR around the underflow (latch 4, IMR TA)\n");
    for (int k = -1; k <= 1; k++) {
        setup_ta(&c, 4);
        wr(&c, 0xD, 0x81);
        wr(&c, 0xE, 0x09 | 0x10);         /* one-shot + start + force load */
        /* find the cycle t at which the ICR flag becomes visible */
        cia_t probe = c; long t = 0;
        for (long i = 1; i < 20; i++) { cia_t q = probe; if (rd(&q, 0xD) & 1) { t = i; break; } idle(&probe, 1); }
        idle(&c, (int)(t - 1 + k));
        bool seen = irq();               /* line level during the read cycle */
        uint8_t v = rd(&c, 0xD);
        for (int i = 0; i < 3; i++) { idle(&c, 1); seen |= irq(); }
        char lbl[64];
        snprintf(lbl, sizeof lbl, "ICR read at t%+d returns", k);
        check(verbose, lbl, v, k < 0 ? 0x00 : k == 0 ? 0x01 : 0x81);
        snprintf(lbl, sizeof lbl, "ICR read at t%+d: interrupt line asserted", k);
        check(verbose, lbl, seen, k != 0);
    }

    printf("[ONESHOT] CRA around the underflow, timer stops\n");
    setup_ta(&c, 4);
    wr(&c, 0xE, 0x19);
    { cia_t probe = c; long t = 0;
      for (long i = 1; i < 20; i++) { cia_t q = probe; if (rd(&q, 0xD) & 1) { t = i; break; } idle(&probe, 1); }
      idle(&c, (int)(t - 2));
      check(verbose, "CRA at t-1", rd(&c, 0xE), 0x09);
      check(verbose, "CRA at t", rd(&c, 0xE), 0x08);
      idle(&c, 5);
      check(verbose, "counter after one-shot underflow (reloaded, stopped)", rd(&c, 4), 4); }

    printf("[force load] running timer, latch 50, STA CRA $11\n");
    setup_ta(&c, 50);
    wr(&c, 0xE, 0x01);
    idle(&c, 10);
    wr(&c, 0xE, 0x11);
    check(verbose, "read 1 after force load", rd(&c, 4), 50);
    check(verbose, "read 2 (next clock removed)", rd(&c, 4), 50);
    check(verbose, "read 3", rd(&c, 4), 49);

    printf("[LOADTH] latch high byte loads the counter only while stopped\n");
    setup_ta(&c, 0x1234);
    wr(&c, 4, 0x00);
    check(verbose, "low byte write while stopped: counter unchanged", rd(&c, 4), 0x34);
    wr(&c, 5, 0x10);
    check(verbose, "high byte write while stopped: counter = latch", rd(&c, 5) << 8 | rd(&c, 4), 0x1000);
    wr(&c, 0xE, 0x01);
    idle(&c, 5);
    { uint8_t hi = rd(&c, 5); wr(&c, 5, 0x30);
      check(verbose, "high byte write while running: counter keeps counting", rd(&c, 5), hi); }

    printf("[IMR] enabling a pending source raises the IRQ, clearing IMR does not drop it\n");
    setup_ta(&c, 3);
    wr(&c, 0xE, 0x19);
    idle(&c, 12);
    check(verbose, "flag pending, masked: no IRQ", irq(), 0);
    wr(&c, 0xD, 0x81);
    idle(&c, 1);
    check(verbose, "IRQ after setting the IMR bit", irq(), 1);
    wr(&c, 0xD, 0x01);
    idle(&c, 1);
    check(verbose, "still asserted after clearing the IMR bit", irq(), 1);
    check(verbose, "ICR read", rd(&c, 0xD), 0x81);
    check(verbose, "released after the ICR read", irq(), 0);

    printf("[cascade] TB counts TA underflows, both latches 2\n");
    cia_init(&c);
    wr(&c, 4, 2); wr(&c, 5, 0); wr(&c, 6, 2); wr(&c, 7, 0);
    wr(&c, 0xF, 0x41);
    wr(&c, 0xE, 0x01);
    idle(&c, 10);
    { int zeros = 0, twos = 0; for (int i = 0; i < 18; i++) { int v = rd(&c, 6); zeros += v == 0; twos += v == 2; }
      check(verbose, "TB reads 0 (zeros visible only in cascade mode), per 2 periods", zeros, 4);
      check(verbose, "TB reads 2, per 2 periods (latch value plus reload)", twos, 8); }

    printf("CIA tests: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
