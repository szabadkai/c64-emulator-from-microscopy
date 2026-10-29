/* state.c - savestates of the complete machine.
 *
 * File layout (native endian, tied to this build):
 *   "C64NETST" | u32 version | u32 sizeof(c64_t) sizeof(vic_t) sizeof(cia_t) sizeof(sid)
 *   u64 len + netlist engine state   (netsim_state_save: node values, pulls,
 *                                     rail counts, conducting transistors and
 *                                     the order of the live adjacency lists)
 *   u64 len + SID state              (sid_state_save: voices, envelopes,
 *                                     filter, resampler history, pending samples)
 *   u64 len + board struct c64_t     (RAM, colour RAM, ROMs, 6510 core
 *                                     bookkeeping + port, VIC-II, both CIAs,
 *                                     SID wrapper counters, all pin masks,
 *                                     keyboard/typing state, cycle counter)
 * Pointers inside c64_t (netlist instance, SID tables, output sinks) are not
 * saved; the loading instance keeps its own. */
#include <stdlib.h>
#include <string.h>
#include "c64.h"

#define STATE_MAGIC "C64NETST"
#define STATE_VERSION 1u

static void scrub(c64_t *b) {   /* clear pointers / run-time sinks in a copy */
    b->cpu.cpu.sim = NULL;
    memset(&b->sid.chip, 0, sizeof b->sid.chip);
    b->sid.buf = NULL;
    b->sid.cap = 0;
    b->sid.wav = NULL;
    b->sid.on_samples = NULL;
    b->sid.on_samples_ctx = NULL;
    b->trace_left = 0;
    b->trace_out = NULL;
    b->sid_log = NULL;
}

static int put_blob(FILE *f, const void *p, uint64_t n) {
    return fwrite(&n, sizeof n, 1, f) == 1 && (n == 0 || fwrite(p, 1, n, f) == n) ? 0 : -1;
}

static void *get_blob(FILE *f, uint64_t *n) {
    if (fread(n, sizeof *n, 1, f) != 1 || *n > (1ull << 30)) return NULL;
    void *p = malloc(*n ? *n : 1);
    if (p && *n && fread(p, 1, *n, f) != *n) { free(p); return NULL; }
    return p;
}

int c64_save_state(c64_t *m, const char *path, char *msg, size_t msglen) {
    sidpins_sync(&m->sid);   /* chip clocked up to "now"; audio flushed */
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(msg, msglen, "cannot write %s", path); return -1; }
    uint32_t hdr[5] = { STATE_VERSION, (uint32_t)sizeof(c64_t), (uint32_t)sizeof(vic_t),
                        (uint32_t)sizeof(cia_t), (uint32_t)sizeof(sid) };
    int err = fwrite(STATE_MAGIC, 1, 8, f) != 8 || fwrite(hdr, sizeof hdr, 1, f) != 1;

    size_t nn = m->cpu.core != M6510_NETLIST ? 0 : netsim_state_size(m->cpu.cpu.sim);
    void *nb = malloc(nn ? nn : 1);
    if (nn) netsim_state_save(m->cpu.cpu.sim, nb);
    err |= put_blob(f, nb, nn);
    free(nb);

    size_t sn = sid_state_size(&m->sid.chip);
    void *sb = malloc(sn);
    sid_state_save(&m->sid.chip, sb);
    err |= put_blob(f, sb, sn);
    free(sb);

    c64_t *b = malloc(sizeof *b);
    memcpy(b, m, sizeof *b);
    scrub(b);
    err |= put_blob(f, b, sizeof *b);
    free(b);
    err |= fclose(f) != 0;
    snprintf(msg, msglen, err ? "error writing %s" : "state saved to %s (cycle %llu, frame %llu)", path,
             (unsigned long long)m->cycle, (unsigned long long)m->vic.frame);
    return err ? -1 : 0;
}

int c64_load_state(c64_t *m, const char *path, char *msg, size_t msglen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, msglen, "cannot read %s", path); return -1; }
    char magic[8];
    uint32_t hdr[5];
    uint32_t want[5] = { STATE_VERSION, (uint32_t)sizeof(c64_t), (uint32_t)sizeof(vic_t),
                         (uint32_t)sizeof(cia_t), (uint32_t)sizeof(sid) };
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, STATE_MAGIC, 8) || fread(hdr, sizeof hdr, 1, f) != 1 ||
        memcmp(hdr, want, sizeof hdr)) {
        fclose(f);
        snprintf(msg, msglen, "%s: not a savestate of this build", path);
        return -1;
    }
    uint64_t nn, sn, bn;
    void *nb = get_blob(f, &nn), *sb = get_blob(f, &sn), *bb = get_blob(f, &bn);
    fclose(f);
    int rc = -1;
    if (nb && sb && bb && bn == sizeof(c64_t)) {
        c64_t *b = bb;
        /* keep this instance's pointers and sinks */
        b->cpu.cpu.sim = m->cpu.cpu.sim;
        b->sid.chip = m->sid.chip;
        b->sid.buf = m->sid.buf;
        b->sid.cap = m->sid.cap;
        b->sid.wav = m->sid.wav;
        b->sid.on_samples = m->sid.on_samples;
        b->sid.on_samples_ctx = m->sid.on_samples_ctx;
        b->trace_left = m->trace_left;
        b->trace_out = m->trace_out;
        b->sid_log = m->sid_log;
        if (b->cpu.core == m->cpu.core &&
            (m->cpu.core != M6510_NETLIST ? nn == 0 : netsim_state_load(b->cpu.cpu.sim, nb, nn) == 0) &&
            sid_state_load(&b->sid.chip, sb, sn) == 0) {
            memcpy(m, b, sizeof *m);
            rc = 0;
        }
    }
    free(nb); free(sb); free(bb);
    snprintf(msg, msglen, rc ? "%s: corrupt or incompatible savestate" : "state loaded from %s (cycle %llu, frame %llu)",
             path, (unsigned long long)m->cycle, (unsigned long long)m->vic.frame);
    return rc;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

uint64_t c64_state_hash(c64_t *m) {
    sidpins_sync(&m->sid);
    uint64_t h = 0xcbf29ce484222325ull;
    size_t nn = m->cpu.core != M6510_NETLIST ? 0 : netsim_state_size(m->cpu.cpu.sim);
    void *nb = malloc(nn ? nn : 1);
    if (nn) netsim_state_save(m->cpu.cpu.sim, nb);
    h = fnv(h, nb, nn);
    free(nb);
    size_t sn = sid_state_size(&m->sid.chip);
    void *sb = malloc(sn);
    sid_state_save(&m->sid.chip, sb);
    h = fnv(h, sb, sn);
    free(sb);
    c64_t *b = malloc(sizeof *b);
    memcpy(b, m, sizeof *b);
    scrub(b);
    h = fnv(h, b, sizeof *b);   /* RAM, VIC (incl. frame buffer), CIAs, port, counters */
    free(b);
    return h;
}
