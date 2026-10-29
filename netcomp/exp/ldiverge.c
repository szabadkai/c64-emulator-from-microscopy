/* run ccpu (exact) and lcpu side by side without resync on random soup;
   report the instruction at the first bus divergence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ccpu.h"
#include "lcpu.h"
static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return rs * 0x2545F4914F6CDD1DULL; }
static const char *DOC = "1100011011100110110001101100011011001110111011101100011011000110110001101110111011000110110001101100011011101110110001101100011001001110101011101100111011100100111011101110111011001110111011101100111011101110110001101100011011001110111011101100011011000110";
static int documented(uint8_t op) { return DOC[op] == '1'; }
static int is_jam(uint8_t op) { return (op & 0x1F) == 0x12 || op == 0x02 || op == 0x22 || op == 0x42 || op == 0x62; }
int main(int argc, char **argv) {
    int runs = argc > 1 ? atoi(argv[1]) : 20;
    long maxc = argc > 2 ? atol(argv[2]) : 200000;
    int ops[256] = {0};
    static ccpu_t C; static lcpu_t L; static uint8_t mem[65536];
    for (int r = 0; r < runs; r++) {
        rs = 1000 + r * 7919;
        for (int i = 0; i < 65536; i++) { uint8_t v; do v = (uint8_t)(rnd() >> 40); while (is_jam(v) || (getenv("DOC") && !documented(v))); mem[i] = v; }
        ccpu_init(&C); lcpu_init(&L);
        uint64_t pc_in = CPU_RW | CPU_RDY, pl_in = CPU_RW | CPU_RDY;
        uint8_t lastop = 0; uint64_t hist[8] = {0};
        long cyc;
        for (cyc = 0; cyc < maxc; cyc++) {
            uint64_t ctl = CPU_RDY; if (cyc < 3) ctl |= CPU_RES;
            uint64_t oc = ccpu_tick(&C, (pc_in & ~(CPU_RES)) | ctl), ol = lcpu_tick(&L, (pl_in & ~CPU_RES) | ctl);
            const uint64_t m = CPU_A_MASK | CPU_RW | CPU_SYNC | CPU_D_MASK;
            uint64_t mc = (oc & CPU_RW) ? (oc & (CPU_A_MASK | CPU_RW | CPU_SYNC)) : (oc & m);
            uint64_t ml = (ol & CPU_RW) ? (ol & (CPU_A_MASK | CPU_RW | CPU_SYNC)) : (ol & m);
            if (mc != ml) {
                ops[lastop]++;
                if (r < 6) { printf("run %d: diverge at cycle %ld in opcode %02X; last bus:", r, cyc, lastop);
                    for (int k = 0; k < 8; k++) printf(" %04X%c", CPU_GET_ADDR(hist[(cyc + k) & 7]), (hist[(cyc + k) & 7] & CPU_SYNC) ? '*' : ' ');
                    printf("\n"); }
                break;
            }
            hist[cyc & 7] = oc;
            uint16_t a = CPU_GET_ADDR(oc);
            if (oc & CPU_RW) { uint8_t v = mem[a]; if ((oc & CPU_SYNC) && is_jam(v)) v = 0xEA; if (oc & CPU_SYNC) lastop = v;
                pc_in = CPU_SET_DATA(oc, v); pl_in = CPU_SET_DATA(ol, v); }
            else { mem[a] = CPU_GET_DATA(oc); pc_in = oc; pl_in = ol; }
        }
        if (cyc == maxc) printf("run %d: no divergence in %ld cycles\n", r, maxc);
    }
    printf("divergence opcodes:"); for (int i = 0; i < 256; i++) if (ops[i]) printf(" %02X:%d", i, ops[i]); printf("\n");
    return 0;
}
