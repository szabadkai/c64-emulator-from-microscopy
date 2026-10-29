/* first lcpu-vs-ccpu mismatching recalc: dump before/after states */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ccpu.h"
#include "lcpu.h"
static uint64_t rs = 7;
static uint64_t rnd(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return rs * 0x2545F4914F6CDD1DULL; }
static uint8_t before[CC_NN];
static ccpu_t C; static lcpu_t L;
static int cmp(const char *ph, long cyc, int which) {
    int bad = 0;
    for (int n = 0; n < CC_NN; n++) if (ccpu_node(&C, n) != lcpu_node(&L, n)) bad++;
    if (!bad) return 0;
    static int dumped; int maxd = getenv("NDUMP") ? atoi(getenv("NDUMP")) : 1;
    static FILE *f; if (!f) f = fopen("build/ldebug.txt", "w"); if (!f) { perror("build/ldebug.txt"); exit(1); }
    fprintf(f, "phase %d %s cycle %ld\n", which, ph, cyc);
    for (int n = 0; n < CC_NN; n++) fprintf(f, "%d %d %d %d %d\n", n, before[n], ccpu_node(&C, n), lcpu_node(&L, n), (C.f[n] >> 1) & 3);
    fprintf(f, "end\n");
    lcpu_set_nodes(&L, C.f, C.f);
    if (++dumped >= maxd) { fclose(f); printf("dumped %d mismatch events\n", dumped); exit(0); }
    return 0;
}
int main(void) {
    static uint8_t mem[65536]; for (int i = 0; i < 65536; i++) mem[i] = (uint8_t)(rnd() >> 40);
    ccpu_init(&C); lcpu_init(&L);
    uint64_t pins = CPU_RW | CPU_RDY;
    static const int16_t db[8] = CC_BUS_DB;
    for (long cyc = 0; cyc < 2000000; cyc++) {
        uint64_t ctl = CPU_RDY; if (cyc < 3) ctl |= CPU_RES;
        if (rnd() % 300 == 0) ctl |= CPU_IRQ; if (rnd() % 100 == 0) ctl &= ~CPU_RDY;
        pins = (pins & ~(CPU_IRQ|CPU_NMI|CPU_RES|CPU_RDY)) | ctl;
        if (C.prev_read) { int d = CPU_GET_DATA(pins); if (d != C.db_driven) {
            for (int n = 0; n < CC_NN; n++) before[n] = C.f[n] & 1;
            for (int i = 0; i < 8; i++) ccpu_drive(&C, db[i], (d >> i) & 1); C.db_driven = d; ccpu_recalc(&C);
            lcpu_step_db(&L, d); cmp("db", cyc, 2); } }
        for (int n = 0; n < CC_NN; n++) before[n] = C.f[n] & 1;
        ccpu_drive(&C, CC_N_CLK0, 0); ccpu_recalc(&C); lcpu_step_phi1(&L); cmp("phi1", cyc, 0);
        for (int n = 0; n < CC_NN; n++) before[n] = C.f[n] & 1;
        ccpu_drive(&C, CC_N_RDY, (pins & CPU_RDY) ? 1 : 0); ccpu_drive(&C, CC_N_IRQ, (pins & CPU_IRQ) ? 0 : 1);
        ccpu_drive(&C, CC_N_NMI, (pins & CPU_NMI) ? 0 : 1); ccpu_drive(&C, CC_N_RES, (pins & CPU_RES) ? 0 : 1); ccpu_recalc(&C);
        lcpu_step_ctl(&L, pins); cmp("ctl", cyc, 0);
        for (int n = 0; n < CC_NN; n++) before[n] = C.f[n] & 1;
        ccpu_drive(&C, CC_N_CLK0, 1); ccpu_recalc(&C); lcpu_step_phi2(&L); cmp("phi2", cyc, 1);
        uint64_t o = lcpu_outputs(&L, pins);
        static const int16_t ab[16] = CC_BUS_AB; uint16_t a = 0; for (int i = 0; i < 16; i++) a |= ccpu_node(&C, ab[i]) << i;
        int rw = ccpu_node(&C, CC_N_RW);
        if (rw) pins = CPU_SET_DATA(o, mem[a]); else { uint8_t d = 0; for (int i = 0; i < 8; i++) d |= ccpu_node(&C, db[i]) << i; mem[a] = d; C.db_driven = -1; pins = o; }
        C.prev_read = rw;
    }
    printf("no mismatch\n");
    return 0;
}
