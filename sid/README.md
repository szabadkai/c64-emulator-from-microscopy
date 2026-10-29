# SID: cycle-clocked MOS 6581 / 8580 emulation (C99)

This SID chip emulation is for the C64 harness, written to play
*Minimalistic Intro* (Proton/Finnish Gold, 2019). That intro's GoatTracker
tune is tagged **PAL + 6581**, so the 6581 model gets the most detail.
I wrote all of it from public documentation. No third-party emulator source
code or binaries were downloaded, compiled or run. It uses no libraries
beyond libc and libm.

| File | Purpose |
|---|---|
| `sid.h`, `sid.c` | the chip: oscillators, envelopes, DACs, filter, mixer, output stage, resampler |
| `wav.h`, `wav.c` | 16-bit mono PCM WAV writer (one-shot and streaming) |
| `sidtest.c` | test suite; writes WAV and CSV files to `out/` and prints PASS/FAIL |
| `Makefile` | `make` builds `sidtest`; `make test` builds and runs it |

`./sidtest pitch env adsr noise filter sync demo speed digi alias reads state`
runs only the named tests.

## API

```c
int     sid_init(sid *s, sid_model model /*SID_6581|SID_8580*/, double clock_hz /*985248*/, double sample_rate /*44100*/);
void    sid_reset(sid *s);
void    sid_free(sid *s);
void    sid_write(sid *s, int reg, uint8_t val);
uint8_t sid_read(sid *s, int reg);
int     sid_clock(sid *s, int cycles, int16_t *out, int max_samples);  /* returns samples written */
int     sid_pending(const sid *s);  void sid_drop_pending(sid *s);
void    sid_set_pots(sid *s, uint8_t potx, uint8_t poty);       /* default $FF/$FF */
void    sid_set_filter_curve(sid *s, double pos);                /* 6581: 0 bright .. 1 dark, default 0.5 */
/* savestates (added later; the functions above are unchanged) */
size_t  sid_state_size(const sid *s);
void    sid_state_save(const sid *s, void *buf);
int     sid_state_load(sid *s, const void *buf, size_t len);   /* 0 ok, -1 bad/truncated */
```

`sid_init` allocates about 130 KB of tables and resampler state on the heap.
Call `sid_free` to release it. Because the `sid` struct holds a pointer to
that memory, you cannot save state by copying the struct with `memcpy`. Use
the savestate functions instead (see "Savestates" below).

## Integration notes

**Clocking.** One `sid_clock` cycle is one PHI2 cycle. You can clock the
chip in either of two ways:

* **Batched (recommended):** keep `last_sid_cycle`. Before every SID
  register access at machine cycle `T`, call
  `sid_clock(&s, T - last_sid_cycle, buf + n, cap - n)` and then
  `sid_write` or `sid_read`. At the end of each frame, clock the rest of the
  frame and hand `buf` to the audio sink. Register changes still take effect
  on the exact cycle, so this is as accurate as clocking every cycle.
  Measured speed: about 90–134× real time (CPU time, single thread).
* **Per cycle:** call `sid_clock(&s, 1, buf + n, cap - n)` once per PHI2
  cycle. This also works, but more slowly (about 30–58× real time),
  because each call has some fixed setup cost.

**Sample-accurate audio.**

* Output timing is fixed: one sample every `clock/sample_rate` cycles. At
  PAL and 44.1 kHz that is 22.341 cycles per sample, or about 879.8 samples
  per 19656-cycle frame.
* The resampler adds a constant group delay of about 0.62 ms.
* No samples are dropped or duplicated, so audio stays locked to emulated
  time. Write the samples into an audio FIFO and let its fill level absorb
  differences between host and emulated speed.
* `out` may be `NULL` or too small. Samples that don't fit are queued
  internally (up to 4096) and returned on the next call. Beyond that the
  oldest samples are dropped. `sid_pending()` reports the queue length.
* Size the buffer to at least `cycles / 22 + 2`.

**Register writes.**

* `reg` is masked to 0–31.
* Writes to `$00–$18` take effect from the next clocked cycle.
* Writes to `$19–$1F` are ignored, but they still update the data bus (see
  "Register reads" below).
* `$15–$18` (filter and volume) are recomputed on every write.
* Writes to the volume register change a DC level, so 4-bit `$D418` digis
  are audible on the 6581 (test 9).

**Register reads.**

| Reg | Returns |
|---|---|
| `$19` / `$1A` | POTX/POTY: the values given to `sid_set_pots` (default `$FF`, nothing connected). |
| `$1B` | OSC3: the top 8 bits of voice 3's 12-bit waveform output, including noise, combined and floating-DAC values. Not affected by the envelope or by 3OFF. |
| `$1C` | ENV3: voice 3's 8-bit envelope counter, including the ADSR delay bug and the zero-freeze. |
| others (write-only or unused) | The last value on the internal data bus. Writes, and reads of `$19–$1C`, refresh it. On the 6581 it reads back for about 7000 cycles and then becomes `$00`. On the 8580 it lasts about 600000 cycles. |

**Savestates.** `sid_state_save` captures all dynamic state:

* voices, oscillators, noise registers and floating DAC values;
* envelopes, including rate and exponential counters and the delay-bug phase;
* filter integrators and the derived mixer and filter coefficients;
* output-stage filter states;
* stage-1 decimator history and phase, and the stage-2 ring buffer and
  fractional position;
* the data bus value and its fade timestamp, the cycle counter and the pot
  values;
* the 6581 filter-curve setting;
* any output samples still queued.

To save, call `sid_state_size()` immediately before `sid_state_save()`,
because the size grows with the number of queued samples (header about
2.3 KB plus 2 bytes per queued sample).

To load, pass `sid_state_load()` an instance that has already been through
`sid_init`. If the saved model, clock or sample rate differs, the instance is
re-initialised to the saved configuration. A bad or truncated buffer returns
-1 and leaves the instance unchanged.

The format is native-endian and tied to the build: it checks a magic number,
a version and `sizeof(sid)`. After a load, output is bit-identical to an
uninterrupted run (test 12).

**Other notes.**

* **Reset:** call `sid_reset()` when the C64 /RESET line goes low.
* **Clock:** use `SID_PAL_CLOCK` (985248) or `SID_NTSC_CLOCK` (1022727).
* **Threads:** each instance is independent. Do not call into one instance
  from several threads at once.
* **Building:** compile `sid.c` with `-std=c99` (or later) and link with
  `-lm`.

## Model description

### Oscillator and waveforms (both models)

The oscillator is a 24-bit phase accumulator. Each cycle it adds the 16-bit
frequency register, so f = reg × clock / 2^24. The TEST bit clears the
accumulator and holds it at 0.

| Waveform | 12-bit output |
|---|---|
| Sawtooth | Accumulator bits 23–12. |
| Triangle | Bits 22–11, inverted when the MSB is set. With RING MOD the MSB is XORed with the MSB of the source voice. |
| Pulse | `(acc >> 12) >= PW`, or TEST. The 12-bit pulse width gives 0 or `$FFF`. |
| Noise | A 23-bit LFSR with feedback bit 22 XOR bit 17. It is clocked on the rising edge of accumulator bit 19. Register bits 22, 20, 16, 13, 11, 7, 4 and 2 drive output bits 11–4. The period is 2^23−1 (tested). The register resets to `$7FFFFF`. |
| No waveform (`$0x`) | The DAC input floats: it holds the last value for about 50000 cycles (6581) or 500000 cycles (8580), then drops to 0. |

**Hard sync and ring modulation.** The source voice is the previous one:
voice 1 ← voice 3, voice 2 ← voice 1, voice 3 ← voice 2. Sync resets the
destination accumulator on the source's MSB rising edge. There is one
special case: if the source is itself being synced on that same cycle, the
destination is not synced.

**Combined waveforms (approximate).** The selector ties the selected bit
lines together. A 0 driver beats a 1 driver. On the 6581, neighbouring bit
lines also leak into each other, so isolated 1 bits get pulled down. The
model works per bit:

1. Compute the drive `v = ones / (ones + 3·zeros)`.
2. Smear `v` over the neighbouring bits with a coupling kernel: (1, .65, .3, .1) on the 6581, or (1, .15, .03, 0) on the 8580.
3. The output bit is 1 only if all selected sources are 1 and the smeared drive exceeds a threshold (0.6 on the 6581, 0.5 on the 8580).

The results are precomputed into tables for TS, PT, PS and PTS, indexed by
the accumulator's top 12 bits plus the ring-flip bit.

**Noise combined with other waveforms.** The output is noise AND the other
waveforms. Each time the LFSR is clocked, and on the control write, the LFSR
tap bits are cleared wherever the combined output is 0. This reproduces the
documented lock-up (the output stays at 0 until TEST). In test 4 the lock-up
appears after combining noise with saw, and TEST recovers it. Setting TEST
refills the LFSR with ones immediately.

### Envelope generator

* **Rate counter:** a 15-bit counter that compares for equality against the
  period for each rate. The periods, in cycles, are 9, 32, 63, 95, 149, 220,
  267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532 and 31251, which match
  the datasheet's 2 ms … 8 s for 255 steps at 1 MHz.
* **Gate:** gate changes switch the state and the period but do not reset
  the counter. If the new period is below the current count, the counter
  must wrap through `$8000` first. This is the **ADSR delay bug**, worth up
  to 32768 cycles (33.26 ms on PAL).
* **Exponential counter (decay and release only):** the step divider
  becomes 1 / 2 / 4 / 8 / 16 / 30 when the envelope reaches
  `$FF / $5D / $36 / $1A / $0E / $06`. This gives the ×3 decay and release
  times: 756 rate periods from `$FF` to 0. Attack is linear: 255 periods.
* **Sustain:** decay stops when the counter equals `S × $11`. Raising the
  sustain level never makes the counter count up.
* **Zero freeze:** reaching 0 freezes the counter until the next gate-on.
  The documented `$FF → $00` wrap (release, then attack before the first
  step) also freezes it.

### DACs (6581 non-linearity)

The waveform (12-bit), envelope (8-bit) and cutoff (11-bit) DACs are R-2R
ladders. Each bit's weight is solved by superposition: walk a Thevenin
equivalent from the LSB to the MSB.

| | 2R/R ratio | LSB termination | Result |
|---|---|---|---|
| 6581 | 2.20 | missing | non-binary weights; the MSB is worth about 3.2 % of full scale less than all lower bits together (the step at `$7FF → $800` / `$3FF → $400`) |
| 8580 | 2.00 | present | ideal binary weights |

### Voice output, mixer, volume and DC levels

Each voice outputs `(waveDAC − zero) × envDAC`. The 6581's waveform zero
level is `$380/$FFF`, so each voice carries a DC offset that the envelope
scales. The 8580's zero level is mid-scale.

* **Routing:** voices go to the filter or directly to the mixer. 3OFF mutes
  voice 3's direct path only when voice 3 is not routed to the filter.
  External input is ignored.
* **Mixer:** `out = (direct + filter_out + mixer_DC) × vol/15`. `mixer_DC`
  is 1.0 voice unit (one full-scale voice peak-to-peak) on the 6581 and
  0.05 on the 8580. This produces the audible 6581 volume digis: −15 dBFS
  for a full-swing 4-bit sine, against −41 dBFS on the 8580.

### Filter

The filter is a two-integrator-loop state-variable filter (Chamberlin form)
run every cycle. LP, BP and HP can be selected in any combination; LP+HP
gives a notch.

**6581 cutoff model** (my own parametric model of the VCR circuit):
`x = DAC6581(FC)`, a value from 0 to 1 that includes the `$3FF/$400` kink.
The cutoff is

`fc = 215 Hz + g / (1 + g/25000)`, where `g = 7370 · softplus((x − x0)/0.065)` and `x0 = 0.48`.

The three terms correspond to parts of the circuit:

* **215 Hz floor:** the fixed resistor in parallel with the VCR (about
  1.5 MΩ with 470 pF).
* **Softplus:** the VCR transistor. Its conductance is exponential below
  threshold and linear above it, in the EKV style.
* **25 kHz series term:** a fixed series resistance that limits the top end.

The parameters are fitted to the published 6581 characteristics:

* about 220 Hz at FC=0, with a flat bottom;
* about 420 Hz at `$200`;
* a steep rise to about 5.9 kHz at `$3FF`;
* a drop to about 4.6 kHz at `$400`;
* about 18 kHz at `$7FF`.

`sid_set_filter_curve` shifts `x0` by ±0.05 to imitate dark or bright chips.

**6581 resonance and saturation:**

* 1/Q falls linearly with RES, from 1.414 to 0.454 (Q 0.71 → 2.2).
* A cubic soft-clipper on the summer (HP) output limits the summer's swing.
  It is memoryless and applied before the first integrator.
* The voice DC offsets pass through the low-pass path, so filter routing
  changes cause audible DC steps, as on a real 6581.

**8580:** the cutoff is linear, `fc = 30 Hz + FC × 5.85 Hz` (up to
12.0 kHz). 1/Q = 2^((4−RES)/8), giving Q 0.71 → 2.6. There is no
saturation.

### Output stage and resampling

* **C64 output stage:**
  * a 1-pole low-pass at 16 kHz, applied every cycle;
  * a 1-pole AC-coupling high-pass at 16 Hz, applied at the output rate.
* **Stage 1:** decimate by D = 8 (to 123156 Hz) with a 62-tap Kaiser
  windowed-sinc FIR (80 dB). Its passband ends at 19.85 kHz and its stopband
  starts at 101 kHz.
* **Stage 2:** a polyphase windowed-sinc resampler with 144 taps × 64
  phases, interpolating linearly between phases. Its stopband starts at
  24.25 kHz, so anything that aliases lands only above 19.85 kHz.
* Other sample rates get an automatically chosen D and filter lengths.
* Output is int16, at 12000 per voice unit (about ±1.36 voice peak-to-peak
  of headroom before clipping).

## Sources consulted (documents only; no code)

| Part | Sources |
|---|---|
| Register map, envelope rate table (2 ms … 8 s, ×3 for decay and release), sync and ring source voices, 3OFF, AND-ing of waveforms, noise lock-up and TEST recovery, POT behaviour, filter caps and range | **MOS 6581 SID datasheet** (scan at 6502.org, pages 5–7) |
| 24-bit accumulator; derivation of the saw, triangle, pulse and noise waveforms; combined waveforms as a wired AND that can lock up noise; the waveform DAC feeding a multiplying envelope DAC; envelope counter, programmable divider and exponential "piece-wise linear" table; sustain compare; sync and ring definitions; state-variable filter with FET VCRs; 11-bit cutoff DAC; 4-bit resonance ladder; volume as a multiplying DAC; volume-register digis | **Bob Yannes interview by Andreas Varga, 1996** (landley.net mirror) |
| 6581 filter topology (two-integrator loop, single-ended summer); NMOS-inverter "op-amps" with limited swing; R-2R DACs with missing termination; VCR plus parallel resistor; Rmax about 1.5 MΩ with 470 pF giving about 220 Hz; 1/Q linear in RES; light/dark curve variation | **libsidplayfp / reSIDfp `Filter6581` class documentation page** (prose description of Dag Lem's schematic, derived from Michael Huth's die photos as vectorised by Tommi Lempinen). Prose only; no code read. |
| 6581 cutoff calibration targets: about 220 Hz to 18 kHz with 470 pF, tanh-like shape, discontinuity at FCHI=`$80`, about 420 Hz at FCHI `$40`, about 4.6 kHz just above `$80` | Measured figures quoted from reSID's documentation comments, seen as **web-search result excerpts** (source not downloaded or used). Also **MIDIbox forum threads** reporting cutoff jumps at `$3FF/$400` on several 6581s and about 1 dB filter loss. |
| Noise taps and output bits, rate-counter periods, exponential breakpoints, 15-bit counter wrap (ADSR delay bug), envelope zero-freeze, 2R/R ≈ 2.2 on the 6581, C64 output RC values (10 µF / 1 kΩ and 10 kΩ / 1 nF), data-bus fade, floating waveform DAC | Widely published C64-scene documentation (codebase64-style write-ups, forum discussions, the reSID changelog seen in search excerpts). Some of this comes from **background knowledge** of those public documents rather than pages fetched in this session. The codebase64 page redirected to an unrelated domain and was not fetched. Every value is checked by the tests where it can be measured. |
| Resampler design (Kaiser window β and length formulas, polyphase) | Standard DSP literature (Kaiser 1974; Oppenheim & Schafer) |

## Test results (`make test`, PAL 985248 Hz, 44.1 kHz) — 32 passed, 0 failed

| # | Test | Result |
|---|---|---|
| 1 | Pitch: triangle, saw and pulse at 6 frequency values (60 Hz – 3.6 kHz), both models | worst error **0.0003 %** (tolerance 0.5 %) |
| 2 | Envelope: all 16 rates, attack, decay and release (48 times) | measured = 255 × period (attack) and 756 × period (decay and release) within one period. Worst deviation from the datasheet is 14.7 % (rate 0: 2.3 ms against 2 ms). Rate 8 attack is 100.0 ms against 100; rate 15 is 7.97 s against 8 s and 23.6 s against 24 s. ENV3 sampled once per frame → `out/env3_trajectory.csv` |
| 3 | ADSR delay bug (release 15, then attack 0; 200 trials) | delay 1.59–**33.24 ms** (theory: up to 33.26 ms), mean 17.3 ms. With a 2-frame `$0F00` hard restart: at most **9 cycles** |
| 4 | Noise | LFSR period **8388607** = 2^23−1. OSC3 mean 127.52, χ²(255) = 254.3, bit bias < 0.0033. Spectrum: octave bands from 125 Hz to 8 kHz within 6.8 dB (slight tilt from tap correlation), flatness 0.85. Lock-up after noise+saw, recovered by TEST |
| 5 | Filter −3 dB (noise in, cross-spectrum against the unfiltered path) | 6581: measured = model ±0.4 % for all 18 FC points; 220 Hz @ `$000`, 249 @ `$100`, 424 @ `$200`, 1545 @ `$300`, 5933 @ `$3FF`, **4624 @ `$400` (kink)**, 9966 @ `$500`, 13624 @ `$600`, 17751 @ `$7FF`. 8580: linear, R² = 1.0000, 5.85 Hz/step, 12.0 kHz @ `$7FF`. Resonance peak +7.1 dB (6581) and +8.4 dB (8580). CSV files: `out/filter_curve_6581.csv`, `out/filter_curve_8580.csv` |
| 6 | Ring modulation and sync | Ring: f1±f3 sidebands +119 dB, carrier −115 dB. Sync: saw at 481.08 Hz synced to 165.37 Hz measures 165.35 Hz (480.90 Hz without sync) |
| 7 | Musical demo (PWM arpeggio, hard-restart bass, melody, resonant LP sweep) | `out/demo_6581.wav`: peak −4.6 dBFS, RMS −17.6 dBFS. `out/demo_8580.wav`: peak −2.8 dBFS, RMS −16.7 dBFS. 0 clipped samples |
| 8 | Speed (demo, CPU time) | latest run: per frame **134×** real time (132 Mcycles/s); per raster line 132×; per cycle 58×. Runs on the heavily loaded test machine ranged from 93× to 134× per frame and 30× to 58× per cycle |
| 9 | `$D418` 4-bit digi, 1 kHz | 6581 −15.0 dBFS; 8580 −41.0 dBFS (26 dB quieter) |
| 10 | Aliasing: saw with exactly 1024 cycles per period | inharmonic energy in 20 Hz – 20 kHz is **−81.4 dB** (naive point sampling: −16.1 dB) |
| 11 | Reads | POT `$FF` and settable; OSC3 saw steps; data bus: `$5A` still reads after 2000 cycles; after 22000 cycles it is `$00` on the 6581 and still `$5A` on the 8580 |
| 12 | Savestate. The demo runs for N = 2,705,217 cycles (137 frames + 12345 cycles, with 453 samples left queued), then the state is saved. Run A continues the original for M = 2,955,711 cycles in uneven batches. Run B loads the state into a fresh instance and does the same | A == B **bit-exact**, 132751 samples, for both 6581 and 8580. The 8580 state is loaded into a 6581/48 kHz instance to exercise re-initialisation. A truncated buffer is rejected. State is 2814 bytes |

Listening and analysis files in `out/`:

* `waveforms_6581.wav`, `waveforms_8580.wav` (T, S, P, N, TS, PT, PS, PTS)
* `noise_6581.wav`
* `ringmod_6581.wav`
* `sync_sweep_6581.wav`
* `digi_6581.wav`, `digi_8580.wav`
* `demo_6581.wav`, `demo_8580.wav`
* `sidtest_log.txt`

## Known deviations from real chips

* **Combined waveforms** use a heuristic bit-coupling model, not sampled
  chip data. Some 6581 combinations are too loud: PS and PT come out near
  saw and triangle level, while the real chip is much quieter. The intro's
  tune uses only `$1x`, `$4x` and `$8x` waveforms, so this does not affect
  it.
* **Noise:**
  * TEST refills the LFSR with ones instantly. On the chip the bits charge
    up over thousands of cycles, and toggling TEST can shift the register.
  * The write-back from combined waveforms happens only on LFSR clocks and
    control writes.
* **Envelope:**
  * There are no 1–2 cycle pipeline delays.
  * The rate counter is a binary counter wrapping at `$8000`; on the chip it
    is a 15-bit LFSR. The difference is at most about 1 cycle per wrap.
* **6581 filter:**
  * The VCR conductance does not depend on the signal, so there is no
    level-dependent cutoff shift and no voice-DC-dependent cutoff, which is
    the main source of real 6581 "dirt". Distortion is one cubic soft-clip on
    the summer.
  * The curve is calibrated to one documented chip; real chips vary widely.
    `sid_set_filter_curve` gives a coarse adjustment.
  * There is no bleed-through when no filter mode is selected.
  * The resonance law is an approximation.
* **8580 filter:** an idealised linear curve and resonance law.
* **DC and levels:** the waveform zero level (`$380`) and mixer DC
  (1.0 / 0.05) are calibrated estimates, so digi loudness is approximate.
  The volume DAC is linear. Signal leakage at volume 0 is not modelled.
* **Floating DAC and data bus:** both hold their value and then drop to 0
  in one step; on the chip the bits fade individually.
* **Pots:** POTX and POTY are static values. The 512-cycle conversion cycle
  is not modelled.
* **External audio input:** ignored.
* **Output stage:** first-order RC approximations.
