# vic2: a cycle-exact MOS 6569 (PAL) VIC-II

This is a drop-in replacement for `harness/src/vic.c` / `vic.h`. It uses the
same pin interface, the same struct field names and the same framebuffer
layout. I wrote it from Christian Bauer's article *The MOS 6567/6569 video
controller (VIC-II) and its application in the Commodore 64*, revision
2024-09-29 (<https://www.cebix.net/VIC-Article.txt>). Section numbers below
(for example "3.8.1") refer to that article. No third-party emulator code was
read, downloaded or run.

| file | contents |
|---|---|
| `vic.h` | the API: the harness contract plus additions marked `[added]` |
| `vic.c` | the implementation, about 610 lines with no dependencies |
| `victest.c` | the test bench (pin-level, scripted CPU, PNG output) |
| `Makefile` | `make test` builds `victest`, runs it and writes `out/*.png` |

`make test` currently reports **420 checks, 0 failed**, at about
**20–29 M VIC cycles/s** on this Mac with 8 sprites, text and per-line
register writes. That is roughly 20–29× real time and about 400× faster than
the netlist CPU, which runs at about 50 k cycles/s.

---

## 1. What is modelled

* **Frame geometry**: 63 cycles × 312 lines, cycle numbers 1..63 as in the
  article. RASTER increments in cycle 1, except line 0, where RASTER (and the
  raster compare) change in cycle 2. In cycle 1 of line 0, `$d012` still
  reads $37 and `$d011` bit 7 reads 1 (3.6.3).
* **Raster IRQ**: compared in cycle 1 (cycle 2 for line 0), and also when
  `$d011`/`$d012` is written, so a write that hits the current line triggers
  immediately. At most one raster IRQ per line (3.12). `$d019` and `$d01a`
  are implemented, with bit 7 = IRQ output. Unused register bits read as 1.
  `$d02f`–`$d03f` read $FF.
* **Bad Line Condition**: evaluated at the start of every cycle as
  `DEN-latch && $30<=RASTER<=$f7 && (RASTER&7)==YSCROLL` (3.5). The DEN
  latch is set by DEN being 1 in any cycle of line $30, including a write
  during that line, and is cleared at line 0. A mid-line `$d011` write
  creates or cancels the condition from the next cycle on, which covers FLD,
  FLI, linecrunch, doubled lines and DMA delay (VSP).
* **BA/AEC**:
  * BA is low in cycles 12..54 while the condition holds, and during sprite
    DMA from 3 cycles before the first s-access to the last one.
  * AEC is low in phase 2 only when the VIC needs that phase (c- or
    s-access) **and** BA has been low for at least 3 cycles (2.4.3).
  * A c-access while AEC is still high reads $FF, with the colour nybble
    taken from the CPU data bus (3.14.6).
  * A normal badline steals 40 cycles plus 3 BA-only cycles. One sprite
    steals 2 cycles plus 3 BA-only cycles. All 8 sprites keep BA low from
    cycle 55 through cycle 10 and steal 16 cycles per line.
* **Memory accesses** (3.6.3): the VIC makes an access in *every* phase 1:
  g-accesses (cycles 16..55), p-accesses (always made), s-accesses, refresh
  in cycles 11..15 (REF counter reset to $FF in line 0, 5 decrements per
  line, address `$3F00|REF`) and idle accesses at $3FFF. Phase-2 accesses
  are c-accesses (cycles 15..54) and s-accesses.
* **VC, VCBASE, RC, VMLI**: implemented exactly as in rules 1–5 of 3.7.2.
  Idle and display state follow 3.7.1. Idle g-accesses go to $3FFF, or
  $39FF when ECM is set, and are displayed with c-data 0. ECM holds address
  lines A9/A10 low in all g-accesses.
* **All 8 graphics modes** (3.7.3), including foreground/background
  classification. The three invalid modes output black but still generate
  foreground pixels for collisions and priority.
* **Sequencer**: XSCROLL delays the shift-register load. Raising XSCROLL in
  mid-line inserts background pixels; lowering it truncates the current
  character (3.7.3).
* **Border unit** (3.9): the main and vertical flip-flops with the exact
  compare values for CSEL/RSEL. The vertical check happens in cycle 63 and
  at the left compare. DEN gates the reset. When the vertical flip-flop is
  set, the sequencer output is turned off (background colour, no sprite/data
  collisions). Hyperscreen and side-border tricks work.
* **Sprites** (3.8.1, 2024 revision):
  * MC/MCBASE, and the "advance line" (Y-expansion) flip-flop rules 1–7.
  * The 7a crunch formula `MCBASE = (101010 & (MCBASE & MC)) | (010101 & (MCBASE | MC))`
    when MxYE is cleared in cycle 15 while the flip-flop is clear.
  * DMA-on checks in cycles 55 and 56, display on/off in cycle 58, DMA off
    in cycle 16 when MCBASE = 63.
  * X expansion, multicolour, 9th X bit, 8-bit Y compare (so Y < 56 also
    matches raster 256+Y on PAL), X $1F8–$1FF never reached.
  * Fixed sprite–sprite priority. MxDP uses the 2024 rule: the foreground
    "inherits" the priority of the topmost opaque sprite.
  * `$d01e`/`$d01f` collisions are cleared on read. The IRQ fires only when
    the register was 0 before.
* **Output**: `fb[284][403]` palette indices, unchanged from the harness
  (row 0 = raster 16, column 0 = X $1E0). `vic_crop_normal*()` produces
  the VICE "normal borders" 384×272 crop. `vic_palette` is Pepto PAL.
* **Not modelled**: the light pen. `$d013`/`$d014` read 0 and the LP IRQ
  never fires.

## 2. Coordinates and the pixel pipeline

* Cycle *n* starts at X-counter value `Xd(n) = $194 + 8(n-1) mod $1F8`
  (3.4, 3.6.3). So cycle 1 is at $194, cycle 13 at $1F4, cycle 14 at $004,
  and for n ≥ 14, `Xd(n) = 8n - 108`.
* **Output lag.** The video output lags the X counter by 8 pixels. Bauer
  says the border appears "approx. 8 pixels later than shown in the Graph.
  line", and that g-data is shown 12 pixels after its access (3.6.1, 3.6.3).
  So the 8 pixels emitted during cycle *n* are the sprite-coordinate
  positions ("display X") `Xd(n)-8 .. Xd(n)-1`.
* The sequencer, the sprites and the border comparators all work in display
  X, so they agree with each other:
  * a sprite at X=24 lines up with column 0 and with the CSEL=1 left border
    edge;
  * with XSCROLL=0, character column *k* occupies X `24+8k .. 31+8k`;
  * the first badline with YSCROLL=3 is raster 51, which is the first line
    of the 25-row window.
* **Sequencer model.** At display X `24+8k` the g-data of column *k*
  (fetched in cycle `16+k`) is offered in a latch. It is loaded into the
  shift register at the pixel where `X & 7 == XSCROLL`. The latch empties
  when loaded, and at X=344.

## 3. Per-cycle table

Columns:

* **X**: X counter at the start of the cycle.
* **out X**: the display-X range this cycle outputs.
* **φ1 / φ2**: memory accesses in each phase:
  * `g` = g-access (character or bitmap data)
  * `c` = c-access, only while the Bad Line Condition holds
  * `pN` = sprite N data-pointer fetch
  * `sN` = sprite N data fetch; `sN / i` = only while that sprite's DMA is
    on, otherwise an idle access
  * `r` = DRAM refresh
  * `i` = idle access at $3FFF
* **BA low if**: `BL` = Bad Line Condition (cycles 12..54); `spr` = DMA
  active for the listed sprites.

| cyc | X | out X | φ1 | φ2 | BA low if | internal events (phase 1 unless noted) |
|---|---|---|---|---|---|---|
|  1 | $194 | $18C-$193 | p3         | s3      | spr 3,4        | RASTER++ and raster compare (line 0: both in cycle 2) |
|  2 | $19C | $194-$19B | s3 / i     | s3      | spr 3,4,5      | (line 0 only) RASTER=0, raster compare |
|  3 | $1A4 | $19C-$1A3 | p4         | s4      | spr 4,5        |  |
|  4 | $1AC | $1A4-$1AB | s4 / i     | s4      | spr 4,5,6      |  |
|  5 | $1B4 | $1AC-$1B3 | p5         | s5      | spr 5,6        |  |
|  6 | $1BC | $1B4-$1BB | s5 / i     | s5      | spr 5,6,7      |  |
|  7 | $1C4 | $1BC-$1C3 | p6         | s6      | spr 6,7        |  |
|  8 | $1CC | $1C4-$1CB | s6 / i     | s6      | spr 6,7        |  |
|  9 | $1D4 | $1CC-$1D3 | p7         | s7      | spr 7          |  |
| 10 | $1DC | $1D4-$1DB | s7 / i     | s7      | spr 7          |  |
| 11 | $1E4 | $1DC-$1E3 | r          | -       | -              |  |
| 12 | $1EC | $1E4-$1EB | r          | -       | BL             |  |
| 13 | $1F4 | $1EC-$1F3 | r          | -       | BL             |  |
| 14 | $004 | $1F4-$003 | r          | -       | BL             | VC=VCBASE, VMLI=0; RC=0 if bad line |
| 15 | $00C | $004-$00B | r          | c (BL)  | BL             |  |
| 16 | $014 | $00C-$013 | g (col 0)  | c (BL)  | BL             | sprites: MCBASE=MC (or crunch value) if flip-flop set; DMA off if MCBASE=63 |
| 17 | $01C | $014-$01B | g (col 1)  | c (BL)  | BL             | left border compare X=24 (CSEL=1) is in this cycle's output |
| 18 | $024 | $01C-$023 | g (col 2)  | c (BL)  | BL             | left border compare X=31 (CSEL=0) |
| 19 | $02C | $024-$02B | g (col 3)  | c (BL)  | BL             |  |
| 20 | $034 | $02C-$033 | g (col 4)  | c (BL)  | BL             |  |
| 21 | $03C | $034-$03B | g (col 5)  | c (BL)  | BL             |  |
| 22 | $044 | $03C-$043 | g (col 6)  | c (BL)  | BL             |  |
| 23 | $04C | $044-$04B | g (col 7)  | c (BL)  | BL             |  |
| 24 | $054 | $04C-$053 | g (col 8)  | c (BL)  | BL             |  |
| 25 | $05C | $054-$05B | g (col 9)  | c (BL)  | BL             |  |
| 26 | $064 | $05C-$063 | g (col 10) | c (BL)  | BL             |  |
| 27 | $06C | $064-$06B | g (col 11) | c (BL)  | BL             |  |
| 28 | $074 | $06C-$073 | g (col 12) | c (BL)  | BL             |  |
| 29 | $07C | $074-$07B | g (col 13) | c (BL)  | BL             |  |
| 30 | $084 | $07C-$083 | g (col 14) | c (BL)  | BL             |  |
| 31 | $08C | $084-$08B | g (col 15) | c (BL)  | BL             |  |
| 32 | $094 | $08C-$093 | g (col 16) | c (BL)  | BL             |  |
| 33 | $09C | $094-$09B | g (col 17) | c (BL)  | BL             |  |
| 34 | $0A4 | $09C-$0A3 | g (col 18) | c (BL)  | BL             |  |
| 35 | $0AC | $0A4-$0AB | g (col 19) | c (BL)  | BL             |  |
| 36 | $0B4 | $0AC-$0B3 | g (col 20) | c (BL)  | BL             |  |
| 37 | $0BC | $0B4-$0BB | g (col 21) | c (BL)  | BL             |  |
| 38 | $0C4 | $0BC-$0C3 | g (col 22) | c (BL)  | BL             |  |
| 39 | $0CC | $0C4-$0CB | g (col 23) | c (BL)  | BL             |  |
| 40 | $0D4 | $0CC-$0D3 | g (col 24) | c (BL)  | BL             |  |
| 41 | $0DC | $0D4-$0DB | g (col 25) | c (BL)  | BL             |  |
| 42 | $0E4 | $0DC-$0E3 | g (col 26) | c (BL)  | BL             |  |
| 43 | $0EC | $0E4-$0EB | g (col 27) | c (BL)  | BL             |  |
| 44 | $0F4 | $0EC-$0F3 | g (col 28) | c (BL)  | BL             |  |
| 45 | $0FC | $0F4-$0FB | g (col 29) | c (BL)  | BL             |  |
| 46 | $104 | $0FC-$103 | g (col 30) | c (BL)  | BL             |  |
| 47 | $10C | $104-$10B | g (col 31) | c (BL)  | BL             |  |
| 48 | $114 | $10C-$113 | g (col 32) | c (BL)  | BL             |  |
| 49 | $11C | $114-$11B | g (col 33) | c (BL)  | BL             |  |
| 50 | $124 | $11C-$123 | g (col 34) | c (BL)  | BL             |  |
| 51 | $12C | $124-$12B | g (col 35) | c (BL)  | BL             |  |
| 52 | $134 | $12C-$133 | g (col 36) | c (BL)  | BL             |  |
| 53 | $13C | $134-$13B | g (col 37) | c (BL)  | BL             |  |
| 54 | $144 | $13C-$143 | g (col 38) | c (BL)  | BL             |  |
| 55 | $14C | $144-$14B | g (col 39) | -       | spr 0          | sprites: DMA-on check (MxE, MxY=RASTER&255) |
| 56 | $154 | $14C-$153 | i          | -       | spr 0          | DMA-on check again; phi2: Y-exp flip-flop ^= MxYE (DMA on); right compare X=335 (CSEL=0) in output |
| 57 | $15C | $154-$15B | i          | -       | spr 0,1        | right border compare X=344 (CSEL=1) in output |
| 58 | $164 | $15C-$163 | p0         | s0      | spr 0,1        | MC=MCBASE; sprite display on/off; RC==7 -> VCBASE=VC, idle unless BL; display -> RC++ |
| 59 | $16C | $164-$16B | s0 / i     | s0      | spr 0,1,2      |  |
| 60 | $174 | $16C-$173 | p1         | s1      | spr 1,2        |  |
| 61 | $17C | $174-$17B | s1 / i     | s1      | spr 1,2,3      |  |
| 62 | $184 | $17C-$183 | p2         | s2      | spr 2,3        |  |
| 63 | $18C | $184-$18B | s2 / i     | s2      | spr 2,3,4      | vertical border compare (top/bottom with RSEL, DEN) |

AEC is low in phase 2 of a cycle with a φ2 access, but only from the 4th
consecutive BA-low cycle on.

## 4. When a CPU write takes effect

The board does register accesses in the PHI2 call. Each write is applied in
two places:

**Cycle-level logic.** This covers the Bad Line Condition, DEN latch,
g/c/p-access addressing (`$d018`, ECM/BMM for addressing), sprite
enable/Y compare, the YSCROLL/RSEL/DEN cycle-63 check and `$d017`:

* A write in cycle *n* is seen from the PHI1 of cycle *n+1*. Bauer defines
  the condition "at the negative edge of φ0 at the beginning of the cycle"
  (3.5).
* So a `$d011` write in cycle 55 creates a badline condition in cycles
  56..63 without DMA. This is what the intro relies on (section 7).
* A `$d018` write in cycle *n* changes the g-access of cycle *n+1*.

**Applied at the moment of the write:**

* the raster compare after a `$d011`/`$d012` write (3.12);
* the DEN latch in line $30;
* `$d017` rule 1 (clearing MxYE sets the flip-flop at once) and the rule-7a
  crunch detection (write in cycle 15);
* `$d019` acknowledge and `$d01a` mask;
* rule 3 (flip-flop inversion) runs in phase 2 of cycle 56 *after* a CPU
  write in that cycle, so a `$d017` write in cycle 56 counts. This follows
  the 2024 text "second phase of cycle 56".

**Pixel pipeline (`preg[]`).** Within the 8 pixels output during the write
cycle (`Xd(n)-8 .. Xd(n)-1`), a write to register *r* becomes visible from
pixel index `wdelay[r]` (0..8). The defaults set in `vic_init` are:

| registers | `wdelay` | first display X with the new value | reasoning |
|---|---|---|---|
| everything except colours: `$d011` ECM/BMM/RSEL/DEN, `$d016` XSCROLL/MCM/CSEL, sprite X/`$d010`, `$d01b`, `$d01c`, `$d01d` | 8 | `Xd(n)`, the first pixel output in cycle n+1 | Latch at the end of φ2. See the derivation below. |
| colour registers `$d020`–`$d02e` | 4 | `Xd(n)-4`, mid-cycle | The register changes during φ2, which starts at counter `Xd(n)+4`. Minus the 8-pixel output lag, that is display X `Xd(n)-4`. This is my own inference: the article gives no pixel position for colour writes. The 8565 "grey dot" is not modelled because the 6569 does not have it. |
| reads of `$d01e`/`$d01f` | 4 | | Collisions found in pixels 0..3 of the read cycle are included in the value read and cleared; those in 4..7 are not. |

**Derivation of the 8 default.** Section 3.14.1 says CSEL must go
1→0 *exactly in cycle 56* to open the side border. Going 0→1 in cycle 17
must be the only way to keep it closed. With the 8-pixel output lag, these
statements hold only for an effect delay between 5 and 11 pixels. I checked
both directions (55/56/57 and 16/17/18) against compare values 24/31/335/344.
A delay of 8 means "latched at the end of φ2" and sits in the middle of that
window. Test T6 verifies that cycles 55 and 57 fail and cycle 56 opens the
border.

The values are per-register fields (`v->wdelay[]`), so the integrator can
tune them against reference shots without touching the code. One limit: the
sprite start positions for a cycle are computed once per output run, so
sprite X writes are only exact with a delay of 0 or 8.

## 5. Deviations, choices and unknowns

* **Output lag.** The 8-pixel lag is "approx." in the article. Screenshot
  alignment does not depend on it, because everything is in sprite
  coordinates. Only the pixel positions of register writes do.
* **Sprite shift register.**
  * s-access data is collected into a 24-bit line that is "armed". It
    starts shifting at the X match. The real chip shifts s-data straight
    into the shift register, so a sprite caught shifting during its own
    s-accesses would glitch on hardware. That case is not modelled.
  * A sprite to the right of X≈$164 shows its first line in the Y-match
    line (3.8.1). Here that starts from about X $174, because the data is
    complete at cycle 60. The difference is only visible with an opened
    side border.
  * X-expansion or multicolour changes in the middle of a sprite are
    applied per pixel. Hardware quirks there are not modelled.
* **Y-expansion rule 3 timing.** The pre-2024 article says "first phase of
  cycle 55"; the 2024 revision says "second phase of cycle 56". I follow
  2024. The task brief said cycle 55. Both give identical results unless
  `$d017` is written in cycles 55–56. The intro writes it in cycles 30 and
  43/44 in its stretch loop (from my reading of its source, see 7).
* **Crunch** applies only to a write in cycle 15, as rule 7a says.
* **VMLI** is a plain 6-bit counter, the simplification the article itself
  uses (3.7.2).
* **c-access while AEC is high**:
  * The character is $FF.
  * The colour nybble is the CPU's D0–D3. That byte comes from the optional
    `VIC_CPUD` pin; without it the VIC uses the last byte it saw in a CPU
    register access, which usually is the `$d011` write that started the
    badline.
  * The same "$FF" rule covers an s-access when sprite DMA started in cycle
    56. In that case BA is low for fewer than 3 cycles before cycle 58.
* **Sequencer before the first load and after column 39** (only visible in
  an opened side border): it outputs "0" pixels with the attributes of the
  last loaded character. In text modes that is `$d021`. In bitmap modes it
  is the last character's colours, which is an unknown.
* **Multicolour text mode, hires characters** (colour bit 3 = 0): pixels
  are classified as foreground/background per bit. The article says MCM
  alone decides; I read that as applying to multicolour characters.
* **Unspecified timings**: VCBASE is reset and the DEN latch cleared in
  line 0. The article says the exact moment is "irrelevant".
* **Framebuffer coverage**: lines 0–15 and 300–311 are processed
  (collisions, flip-flops) but not stored. `fb` is drawn in place with no
  double buffer. At the frame boundary (line 0, cycle 1) it holds a complete
  frame.
* **`badlines` / `last_badlines`** count lines in which BA went low for a
  badline (cycles 12..54). The intro's FPP lines therefore count as 0.

## 6. API: the harness contract plus `[added]` items

Unchanged:

* `vic_init`, `vic_tick`, `vic_screen_addr` and `vic_palette` (Pepto).
* All pin bits 0..34.
* `VIC_SCREEN_*` and `fb`.
* The fields `reg`, `cycle`, `line`, `frame`, `raster_match`, `irq_latch`,
  `irq_mask`, `den_frame`, `badline`, `display`, `vc`, `vcbase`, `rc`,
  `vmli`, `ba_low`, `ba`, `aec`, `fetch_kind`, `fetch_col`, `cbuf`,
  `colbuf`, `gdata`, `gchar`, `gcol`, `gidle`, `vborder`, `mborder`,
  `badlines`, `last_badlines`, `raster_irqs`.
* `raster_match` now means "raster IRQ already triggered in this line".

Added:

| item | purpose |
|---|---|
| `VIC_CPUD` (bit 35, in), bits 36..43 | optional: the CPU data byte of this PHI2, used for the colour nybble of AEC-high c-accesses |
| `VIC_CROP_W`/`VIC_CROP_H` (384×272) | size of the VICE "normal borders" crop |
| `vic_crop_normal(v, dst)` | crop as palette indices. Column 0 = X $1F0 (fb column 16), row 0 = raster 16. The display window lands at (32,35), so `demo/minimalistic/dl/185573.png` aligns at (0,0). |
| `vic_crop_normal_rgb(v, rgb, pal)` | same crop as RGB bytes. `pal` = NULL means Pepto. |
| `vic_palette_pepto[16]` | Pepto PAL, identical to `vic_palette` |
| `vic_raster(v)` | RASTER as the CPU reads it (handles the line-0 special case) |
| fields `preg[64]`, `wdelay[64]` | pixel-pipeline register view and the per-register write delay (section 4) |
| fields `raster_cmp`, `refresh`, `fetch2_kind/idx`, `p2_kind/idx`, `fetch_addr` | internal state |
| fields `sq_*` | graphics-sequencer state |
| fields `mc`, `mcbase`, `sptr`, `sdma`, `sdisp`, `yexp_ff`, `crunch`, `sarmed`, `sfetch`, `sdata`, `sshift`, `spix`, `sactive`, `col_ss`, `col_sb` | sprite state |
| fields `cpu_bus`, `lpx`, `lpy`, `x0`, `line_badline` | internal state |

## 7. Integration notes for `harness/src/c64.c`

**No change to `c64.c` is required.** Replace `harness/src/vic.c` and
`vic.h` with these files, or compile `../vic2/vic.c` with `../vic2/vic.h`
taking precedence. Things the integrator should know:

1. **The VIC now raises `VIC_FETCH` in every PHI1.** The old harness VIC
   only did so for g-accesses. `vic_fetch()` therefore runs every cycle, and
   `m->open_bus` now follows the real phase-1 accesses (idle $3FFF, refresh,
   sprite pointers), as in Bauer section 4 ($de00 area). The cost, one
   `c64_vic_read` per cycle, is negligible next to the netlist CPU.
2. **Phase-2 fetches also happen for sprite s-accesses**, not only for
   c-accesses. `c64.c` already answers any `VIC_FETCH` after the PHI2 call.
3. **Register-write cycles.** In step 5 the board overwrites D0..D11 with
   the CPU write data, which destroys the phase-1 fetch data. The VIC
   detects `VIC_CS && !VIC_RW`, re-issues its phase-1 address with
   `VIC_FETCH` in the PHI2 output, and consumes the answer at the next PHI1.
   This is exact for three reasons:
   * in that cycle the CPU wrote to the VIC, so RAM, colour RAM and the CIA2
     bank bits are unchanged;
   * the VIC never needs PHI2 for itself while the CPU owns it;
   * the re-fetched g-data is not needed before the next cycle.

   The old harness VIC had a bug here: it took the CPU write value as
   g-data. That bug would have corrupted the intro's chessboard wherever it
   writes `$d017`, `$d011` or `$d018` during g-access cycles.
4. **Optional exact VSP/FLI colour nybble.** In step 5, before
   `vic_tick`, add:
   `vp = (vp & ~(0x1FFULL << 35)) | (aec ? VIC_CPUD | ((uint64_t)(rw ? data : wdata) << VIC_CPUD_SHIFT) : 0);`
   Here `data` is the byte the CPU read this cycle. VIC register reads
   could be ignored for this purpose. The intro does not need it: its only
   real badline (raster 51) starts at cycle 1.
5. `c64_vic_read()` feeds the PLA a BA value from `m->vic_pins`, i.e. the
   previous cycle. This is harmless without cartridges.
6. The frame-boundary check `line == 0 && cycle == 1`, `last_badlines`,
   `raster_irqs`, `frame` and `vic_screen_addr()` behave as before.
7. **Screenshots for comparison.** `main.c` writes the 403×284 `fb`. To
   compare against `185573.png`, use `vic_crop_normal_rgb()` + `png_write_rgb()`
   (the reference aligns at 0,0), or tell `cmpshot.py` that the crop sits
   at fb offset (16,0).

### What the intro needs, and where to look if it renders wrong

This is from my reading of `minimalistic.asm` plus cycle counting. I have
not checked it against a run.

* **FPP.** `$d011=$53` is written at the end of each frame: DEN=1, ECM=1,
  YSCROLL=3. So line 51 becomes the only real badline, and it loads
  `$0400-$0427` into the video-matrix line.
* **Loop timing.** In the per-line loops (e.g. `fpploop1`, 45 CPU cycles),
  the sprite DMA of sprites 0–7 lets the CPU run only in cycles 11..54 plus
  write cycles. That pins `sta $d011,x` to **cycle 55**. YSCROLL = RASTER&7
  then gives a badline condition only in cycles 56..63:
  * no DMA;
  * the display state is kept at cycle 58 and RC keeps counting;
  * the characters stay those of row 0, and every line of the zoomer chars
    is identical;
  * `$d018` switches per line between the two 16-character sets at $0100
    (inverted) and $0900, which gives the chessboard.
* **Sprite stretch.** `stx $d017` (=0) lands in cycle 30 and `dec $d017`
  writes $00/$FF in cycles 43/44. The flip-flop is set between them and
  inverted in cycle 56. It is therefore never set in cycle 16, MCBASE
  freezes and the same sprite line repeats on every raster line. Test T5
  reproduces exactly this pattern.
* **Reference frame** (crop coordinates): window raster 55..223 (crop rows
  39..207), 38 columns (X 31..334), chessboard colour 6 on black. The
  scroller is colour 4, rows 52..77, with blank lines between. The logo is
  colour 15, rows 40..46.
* **First checks if it renders wrong**:
  1. Does the `$d011` write really land in cycle 55? Use `--trace` and look
     for `BA` from cycle 55 with 8 sprites.
  2. Are `$d018` writes visible one cycle later?
  3. The `wdelay` defaults.

## 8. Tests (`make test`; images in `out/`, 2× scaled 384×272 crops)

The bench drives the pins exactly like `c64.c`: PHI1, then answer the
fetch, then the scripted CPU (allowed only while AEC is high), then PHI2 with
write data clobbering D, then answer the fetch. It counts any CPU access
made while AEC was low and fails the test.

| test | checks | images |
|---|---|---|
| T1 | BA/AEC on every cycle of a whole frame: badlines (BA low 12..54, AEC low 15..54, exactly 40 + 3 per badline, 25 per frame); each sprite alone (5 BA-low / 2 stolen cycles per line, 21 lines, s-access addresses); all 8 sprites (19 BA-low / 16 stolen per line); a badline with 8 sprites (BA high only in cycle 11); g/c/p/refresh/idle addresses | |
| T2 | raster IRQ asserted in PHI1 of cycle 1 (line 0: cycle 2); `$d012` reads 99→100 across a line boundary; line 0 cycle 1 still reads 311; immediate trigger on writing the current line; never twice per line; `$d019`/`$d01a` | |
| T3 | full-frame pixel compare against a model built from the article's tables, for all 8 modes, XSCROLL 5 / YSCROLL 7 / 38×24, YSCROLL 0 (idle-state stripes); a known diagonal glyph exactly at X 24 / line 51 | `mode0_text` … `mode7_invalid_bm2`, `text_xs5_ys7_38x24`, `text_ys0_idle_stripes` |
| T4 | FPP. Classic version (write `$d011` in cycle 2): each line a full DMA badline showing row 0, pixel line 0. Intro version (write in cycle 55): only one DMA line (51), every line shows row 0's characters with RC = (y-51)&7 and the per-line charset from `$d018` | `fpp_classic`, `fpp_intro_style` |
| T5 | Stretch: `$d017` cleared in cycle 30 and set in cycle 44 on lines 70..89, the intro's pattern. Displayed MC per line matches the article model, row 10 frozen for 21 lines, then normal doubling. Crunch: MxYE cleared in cycle 15 gives MCBASE 0→1 (Bauer's example), then 1,4,…,61,0,3,…,60, so the sprite is shown on 85 lines against 42 for the control | `sprite_stretch`, `sprite_crunch` |
| T6 | RSEL 1→0 at line 249 opens the lower and upper border (sprite visible, side border still active); CSEL→0 in cycle 56 opens the right border, cycles 55 and 57 don't | `border_closed`, `border_open_rsel`, `border_open_csel` |
| T7 | sprite–sprite and sprite–data collisions, clear-on-read, IRQ only when the register was 0, priority, MxDP, multicolour + X-expanded pixel widths | `sprites_collision` |
| T8 | DEN set after line $30 gives no badlines; DMA delay: condition from cycle 20 gives BA low from 20, AEC low from 23, three $FF c-accesses | `dma_delay` |
| T9 | performance, required ≥ 10 M cycles/s | `perf_frame` |
