/* ccpu_shim.c - binds the netcpu_init/tick/free calls of a second, renamed
 * compile of harness/src/port6510.c to the compiled core (board-level
 * lockstep and speed test only; one instance). */
#include <string.h>
#include "ccpu.h"

static ccpu_t g_cc;

void shim_netcpu_init(netcpu_t *c) { memset(c, 0, sizeof(*c)); ccpu_init(&g_cc); }
void shim_netcpu_free(netcpu_t *c) { (void)c; }
uint64_t shim_netcpu_tick(netcpu_t *c, uint64_t pins) { (void)c; return ccpu_tick(&g_cc, pins); }
ccpu_t *ccpu_shim_cpu(void) { return &g_cc; }
