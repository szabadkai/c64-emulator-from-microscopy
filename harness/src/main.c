/* main.c - command line front end of the headless C64. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "../gen/testrom.h"
#include "c64.h"
#include "ciatest.h"
#include "cputest.h"
#include "png.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* CPU time of this process: independent of how busy the machine is */
static double cpu_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void usage(void) {
    puts("usage: c64 [options]\n"
         " machine\n"
         "  --rom-dir DIR        KERNAL/BASIC/CHARGEN directory (default: roms)\n"
         "  --test-rom           use the built-in self-written test KERNAL instead\n"
         "  --cpu netlist|compiled|fast  CPU core: visual6502 transistor netlist (default); the same\n"
         "                       netlist compiled to C (node-identical, ~1.75x faster); or the\n"
         "                       cycle-exact behavioural core (bus-identical, ~350x real time)\n"
         "  --sid-model M        6581 (default) or 8580\n"
         " running\n"
         "  --frames N | --cycles N   run N PAL frames (19656 cycles each) / N cycles (default 50 frames)\n"
         "  --until-text STR     stop (one frame later) once STR is on screen\n"
         "  --type STR           type STR (\\n = RETURN) once READY. is shown (or --type-at-frame N)\n"
         "  --prg FILE           autostart: at READY. inject FILE at its load address, set the BASIC\n"
         "                       pointers like LOAD, then type RUN<RETURN>\n"
         "  --psid FILE [--song N]  at READY. load a PSID tune and start a RAM driver (raster IRQ for\n"
         "                       VBI speed, KERNAL CIA1 IRQ for CIA speed) that calls init, then play\n"
         "  --load-state FILE    start from a savestate (frames/cycles then count from there)\n"
         "  --save-state FILE    write a savestate at the end of the run\n"
         " output\n"
         "  --screenshot PATH    full 403x284 frame buffer as PNG\n"
         "  --dump-frames DIR    every completed frame as PNG (384x272 VICE normal-border crop)\n"
         "  --dump-from F        first frame to dump (default 0)   --dump-count N   number of frames\n"
         "  --wav FILE           SID audio, 44.1 kHz mono 16-bit, sample-locked to emulated time\n"
         "  --sid-log FILE       one line per SID write: cycle frame line rasterCycle reg value (hex)\n"
         "  --frame-log FILE     one line per frame: frame cycle sid_writes irqs nmis badlines stalled\n"
         "  --watch ADDR         report the first opcode fetch at ADDR (hex, repeatable)\n"
         "  --screen-text        print the 40x25 text screen\n"
         "  --trace N            print N bus cycles (addr/rw/data/sync/BA/AEC/IRQ/regs) from --trace-at C\n"
         "  --stats              timing / stall / interrupt / SID / speed statistics\n"
         "  --state-hash         print a hash of the complete machine state at the end\n"
         " self tests\n"
         "  --cpu-test           netlist CPU instruction tests      --cpu-bench N   CPU-alone speed\n"
         "  --cia-test           6526 timer/ICR pipeline tests");
}

static int write_screenshot(const c64_t *m, const char *path) {
    return png_write_indexed(path, &m->vic.fb[0][0], VIC_SCREEN_W, VIC_SCREEN_H, vic_palette, 16);
}

static int write_crop(const c64_t *m, const char *path) {
    static uint8_t buf[VIC_CROP_W * VIC_CROP_H];
    vic_crop_normal(&m->vic, buf);
    return png_write_indexed(path, buf, VIC_CROP_W, VIC_CROP_H, vic_palette, 16);
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void mkdir_p(const char *dir) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

int main(int argc, char **argv) {
    const char *rom_dir = NULL, *shot = NULL, *until = NULL, *type = NULL, *prg = NULL, *psid = NULL;
    const char *dump_dir = NULL, *save_state = NULL, *load_state = NULL, *sid_log = NULL, *wav = NULL;
    const char *frame_log = NULL;
    bool test_rom = false, screen_text = false, stats = false, state_hash = false;
    int cpu_core = M6510_NETLIST;
    long long frames = -1, cycles = -1, trace = 0, trace_at = 0, type_frame = -1;
    long long dump_from = 0, dump_count = -1, song = 0;
    sid_model model = SID_6581;
    uint16_t watch[C64_MAX_WATCH];
    int nwatch = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define ARG() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "%s needs an argument\n", a), exit(2), ""))
        if (!strcmp(a, "--rom-dir")) rom_dir = ARG();
        else if (!strcmp(a, "--test-rom")) test_rom = true;
        else if (!strcmp(a, "--cpu")) {
            const char *c = ARG();
            if (!strcmp(c, "fast")) cpu_core = M6510_FAST;
            else if (!strcmp(c, "compiled")) cpu_core = M6510_COMPILED;
            else if (strcmp(c, "netlist")) { fprintf(stderr, "--cpu: netlist, compiled or fast\n"); return 2; }
        }
        else if (!strcmp(a, "--sid-model")) {
            const char *s = ARG();
            if (!strcmp(s, "6581")) model = SID_6581;
            else if (!strcmp(s, "8580")) model = SID_8580;
            else { fprintf(stderr, "--sid-model must be 6581 or 8580\n"); return 2; }
        }
        else if (!strcmp(a, "--frames")) frames = atoll(ARG());
        else if (!strcmp(a, "--cycles")) cycles = atoll(ARG());
        else if (!strcmp(a, "--until-text")) until = ARG();
        else if (!strcmp(a, "--type")) type = ARG();
        else if (!strcmp(a, "--type-at-frame")) type_frame = atoll(ARG());
        else if (!strcmp(a, "--prg")) prg = ARG();
        else if (!strcmp(a, "--psid")) psid = ARG();
        else if (!strcmp(a, "--song")) song = atoll(ARG());
        else if (!strcmp(a, "--screenshot")) shot = ARG();
        else if (!strcmp(a, "--dump-frames")) dump_dir = ARG();
        else if (!strcmp(a, "--dump-from")) dump_from = atoll(ARG());
        else if (!strcmp(a, "--dump-count")) dump_count = atoll(ARG());
        else if (!strcmp(a, "--save-state")) save_state = ARG();
        else if (!strcmp(a, "--load-state")) load_state = ARG();
        else if (!strcmp(a, "--sid-log")) sid_log = ARG();
        else if (!strcmp(a, "--wav")) wav = ARG();
        else if (!strcmp(a, "--frame-log")) frame_log = ARG();
        else if (!strcmp(a, "--watch")) {
            const char *s = ARG();
            if (nwatch < C64_MAX_WATCH) watch[nwatch++] = (uint16_t)strtol(s[0] == '$' ? s + 1 : s, NULL, 16);
        }
        else if (!strcmp(a, "--screen-text")) screen_text = true;
        else if (!strcmp(a, "--trace")) trace = atoll(ARG());
        else if (!strcmp(a, "--trace-at")) trace_at = atoll(ARG());
        else if (!strcmp(a, "--stats")) stats = true;
        else if (!strcmp(a, "--state-hash")) state_hash = true;
        else if (!strcmp(a, "--cpu-test")) return cputest_run(1);
        else if (!strcmp(a, "--cia-test")) return ciatest_run(1);
        else if (!strcmp(a, "--cpu-bench")) { cputest_bench((uint64_t)atoll(ARG())); return 0; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
#undef ARG
    }
    if (!rom_dir && !test_rom) rom_dir = "roms";

    c64_t *m = calloc(1, sizeof(c64_t));
    if (c64_init(m, model) != 0) { fprintf(stderr, "SID init failed\n"); return 1; }
    if (cpu_core == M6510_FAST) c64_use_fast_cpu(m);
    if (cpu_core == M6510_COMPILED) c64_use_compiled_cpu(m);
    char msg[2048];
    if (rom_dir) {
        if (c64_load_roms(m, rom_dir, msg, sizeof msg) != 0) {
            if (!test_rom) { fprintf(stderr, "%s\n", msg); return 1; }
        } else fprintf(stderr, "%s\n", msg);
    }
    if (test_rom) {
        memcpy(m->kernal, testrom_bin, sizeof m->kernal);
        fprintf(stderr, "KERNAL: built-in self-written test ROM\n");
    }
    uint8_t *prg_data = NULL, *psid_data = NULL;
    size_t prg_len = 0, psid_len = 0;
    if (prg && !(prg_data = read_file(prg, &prg_len))) { fprintf(stderr, "cannot read %s\n", prg); return 1; }
    if (psid && !(psid_data = read_file(psid, &psid_len))) { fprintf(stderr, "cannot read %s\n", psid); return 1; }

    /* output sinks (kept across --load-state) */
    m->trace_out = stdout;
    wav_writer ww;
    if (wav) {
        if (wav_open(&ww, wav, 44100) != 0) { fprintf(stderr, "cannot write %s\n", wav); return 1; }
        m->sid.wav = &ww;
    }
    if (sid_log && !(m->sid_log = fopen(sid_log, "w"))) { fprintf(stderr, "cannot write %s\n", sid_log); return 1; }
    FILE *flog = NULL;
    if (frame_log) {
        if (!(flog = fopen(frame_log, "w"))) { fprintf(stderr, "cannot write %s\n", frame_log); return 1; }
        fprintf(flog, "# frame end_cycle sid_writes irqs nmis badlines cpu_stalled\n");
    }
    if (dump_dir) mkdir_p(dump_dir);

    if (load_state) {
        if (c64_load_state(m, load_state, msg, sizeof msg) != 0) { fprintf(stderr, "%s\n", msg); return 1; }
        fprintf(stderr, "%s\n", msg);
    }
    m->nwatch = nwatch;
    for (int i = 0; i < nwatch; i++) { m->watch_addr[i] = watch[i]; m->watch_cycle[i] = -1; }

    if (frames < 0 && cycles < 0) frames = 50;
    const uint64_t start_cycle = m->cycle;
    uint64_t limit = start_cycle + (cycles >= 0 ? (uint64_t)cycles : (uint64_t)frames * C64_CYCLES_PER_FRAME);

    /* autostart bookkeeping (not used when resuming from a savestate) */
    bool auto_done = load_state != NULL || (!prg && !psid);
    bool typing_started = false, typing_reported = false;
    uint64_t typed_done_frame = 0, stop_frame = 0, auto_frame = 0;
    uint64_t last_frame = m->vic.frame;
    uint64_t sidw_frames = 0, sidw_total = 0, sidw_min = UINT64_MAX, sidw_max = 0, dumped = 0;
    uint32_t sidw_hist[64] = { 0 };

    double t0 = now(), c0 = cpu_now();
    while (m->cycle < limit) {
        if (trace > 0 && m->cycle == (uint64_t)trace_at) m->trace_left = trace;
        c64_tick(m);
        if (m->vic.frame == last_frame) continue;
        last_frame = m->vic.frame;
        uint64_t done_frame = m->vic.frame - 1;   /* frame just completed */

        if (flog)
            fprintf(flog, "%llu %llu %u %u %u %u %u\n", (unsigned long long)done_frame,
                    (unsigned long long)m->cycle, m->flast.sid_writes, m->flast.irqs, m->flast.nmis,
                    m->flast.badlines, m->flast.cpu_stalled);
        /* SID writes per frame: from the autostart on (or from the first
           frame with writes when nothing is autostarted) */
        if ((prg || psid) ? (auto_frame && done_frame >= auto_frame)
                          : (sidw_frames > 0 || m->flast.sid_writes > 0)) {
            uint32_t w = m->flast.sid_writes;
            sidw_frames++; sidw_total += w;
            if (w < sidw_min) sidw_min = w;
            if (w > sidw_max) sidw_max = w;
            sidw_hist[w < 63 ? w : 63]++;
        }
        if (dump_dir && (long long)done_frame >= dump_from &&
            (dump_count < 0 || (long long)done_frame < dump_from + dump_count)) {
            char path[1200];
            snprintf(path, sizeof path, "%s/frame_%05llu.png", dump_dir, (unsigned long long)done_frame);
            if (write_crop(m, path) == 0) dumped++;
        }

        /* autostart: --prg / --psid once READY. is on screen */
        if (!auto_done && c64_screen_contains(m, "READY.")) {
            auto_done = true;
            if (prg) {
                uint16_t s, e;
                if (c64_inject_prg(m, prg_data, prg_len, &s, &e, msg, sizeof msg) != 0) {
                    fprintf(stderr, "%s\n", msg);
                    return 1;
                }
                auto_frame = m->vic.frame;
                c64_type(m, "RUN\\n");
                typing_started = true;
                fprintf(stderr, "frame %llu cycle %llu: %s; typing RUN<RETURN>\n",
                        (unsigned long long)m->vic.frame, (unsigned long long)m->cycle, msg);
            } else {
                if (c64_install_psid(m, psid_data, psid_len, (int)song, msg, sizeof msg) != 0) {
                    fprintf(stderr, "%s\n", msg);
                    return 1;
                }
                auto_frame = m->vic.frame;
                fprintf(stderr, "frame %llu cycle %llu: %s\n", (unsigned long long)m->vic.frame,
                        (unsigned long long)m->cycle, msg);
            }
        }
        if (type && !typing_started && auto_done) {
            bool go = type_frame >= 0 ? (long long)m->vic.frame >= type_frame
                                      : c64_screen_contains(m, "READY.");
            if (go) {
                c64_type(m, type);
                typing_started = true;
                fprintf(stderr, "frame %llu cycle %llu: typing \"%s\"\n", (unsigned long long)m->vic.frame,
                        (unsigned long long)m->cycle, type);
            }
        }
        if (typing_started && !c64_typing(m) && !typed_done_frame) {
            typed_done_frame = m->vic.frame;
            if (!typing_reported) {
                typing_reported = true;
                fprintf(stderr, "frame %llu cycle %llu: typing finished (last key released)\n",
                        (unsigned long long)m->vic.frame, (unsigned long long)m->cycle);
            }
        }
        if (stop_frame && m->vic.frame >= stop_frame) break;
        if (until && !stop_frame && c64_screen_contains(m, until)) {
            /* when typing, keep going until the typed line was processed */
            if (!typing_started || (typed_done_frame && m->vic.frame > typed_done_frame + 10)) {
                fprintf(stderr, "frame %llu: \"%s\" found on screen, stopping after one more"
                        " frame (so the frame buffer shows it)\n", (unsigned long long)m->vic.frame, until);
                stop_frame = m->vic.frame + 1;
            }
        }
    }
    double dt = now() - t0, ct = cpu_now() - c0;
    sidpins_sync(&m->sid);
    uint64_t ran = m->cycle - start_cycle;

    if (screen_text) {
        char t[25][41];
        c64_screen_text(m, t);
        printf("+----------------------------------------+\n");
        for (int r = 0; r < 25; r++) printf("|%s|\n", t[r]);
        printf("+----------------------------------------+\n");
    }
    if (shot) {
        if (write_screenshot(m, shot) == 0) fprintf(stderr, "screenshot written to %s\n", shot);
        else fprintf(stderr, "could not write %s\n", shot);
    }
    if (dump_dir) fprintf(stderr, "%llu frames written to %s/\n", (unsigned long long)dumped, dump_dir);
    for (int i = 0; i < m->nwatch; i++) {
        if (m->watch_cycle[i] >= 0)
            printf("watch $%04X: first opcode fetch at cycle %lld (frame %llu, line %d, cycle %d)\n",
                   m->watch_addr[i], (long long)m->watch_cycle[i], (unsigned long long)m->watch_frame[i],
                   m->watch_line[i], m->watch_lcycle[i]);
        else
            printf("watch $%04X: never fetched as an opcode\n", m->watch_addr[i]);
    }
    if (stats) {
        printf("cycles run          : %llu (%.2f frames, %.3f s emulated); machine cycle now %llu, frame %llu\n",
               (unsigned long long)ran, (double)ran / C64_CYCLES_PER_FRAME, (double)ran / C64_PAL_HZ,
               (unsigned long long)m->cycle, (unsigned long long)m->vic.frame);
        printf("wall time           : %.3f s -> %.0f cycles/s = %.2f%% of real time (1/%.1f)\n", dt,
               ran / dt, 100.0 * ran / dt / C64_PAL_HZ, C64_PAL_HZ / (ran / dt));
        printf("process CPU time    : %.3f s -> %.0f cycles/s = %.2f%% of real time (1/%.1f)\n", ct,
               ran / ct, 100.0 * ran / ct / C64_PAL_HZ, C64_PAL_HZ / (ran / ct));
        printf("last full frame     : badlines %u, BA low %u cycles, CPU stalled (RDY low on read) %u,"
               " VIC-owned PHI2 (AEC low) %u, /IRQ asserted %u cycles, IRQs %u, NMIs %u, SID writes %u\n",
               m->flast.badlines, m->flast.ba_low, m->flast.cpu_stalled, m->flast.vic_stolen,
               m->flast.irq_cycles, m->flast.irqs, m->flast.nmis, m->flast.sid_writes);
        printf("interrupts taken    : IRQ %llu (first at cycle %lld), NMI %llu (first at cycle %lld)\n",
               (unsigned long long)m->irqs_taken, (long long)m->first_irq_cycle,
               (unsigned long long)m->nmis_taken, (long long)m->first_nmi_cycle);
        printf("VIC                 : frame %llu line %d cycle %d, raster IRQs latched %llu\n",
               (unsigned long long)m->vic.frame, m->vic.line, m->vic.cycle, (unsigned long long)m->vic.raster_irqs);
        printf("CIA1                : TA latch $%04X underflows %llu, IRQs %llu; CIA2: TA latch $%04X"
               " underflows %llu, NMIs %llu; RAM[$02A6] (KERNAL PAL flag) = %u\n",
               m->cia1.ta.latch, (unsigned long long)m->cia1.ta.underflows, (unsigned long long)m->cia1.irqs,
               m->cia2.ta.latch, (unsigned long long)m->cia2.ta.underflows, (unsigned long long)m->cia2.irqs,
               m->ram[0x02A6]);
        printf("SID                 : %s, %llu writes, %llu reads, %llu samples (%.1f per frame)",
               m->sid.chip.model == SID_8580 ? "8580" : "6581", (unsigned long long)m->sid.writes,
               (unsigned long long)m->sid.reads, (unsigned long long)m->sid.samples,
               m->cycle ? (double)m->sid.samples * C64_CYCLES_PER_FRAME / (double)m->sid.clocked : 0.0);
        if (sidw_frames)
            printf("; writes/frame %s: avg %.2f min %llu max %llu over %llu frames",
                   (prg || psid) ? "from the autostart frame on" : "from the first write on",
                   (double)sidw_total / sidw_frames, (unsigned long long)sidw_min,
                   (unsigned long long)sidw_max, (unsigned long long)sidw_frames);
        printf("\n");
        if (sidw_frames) {
            printf("SID writes/frame    :");
            for (int i = 0; i < 64; i++)
                if (sidw_hist[i]) printf(" %d%s:%u", i, i == 63 ? "+" : "", sidw_hist[i]);
            printf("  (value:frames)\n");
        }
        printf("6510 port           : DDR=$%02X DATA=$%02X pins=$%02X; RAM[$00]=$%02X RAM[$01]=$%02X\n",
               m->cpu.ddr, m->cpu.data, m6510_port_pins(&m->cpu), m->ram[0], m->ram[1]);
        printf("CPU registers       : PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X\n", m6510_pc(&m->cpu),
               m6510_a(&m->cpu), m6510_x(&m->cpu), m6510_y(&m->cpu), m6510_s(&m->cpu), m6510_p(&m->cpu));
        const netcpu_t *c = &m->cpu.cpu;
        const netsim_stats_t *st = m->cpu.core != M6510_NETLIST ? NULL : netsim_stats(c->sim);
        if (m->cpu.core == M6510_FAST)
            printf("CPU core            : fast behavioural (bus-cycle identical to the netlist)\n");
        if (m->cpu.core == M6510_COMPILED) {
            const ccpu_stats_t *cs = ccpu_stats(&m->cpu.comp);
            double n = m->cpu.comp.cycles ? (double)m->cpu.comp.cycles : 1;
            if (cs->passes)   /* counters exist only in a -DCCPU_STATS build */
                printf("CPU core            : compiled netlist (node-identical to netsim); per cycle %.1f passes,"
                       " %.1f queue entries, %.1f node changes, loop-limit hits %llu\n", cs->passes / n, cs->evals / n,
                       cs->changes / n, (unsigned long long)cs->loop_limit_hits);
            else
                printf("CPU core            : compiled netlist (node-identical to netsim); build with"
                       " -DCCPU_STATS for per-cycle counters\n");
        }
        else printf("netlist             : %d transistors; per cycle %.1f group evals, %.2f nodes/group,"
               " %.1f passes, loop-limit hits %llu\n",
               netcpu_transistors(c), (double)st->groups / c->cycles,
               (double)st->group_nodes / (st->groups ? st->groups : 1), (double)st->passes / c->cycles,
               (unsigned long long)st->loop_limit_hits);
    }
    if (save_state) {
        int r = c64_save_state(m, save_state, msg, sizeof msg);
        fprintf(stderr, "%s\n", msg);
        if (r) return 1;
    }
    if (state_hash) printf("state hash          : %016llx (cycle %llu)\n",
                           (unsigned long long)c64_state_hash(m), (unsigned long long)m->cycle);
    if (wav) {
        m->sid.wav = NULL;
        wav_close(&ww);
        fprintf(stderr, "audio written to %s\n", wav);
    }
    if (m->sid_log) fclose(m->sid_log);
    if (flog) fclose(flog);
    c64_free(m);
    free(m);
    free(prg_data);
    free(psid_data);
    return 0;
}
