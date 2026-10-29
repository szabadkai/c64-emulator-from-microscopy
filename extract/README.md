# Milestone 2: die photo → layer masks → transistor netlist

This pipeline is validated on the MOS 6502, where the visual6502 hand-traced netlist is the
ground truth (GT). It then makes a first attempt on the VIC-II 6569 R1.

Summary of results:

* **Geometry → netlist works.** Perfect layer masks at 0.44 µm/px recover 99.49 % of the GT
  transistors, and 99.97 % of the extracted devices are correct. All 1017 depletion
  pull-ups are found. The node matching uses only the pad names.
* **The extractor needs at least ~0.9 µm/px** to stay above 95 % (≈0.33 px per µm of
  feature). At 3 µm/px it fails completely.
* **Segmenting the photos with classical CV is the bottleneck.** The 6502 photos align
  to the GT with sub-pixel accuracy, so the masks can be scored. Metal reaches IoU
  0.76. Diffusion from the delayered photo reaches 0.51. Exposed poly reaches only 0.18.
  These masks are far too poor to feed the extractor.
* **On the VIC-II, only metal can be recovered from these photos.** Against Tommi
  Lempinen's metal trace, metal reaches IoU 0.76–0.83 (0.85–0.90 with a 2 px boundary
  band excluded). Poly, diffusion and buried contacts cannot be told apart, and nothing
  under metal is visible. The netlist step returns essentially nothing, as expected.

## Layout

```
extract/
  .venv/                 python3.14 venv (numpy scipy pillow opencv-python-headless scikit-image shapely networkx)
  src/                   all code (each stage has an argparse CLI, see below)
  data/visual6502/repo   git clone of trebonian/visual6502 (segdefs.js, transdefs.js, nodenames.js)
  data/visual6502/images 6502 photos + layer renders from visual6502.org/images/6502/ (~125 MB)
  data/retronn/          6569R1.jpg (64 MB) + 3 raw tiles (0707/0708/0807, scale calibration only)
  data/wmhost/           Tommi Lempinen: MOS_6569R1_L1_metal.png (metal trace), MOS_6569_R1.jpg
  out/roundtrip/         6502 round trip: per-scale netlists, comparison reports, sweep table, overlays
  out/align6502.json     6502 photo <-> segdefs registration
  out/seg6502/           6502 photo segmentation report, masks, overlay PNGs
  out/vic2/              VIC-II overview/tile index, metal registration, region reports, overlays, netlists
```

Run every script that reads downloaded files with `-I`, from `extract/`:
`.venv/bin/python -I src/<script>.py ...`. The scripts add `src/` to `sys.path` themselves.

## Stage 1: ground-truth loader (`src/gt6502.py`)

* Reads `segdefs.js`, `transdefs.js` and `nodenames.js` with regexes, as text. Nothing is
  executed.
* Layer indices come from `expertWires.js` (`layernames`):
  `0 metal, 1 switched diffusion, 2 inputdiode, 3 grounded diffusion, 4 powered diffusion, 5 polysilicon`.
* The data contains 8233 polygons, 1704 nodes, 3510 enhancement transistors and 1018
  pull-up nodes (`'+'`). Depletion loads are flags, not transistors. The `extra` field
  of each transistor is `[w1, w2, L, nseg, area]`, with L between 12 and 35 units.
* Facts found in the data that affect extraction:
  * **No vcc/vss metal at all.** vss consists only of grounded diffusion (+ inputdiode),
    and vcc only of powered diffusion.
  * The 17 `inputdiode` polygons are pad-protection structures. They are labelled vss
    and abut both pad diffusion and ground diffusion. The 17 GT transistors with
    gate = vss (always off) are exactly these protection devices.
  * Polygons of different nodes on the same layer are always ≥ 6 units apart, except
    those 17 abutments.
  * Diffusion polygons stop at gate edges, so channels are gaps (self-aligned process).
* Scale: the polygons span 8769 × 9628 units, and the die is 3.9 × 4.3 mm. That gives
  **1 unit ≈ 0.444 µm**.
* CLI: `gt6502.py summary`, and `gt6502.py raster --scale 0.25 --out x.npz [--png x.png]`.

## Stage 2: geometry → netlist extractor (`src/extract.py`)

Input: boolean masks for `metal`, `poly`, `diff` (doped diffusion), `contacts` (cuts),
`buried`, and optionally `vss_seed`/`vcc_seed`, plus a JSON sidecar
`{scale, anchors:{name:{row,col,layer}}}`. The input never contains node IDs.

The algorithm:

1. **Channels.** Scan lines at 0/90/45/135° find runs of non-diffusion pixels that are:
   * fully covered by poly,
   * bounded by two *different* diffusion components,
   * at most `--lmax` long (26 units ≈ 11.5 µm). Runs with a VCC diffusion at one end may
     be up to `--lmax-pullup` long (64 units), because depletion loads have long
     channels: 10–55 units on the 6502.

   Each pixel remembers the diffusion pair it bridges. A channel is one connected region
   of one pair, after a 3×3 closing. It must also:
   * share ≥ 14 units of border with both sides,
   * cover ≥ 120 units²,
   * get ≥ 30 % of its pixels from horizontal or vertical runs. Diagonals only complete
     gate corners.

   Each of these rules removed a measured artefact class: field gaps between fingers,
   1-px gate-edge slivers, poly T-junctions, and concave diffusion corners.
2. **Conductors.** Connected components of metal, poly and diffusion, merged by
   union-find:
   * A contact cut joins metal to poly if it sits mostly on poly. Otherwise it joins
     metal to diffusion.
   * A cut also reaches diffusion when it sits on a buried contact.
   * Buried contacts join poly and diffusion.
   * Power seeds join diffusion to VSS/VCC.
3. **Devices.** Gate = the poly component, c1/c2 = the two bridged diffusion nodes. A
   device whose gate equals one terminal and whose other terminal is VCC is a
   **depletion pull-up**: its node is flagged `'+'` and the device is dropped, which is
   the visual6502 convention.
4. **Anchors.** The node under each anchor pixel receives that name.

Output (`write_netlist`):
* `netlist.json`: nodes with names, pull-up flag and layer areas; transistors with bbox,
  width and area.
* `transdefs.js` and `nodenames.js` in visual6502 format.
* Optionally `segdefs.js`, with vectorised polygons per node.

CLI: `extract.py --masks in.npz --out dir [--lmax 30] [--lmax-pullup 64] [--segdefs] [--ymax 10000]`

### Keeping the round trip non-circular (`src/synth.py`)

visual6502 does not store contacts, and its polygons are already grouped by node. We
derive inter-layer connections **once, in vector space, then discard node IDs**:

* **Metal–diffusion and metal–poly cuts.** For every connected overlap of a node's metal
  with *the same node's* diffusion or poly, we emit **one disc** of radius ≤ 6 units
  (≈ a 5 µm cut) at the pole of inaccessibility. This is what a contact detector would
  report: 1719 metal–diffusion and 1532 metal–poly cuts.
* **Buried contacts.** Same-node poly∩diffusion polygons: 1715.
* **Power seeds.** Grounded and powered diffusion polygons stand in for the missing
  power buses, as if a human had traced them. This is information the photo flow has to
  supply too.
* **Pad anchors.** One point inside each named pad's largest metal polygon (34 pads),
  plus vcc/vss from the seeds. This is the "human labels the bond pads" step.
* **`inputdiode` is excluded from the conductor masks.** As a conductor it shorts every
  input pad to vss.

The extractor then sees only unions of layers plus anonymous cuts. Every short or open
caused by the geometry or by rasterisation is visible in the comparison.

## Stage 3: comparison and resolution sweep (`src/compare.py`, `src/roundtrip.py`)

`compare.py` scores a netlist under two node matchings:

* **anchored (the honest one).** It starts only from vcc, vss and the 34 pad names. It
  then seeds nodes whose anchor-seeded Weisfeiler–Lehman colour (≥ 2 hops) is unique and
  identical in both netlists; hubs and anchors keep frozen colours. Finally it propagates
  through the transistor graph LVS-style, breaking ties by WL similarity, with forced
  choices only when nothing else moves.
* **oracle (diagnostic upper bound).** Each extracted node maps to the GT node that owns
  most of its pixels. This separates extraction errors from matching errors.

A GT transistor counts as recovered when an extracted device maps to exactly its
`(gate, {c1,c2})`. Matching is by multiset, so parallel devices count individually.
**Recall** = matched / 3510 GT. **Precision** = matched / extracted.

`roundtrip.py --scales ...` runs rasterise → extract → oracle map → compare for each
scale. `rt_overlay.py` draws extracted devices over the layout:
* green: matched,
* red: extra or mismatched,
* yellow: GT device not recovered.

### Results (`out/roundtrip/sweep.md`)

| px/unit | µm/px | raster | extracted T | anchored recall % | anchored precision % | oracle recall % | oracle precision % | pull-ups (GT 1018) |
|---|---|---|---|---|---|---|---|---|
| 1.0  | 0.444 | 8986×9824 | 3493 | **99.49** | **99.97** | 99.49 | 99.97 | 1017 (all match) |
| 0.5  | 0.888 | 4494×4913 | 3524 | 95.75 | 95.37 | 99.49 | 99.09 | 1018 (1015 match) |
| 0.33 | 1.345 | 2967×3244 | 3636 | 91.79 | 88.61 | 99.20 | 95.76 | 1020 (1013) |
| 0.25 | 1.776 | 2248×2458 | 3854 | 84.16 | 76.65 | 98.18 | 89.41 | 1019 (1003) |
| 0.2  | 2.22  | 1799×1967 | 4270 | 65.19 | 53.58 | 87.26 | 71.73 | 978 (912) |
| 0.15 | 2.96  | 1350×1476 | 5223 | 1.88 | 1.26 | 20.54 | 13.80 | VSS–VCC short |
| 0.1  | 4.44  | 901×985   | 2895 | 0.00 | 0.00 | 0.03 | 0.03 | VSS–VCC short |

Notes:

* **The 18 GT devices missed at 1.0 px/unit** are the 17 gate=vss pad-protection
  transistors, whose only "gate" is the excluded `inputdiode` polygon, plus `t2863`.
  Without the 17 always-off devices, recall at 1.0 is 3492/3493.
* **Precision drops below 1 px/unit because of false channels.** Poly runs over short
  field gaps between two diffusion regions, and at coarse pixels these look exactly like
  gates. Example: a 28-unit field gap under poly against a 15–23-unit gate length. An
  **active-area (thin-oxide) layer** would remove most of this ambiguity, and the
  delayered photo does show it.
* **The gap between anchored and oracle scores at 0.5 comes from regular structures.**
  About 40 nodes in regular arrays (the PLA) are matched to structurally identical
  neighbours after a nearby extra device perturbs the local graph. Anchored and oracle
  agree on 97.9 % of nodes.
* **Resolution needed:** ≤ 0.9 µm/px for ≥ 95 %, ≤ 1.35 µm/px for ≥ 90 %. At about
  3 µm/px the 6502 cannot be extracted at all.
* Runtime: 245 s at 1.0 (dominated by rasterising the oracle labels) and 42 s at 0.5.

Deliverables for the 6502 round trip:
* `out/roundtrip/s1/{netlist.json, transdefs.js, nodenames.js, compare.json}` (best run).
* `out/roundtrip/s0.5/...`, which also includes `segdefs.js` and
  `roundtrip_overlay_s0.5.png`.
* `compare.json` lists, for the oracle mapping, the names of every unrecovered GT
  transistor and every extra extracted transistor.

## Stage 4: photo → layer masks on the 6502 (`src/align6502.py`, `src/segment.py`, `src/seg6502.py`)

### Alignment

**The photos are alignable.** Inputs: visual6502's 4677×5097 surface photo (`top`) and
delayered photo (`sub`), plus their 1027-px vector renders, which share the photo frame.
`align6502.py` does three things:
1. A scale sweep with phase correlation of the GT diffusion against
   `MOS_6502_layers_diffusion.png`.
2. An affine ECC fit.
3. Per-512 px tile residuals from GT-outline vs. photo-outline phase correlation, fitted
   with a 2nd-order polynomial (4 iterations).

Final residual: **median 0.29 px, p90 0.58 px over 299 tiles**, at 1.96 units/px
(≈0.87 µm/px). Overlays confirm a pixel-accurate fit.

### Segmentation

Protocol: one 640×640 training tile in the die centre names the clusters (k-means) or
trains a per-class Gaussian (QDA). Scores cover the rest of the die core.

segdefs has no power metal, so the metal GT is signal metal plus
`MOS_6502_layers_metal_for_ground_and_power.png`, upsampled 4.55×. A ±4 px band around
the upsampled edges is excluded from scoring.

| layer (photo) | method | IoU | precision | recall |
|---|---|---|---|---|
| metal, signal+power (surface) | QDA on Lab+texture | **0.758** | 0.798 | 0.938 |
| metal, signal nets only (power area excluded) | QDA | 0.703 | 0.710 | 0.986 |
| poly, exposed i.e. not under metal (surface) | QDA | 0.185 | 0.598 | 0.211 |
| diffusion, exposed (surface) | QDA | 0.276 | 0.878 | 0.287 |
| metal (surface) | k-means k=10, clusters named on tile | 0.733 | 0.763 | 0.949 |
| diffusion, full (delayered) | edge-bounded regions (Lab-b outline map → regions → rel. lightness) | **0.509** | 0.650 | 0.702 |
| diffusion, full (delayered) | pixel k-means | 0.475 | 0.539 | 0.798 |
| diffusion, full (delayered) | pixel QDA + hole fill | 0.458 | 0.624 | 0.632 |

| contacts (delayered photo) | precision | recall | F1 |
|---|---|---|---|
| DoG dark-blob detector vs `MOS_6502_layers_vias_to_diffusion.png` (7 px tolerance, 3239 cuts) | 0.41 | 0.68 | 0.51 |

The contact GT comes from a 1027-px render, so its positions carry about ±2.3 px of
quantisation. Overlays:
* `out/seg6502/6502_{alu,decode,regs}_seg_vs_gt.png`. Each holds three panels:
  prediction on the surface photo, GT on the surface photo, and diffusion prediction
  (magenta) vs GT (green) on the delayered photo. Detected contacts are red circles; GT
  contacts are blue.
* `out/seg6502/6502_full_segmentation.png`.

What limits segmentation:
* **Metal** over-segments the narrow gaps between lines.
* **Poly** is mostly hidden under metal (61 % of the die), and where exposed its colour
  is close to field.
* **Diffusion outlines leak.** The delayered photo shows diffusion as outlines, not
  filled regions; the outline map is not watertight and one huge background region
  swallows many diffusion regions.
* **Gates are not separated.** Nothing yet splits channels, i.e. the gate stripes,
  from source/drain diffusion in the delayered photo.

**Not done:** running the extractor end to end on these photo masks. With poly at 0.18
and diffusion at 0.5 IoU, the stage-3 sweep implies the result would be meaningless.
Stage 4 needs to reach about 0.95+ IoU before that test is worth running.

## Stage 5: VIC-II 6569 R1 first attempt (`src/vic2.py`)

* **Scale.** The three raw tiles (2048×1536) were located in the stitched JPEG. The
  0.5 mm X step gives 1576 px and the 0.45 mm Y step gives 1416 px, so
  **0.317 µm/px** on both axes. The JPEG is therefore at full raw resolution, and the
  die image is 5.57 × 6.24 mm. In pixel terms this is finer than the 0.44 µm/px at which
  the 6502 round trip is near-perfect. The limits are optical blur (a few px) and
  visibility of the layers, not pixel pitch.
* **Tiles.** `vic2.py tiles` writes `out/vic2/6569R1_overview_tiles.jpg` and
  `tiles_index.json`: 90 tiles of 2048 px. `--write i,j` saves full-resolution tiles to
  `data/retronn/tiles/`.
* **Independent metal reference.** Tommi Lempinen's `MOS_6569R1_L1_metal.png` is a
  1-bit metal trace, rotated 90° relative to the photo. It was registered by rot90,
  scale sweep and affine ECC (corr 0.644). Tile residuals: **median 1.75 px, p90
  3.17 px** (39 tiles, ≈0.55 µm), refined locally per region when the correlation peak
  is reliable.
* **Regions** (`vic2.py region`). Metal QDA is trained on a different tile, (2600, 4200)
  1200×1000, using Tommi's labels:

| region | size | metal IoU strict (P / R) | metal IoU, 2 px boundary band ignored | metal nets: seg / reference | contacts detected | "transistors" |
|---|---|---|---|---|---|---|
| `array_tl`: top-left register/bit-cell array, x 2600–3800, y 1700–2700 | 380×317 µm | 0.827 (0.957 / 0.859) | 0.899 | 77 / 19 | 301 | 2 |
| `logic_br`: bottom-right random logic, x 13900–15300, y 12800–13900 | 444×349 µm | 0.763 (0.898 / 0.836) | 0.845 | 126 / 51 | 266 | 14 |

Overlays (`out/vic2/vic2_<region>_overlays.png`, three panels):
1. Metal: segmentation in magenta, Tommi's reference in cyan.
2. The unsupervised "poly?" (red) and "diffusion?" (yellow) clusters, contacts (green)
   and extracted device boxes.
3. A layer composite.

The raw crop is next to it in `vic2_<region>_photo.png`. Netlists are in
`out/vic2/netlist_<region>/`.

### Assessment of what these photos can deliver

* **Metal: recoverable.**
  * Recall is 0.84–0.86 and precision 0.90–0.96.
  * The main defect is **fragmentation**: dark lines across the metal break wires, so the
    segmentation has 2.5–4× as many metal nets as the reference. These lines are the
    poly/diffusion edges underneath, printed as topography steps on the metal.
  * Wide buses and pads are trivial.
  * Tommi's trace is already a cleaned metal layer and can be used as is.
* **Contacts: detectable but unverified.** The ring-shaped cuts are clearly visible and
  the detector fires on them, but there is no reference to score against. False hits
  occur on the same dark step lines.
* **Poly vs diffusion vs field: not separable by colour.**
  * Where not covered by metal, all three are dark blue-grey with only faint outlines.
    k-means on chroma produces blobs that do not follow any layer.
  * There is no reference to measure against. The site's warning holds: "the deepest
    layer isn't always visible".
* **Hidden under metal.** In dense areas such as `array_tl`, the gaps between metal lines
  are only a few µm wide and almost all poly and diffusion lies under metal.
  * All 301 detected cuts land on nothing (`contacts_floating`), because the layer below
    is not visible.
  * Diffusion under metal is the main ambiguity. Step edges on the metal hint at poly
    and diffusion boundaries but cannot say which layer.
* **Buried contacts: invisible.** Without them the gate–source ties of depletion loads,
  and many poly–diffusion connections, cannot be reconstructed.
* **Netlist.** The extractor run on these masks gives 2 and 14 "transistors", which is
  noise. **No meaningful VIC-II netlist can be extracted from this photo set
  automatically.**

### What it would take to finish the VIC-II

1. **Delayering, as visual6502 did.** Strip metal and image (poly + diffusion), then strip
   poly and image (diffusion + channels + buried contacts). Use the same or better
   optics: ≤ 0.5 µm/px with a 20–50× objective. The stage-3 sweep says ≤ 0.9 µm/px is
   enough once the masks are clean.
2. Alternatively, dark-field or confocal imaging of the existing die. This shows step
   edges but still cannot resolve diffusion under wide metal.
3. **A manual tracing/correction UI** as part of the plan, even with good photos. On the
   6502, classical CV gives only 0.5 diffusion IoU on a delayered photo. Some options:
   * Feed this repo's masks plus overlays into a polygon editor with per-node
     highlighting, like visual6502's python tool.
   * Pre-fill metal from Tommi's trace.
   * Have humans mark power buses, pads, buried contacts and active area.
4. Use the 6502 as a training set for a learned segmenter (U-Net on the aligned
   top/sub photos vs. rasterised segdefs, where pixel-accurate labels now exist). Then
   fine-tune on a small hand-labelled VIC-II set.
5. Add an **active-area** layer to the extractor input. It is visible in delayered photos
   and removes the field-gap channel ambiguity responsible for most false devices at
   coarser pixels.

## Known issues

* **Metal GT for the 6502 is approximate on the power buses.** They are upsampled from a
  1027-px render, and a ±4 px band is excluded.
* **The anchored matcher makes forced choices in symmetric or near-symmetric
  structures:** 21 at 1.0 and 38 at 0.5. It is deterministic but not optimal; local swap
  repair is not implemented.
* **`find_channels` thresholds come from 6502 design rules.** They are applied to the
  VIC-II through a µm conversion (`k_um = 0.444/0.317`) and have not been validated
  there.
* **Diagonal-only channel pixels are discarded** (`hv_frac`). A purely 45° gate would
  still be found, because its H/V runs are √2·L.
* `align6502.json` stores the affine from an ECC fit that was ~13 px off. The
  2nd-order polynomial term absorbs that offset, and residuals are measured after it.
  Always use `affine` + `poly` together.
* The 6502 surface segmentation is trained on one tile and has not been tuned. Results
  vary by a few IoU points with `k` and the tile location.

## Things for the user to decide

* **No need to download the 1.4 GB VIC-II TIFF.** The JPEG is already at raw resolution
  (0.317 µm/px); the TIFF would only remove JPEG artefacts.
  * If lossless data is wanted for a region, the 168 raw PNG tiles (~5 MB each, about
    840 MB in total) can be fetched selectively from
    `ftp://retronn.de/chips/vic2_6569r1_2783/original/`. Each is below the 200 MB rule,
    but the full set is not.
  * Better optics or delayering matter far more than either.
* **Decide how to source poly and diffusion for the VIC-II:**
  1. Obtain or delayer a die and re-image it.
  2. Invest in a manual tracing workflow on the existing photos, where under-metal
     diffusion is a guess.
  3. Look for an existing community trace, such as a VIC-II poly/diffusion trace if one
     exists beyond Tommi's metal layer.

  The automated pipeline cannot close this gap from the current photos.
