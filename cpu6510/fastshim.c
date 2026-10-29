/* fastshim.c - binds the netcpu_init/tick/free calls of a second, renamed
 * compile of harness/src/port6510.c to fastcpu (board-level lockstep only;
 * one instance). */
#include <string.h>
#include "fastcpu.h"

static fastcpu_t g_fast;

void shim_netcpu_init(netcpu_t *c) { memset(c, 0, sizeof(*c)); fastcpu_init(&g_fast); }
void shim_netcpu_free(netcpu_t *c) { (void)c; }
uint64_t shim_netcpu_tick(netcpu_t *c, uint64_t pins) { (void)c; return fastcpu_tick(&g_fast, pins); }
fastcpu_t *fastshim_cpu(void) { return &g_fast; }
