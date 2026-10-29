# Minimalistic Intro on our emulator: fidelity report

**Demo:** "Minimalistic Intro" by Proton / Finnish Gold, a C64 4K intro from the CSDb Intro Creation Competition 2019 (CSDb release 185573). Proton did the code, music and design.

**What the creator intended**, taken only from his own release material:

| Aspect | Source | Value |
|---|---|---|
| Video standard | `minimalistic.asm`: the NMI timer latch is `$4CC7` (period 19,656 cycles = 312 × 63, exactly one PAL frame); `.sid` header flag | PAL |
| SID chip | `.sid` header flags in the release | MOS 6581 |
| How it's started | source comments: VICE `-autostartprgmode 1` (inject the PRG into RAM, then RUN) | `--prg` does the same: PRG injected at `READY.`, `RUN` typed |
| Look | CSDb release screenshot (`dl/185573.png`, VICE normal borders, Pepto palette) | used as the pixel reference |
| Music | the creator's own `.sid` export of the tune | used as the register-write reference |

## The machine

- **CPU:** the visual6502 transistor netlist (3,510 transistors traced from die photos), simulated at switch level, with the 6510 I/O port added by hand.
- **PLA:** logic equations dumped from a real 906114 chip.
- **VIC-II:** cycle-exact 6569, written from Christian Bauer's VIC article.
- **SID:** our own 6581 model.
- **CIA:** 6526 timer pipeline after Wolfgang Lorenz's write-up.
- **ROMs:** original KERNAL 901227-03, BASIC 901226-01 and CHARGEN 901225-01.

All code is our own: no third-party emulator code was used.

## Evidence

| Check | Result |
|---|---|
| **Visuals vs creator's screenshot** | Frame 818 matches `185573.png` on **100.000 % of 104,448 pixels** (C64 colour-index level), alignment (0,0), no tolerance. That one frame covers the per-line FPP `$D011`/`$D018` stretching, per-line `$D017` sprite stretching, the chessboard zoom/position, the sprite scroller position and the FIG logo. |
| **Music vs creator's `.sid`** | 191 of 191 player calls from the creator's own `.sid` export appear in the intro's SID write stream, write for write and value for value, starting at the intro's first music frame and running at one call per PAL frame. |
| **Interrupt timing** | Start screen: exactly 1 CIA2 NMI per frame (236 frames). Main part: exactly 1 raster IRQ per frame. |
| **Full run** | 3,750 frames (74.8 s): boot, bouncing-raster screen, title, one complete 361-character scroll-text loop and its wrap-around. |
| **Determinism** | Two independent transistor-level runs are byte-identical on all frames they share. |
| **Fast core = transistor core** | The `--cpu fast` run is identical to the transistor-level run: all 3,750 frames, the WAV byte for byte, all 60,994 cycle-stamped SID writes, and the per-frame IRQ/NMI/badline/stall log. The fast core is also lockstep-verified against the netlist over 58 M cycles of instruction, interrupt and RDY tests. |
| **Compiled netlist core = transistor core** | `--cpu compiled` (the netlist compiled to C, node-identical to the switch-level simulation over 60 M test cycles) reproduces the full run exactly: all 3,750 frames, the WAV, all 60,994 SID writes and the per-frame log, at 1.7× the netlist's speed. |
| **Real-time playback** | `c64player`: 50.18 fps over 60 s (PAL 50.125; the extra is the 3-frame audio prefill), 0 audio underruns. Pacing is locked to the audio clock. |
| **Undocumented opcodes** | The netlist differs from real silicon on ANC/ALR/ARR/LAS. The intro's source uses no undocumented opcodes. |

## What is not verified

- **The 6581 filter sound.** The tune routes every voice through the filter at high resonance (`$D417` = `$F7`/`$61`), in HP+BP and HP+LP modes, with the cutoff sweeping through the 6581's steep mid range. Our filter is our own model, calibrated to published 6581 measurements. Real 6581s differ audibly from chip to chip, and the creator published no recording. Notes and timing are exactly the creator's; the timbre is a 6581 approximation, not his specific chip.
- **Only one reference image.** Only one frame has a creator reference. All other frames come from the same deterministic code path.
- **Border pixels on raster bars.** The bouncing-raster start screen depends on exact CIA start-delay cycles, which follow Lorenz's description. Nothing published lets us check it pixel by pixel.

## Reproduce

```bash
cd harness && make c64 c64player
./c64 --cpu fast --rom-dir roms --prg ../demo/minimalistic/prg/minimalistic_fig.prg --frames 3750 --dump-frames out/fast/frames --wav out/fast/intro_fast.wav
python3 -I ../demo/tools/cmpshot.py ../demo/minimalistic/dl/185573.png out/fast/frames --align 0,0 --step 1
./c64player --rom-dir roms --prg ../demo/minimalistic/prg/minimalistic_fig.prg            # real time, with sound
./c64 --cpu netlist ...   # same run on the transistor-level CPU (~15 min), bit-identical output
```

Videos: `minimalistic_full_netlist.mp4` (full run, transistor-level CPU) and `minimalistic_preview_900f.mp4`.
