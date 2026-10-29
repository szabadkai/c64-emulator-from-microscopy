"""Stage 1: visual6502 ground-truth loader and rasterizer.

Parses segdefs.js / transdefs.js / nodenames.js *as text data* (never executed)
and rasterizes the polygon layers to binary masks at a chosen scale.

Layer indices (from expertWires.js `layernames` in the visual6502 repo):
    0 metal, 1 switched diffusion, 2 inputdiode, 3 grounded diffusion,
    4 powered diffusion, 5 polysilicon

Coordinates: segdefs/transdefs use "chip units"; visual6502 renders with
y flipped (y_screen = grChipSize - y, grChipSize = 10000). We keep the raw
chip coordinates and flip only when rasterizing (row 0 = top of the die as
displayed by visual6502), so rasters line up with the published photos.

CLI:
    python -I src/gt6502.py summary
    python -I src/gt6502.py raster --scale 0.25 --out out/gt_masks_s025.npz
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)
REPO = os.path.join(ROOT, "data", "visual6502", "repo")

LAYER_NAMES = ["metal", "switched_diffusion", "inputdiode", "grounded_diffusion",
               "powered_diffusion", "polysilicon"]
METAL, SWDIFF, INDIODE, GNDDIFF, PWRDIFF, POLY = range(6)
DIFF_LAYERS = (SWDIFF, INDIODE, GNDDIFF, PWRDIFF)
CHIP_SIZE = 10000  # grChipSize in expertWires.js


@dataclass
class Seg:
    node: int
    pullup: bool
    layer: int
    pts: np.ndarray  # (N,2) float, chip coords (x, y) y-up


@dataclass
class Trans:
    name: str
    gate: int
    c1: int
    c2: int
    bb: tuple  # xmin, xmax, ymin, ymax (chip coords, y-up)
    extra: tuple


@dataclass
class Chip:
    segs: list
    trans: list
    names: dict  # name -> node
    node_names: dict = field(default_factory=dict)  # node -> [names]

    @property
    def vss(self):
        return self.names["vss"]

    @property
    def vcc(self):
        return self.names["vcc"]

    def pullups(self):
        return {s.node for s in self.segs if s.pullup}

    def bbox(self):
        allp = np.concatenate([s.pts for s in self.segs])
        return allp.min(0), allp.max(0)


_SEG_RE = re.compile(r"^\[\s*(\d+)\s*,\s*'([+-])'\s*,\s*(\d+)\s*,([\d,\s.\-]+)\]", re.M)
_TR_RE = re.compile(
    r"^\['(t\d+)',\s*(\d+),\s*(\d+),\s*(\d+),\s*\[([\d,\s\-]+)\],\s*\[([\d,\s\-]+)\]\s*\]", re.M)
_NN_RE = re.compile(r"^\s*\"?([~#A-Za-z0-9_.]+)\"?\s*:\s*(\d+)\s*,", re.M)


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def load(repo: str = REPO) -> Chip:
    seg_txt = _read(os.path.join(repo, "segdefs.js"))
    segs = []
    for m in _SEG_RE.finditer(seg_txt):
        nums = [float(v) for v in m.group(4).replace("\n", "").split(",") if v.strip()]
        if len(nums) < 6 or len(nums) % 2:
            continue
        segs.append(Seg(int(m.group(1)), m.group(2) == "+", int(m.group(3)),
                        np.array(nums, dtype=np.float64).reshape(-1, 2)))
    tr_txt = _read(os.path.join(repo, "transdefs.js"))
    trans = []
    for m in _TR_RE.finditer(tr_txt):
        bb = tuple(int(v) for v in m.group(5).split(","))
        ex = tuple(int(v) for v in m.group(6).split(","))
        trans.append(Trans(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), bb, ex))
    nn_txt = _read(os.path.join(repo, "nodenames.js"))
    body = nn_txt[nn_txt.index("var nodenames"):]
    names = {}
    for m in _NN_RE.finditer(body):
        names[m.group(1)] = int(m.group(2))
    node_names = {}
    for k, v in names.items():
        node_names.setdefault(v, []).append(k)
    return Chip(segs, trans, names, node_names)


# ---------------------------------------------------------------- raster

def to_px(pts, scale, ymax=CHIP_SIZE, x0=0.0, y0=0.0):
    """chip coords (y-up) -> pixel coords (col,row) at given scale (px per unit)."""
    out = np.empty_like(pts)
    out[:, 0] = (pts[:, 0] - x0) * scale
    out[:, 1] = (ymax - pts[:, 1] - y0) * scale
    return out


def raster_shape(chip: Chip, scale: float):
    lo, hi = chip.bbox()
    w = int(np.ceil((hi[0] + 1) * scale)) + 2
    h = int(np.ceil((CHIP_SIZE - lo[1] + 1) * scale)) + 2
    return h, w


def rasterize_layers(chip: Chip, scale: float, shape=None, label_nodes=False, layers=None):
    """Return dict layer_name -> bool mask (and optionally node-label int32 rasters).

    Rasterization uses cv2.fillPoly with 4-bit subpixel precision; polygons are
    filled with pixel-centre sampling (a pixel belongs to a polygon if its centre
    is inside), approximately.
    """
    import cv2
    if shape is None:
        shape = raster_shape(chip, scale)
    h, w = shape
    SH = 4
    masks = {n: np.zeros((h, w), np.uint8) for n in LAYER_NAMES}
    labels = {n: np.zeros((h, w), np.int32) for n in LAYER_NAMES} if label_nodes else None
    for s in chip.segs:
        if layers is not None and s.layer not in layers:
            continue
        p = to_px(s.pts, scale)
        # cv2 treats integer coords as pixel centres; shift by -0.5 so that
        # continuous coords map to pixel-centre sampling.
        pi = np.round((p - 0.5) * (1 << SH)).astype(np.int32).reshape(-1, 1, 2)
        ln = LAYER_NAMES[s.layer]
        cv2.fillPoly(masks[ln], [pi], 1, lineType=cv2.LINE_8, shift=SH)
        if label_nodes:
            cv2.fillPoly(labels[ln], [pi], int(s.node) + 1, lineType=cv2.LINE_8, shift=SH)
    masks = {k: v.astype(bool) for k, v in masks.items()}
    return (masks, labels) if label_nodes else masks


def combined(masks):
    """Collapse visual6502 layers to physical layers: metal, poly, diffusion."""
    diff = masks["switched_diffusion"] | masks["inputdiode"] | masks["grounded_diffusion"] | masks["powered_diffusion"]
    return {"metal": masks["metal"], "poly": masks["polysilicon"], "diff": diff}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("summary")
    r = sub.add_parser("raster")
    r.add_argument("--scale", type=float, default=0.25, help="pixels per chip unit")
    r.add_argument("--out", required=True)
    r.add_argument("--png", help="optional colour PNG preview")
    a = ap.parse_args(argv)
    chip = load()
    if a.cmd == "summary":
        lo, hi = chip.bbox()
        from collections import Counter
        lc = Counter(LAYER_NAMES[s.layer] for s in chip.segs)
        nodes = {s.node for s in chip.segs}
        print(json.dumps({
            "segments": len(chip.segs), "per_layer": lc, "nodes_with_geometry": len(nodes),
            "transistors": len(chip.trans), "named_entries": len(chip.names),
            "pullup_nodes": len(chip.pullups()), "bbox_lo": lo.tolist(), "bbox_hi": hi.tolist(),
            "vcc": chip.vcc, "vss": chip.vss}, indent=1, default=int))
    elif a.cmd == "raster":
        masks = rasterize_layers(chip, a.scale)
        np.savez_compressed(a.out, **masks)
        if a.png:
            from render import layer_png
            layer_png(combined(masks), a.png)
        print("wrote", a.out, next(iter(masks.values())).shape)


if __name__ == "__main__":
    sys.exit(main())
