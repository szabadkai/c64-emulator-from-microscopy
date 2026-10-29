#!/usr/bin/env python3
"""Make build/ordersim.c from harness/src/netsim.c: the same engine, with one
switch (osim_policy) that changes only the order in which the nodes queued
for a pass are processed:
  0  netsim's own order (the file unchanged, apart from renaming)
  1  random permutation of every pass
  2  every pass reversed
Everything else (group rule, pruning, passes) is netsim's.  Used by
exp/orderexp.c to show that netsim's stable state depends on that order.

usage: python3 -I tools/mkordersim.py ../harness/src/netsim.c build/ordersim.c
"""
import sys

src = open(sys.argv[1]).read()
hook = '#define LOOP_LIMIT 1000'
assert hook in src
src = src.replace(hook, hook + '''
int osim_policy = 0;
static uint64_t orng = 88172645463325252ULL;
static inline uint32_t orand(void) { orng ^= orng << 13; orng ^= orng >> 7; orng ^= orng << 17; return (uint32_t)(orng >> 11); }
void osim_seed(uint64_t s) { orng = s * 0x9E3779B97F4A7C15ULL + 1; }
''')
anchor = '''        uint32_t pass_start = s->gstamp;
        s->st.passes++;'''
assert anchor in src
src = src.replace(anchor, anchor + '''
        if (osim_policy == 1) {
            for (int i = s->nin - 1; i > 0; i--) {
                int j = (int)(orand() % (uint32_t)(i + 1));
                uint16_t t = s->listin[i]; s->listin[i] = s->listin[j]; s->listin[j] = t;
            }
        } else if (osim_policy == 2) {
            for (int i = 0, j = s->nin - 1; i < j; i++, j--) {
                uint16_t t = s->listin[i]; s->listin[i] = s->listin[j]; s->listin[j] = t;
            }
        }''')
open(sys.argv[2], "w").write(src)
