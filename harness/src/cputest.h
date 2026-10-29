/* cputest.h - netlist CPU validation and benchmark on flat memory */
#pragma once
#include <stdint.h>

int    cputest_run(int verbose);        /* 0 = all checks passed */
double cputest_bench(uint64_t cycles);  /* returns cycles/second */
