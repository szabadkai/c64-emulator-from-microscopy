"""Round-trip overlay: GT layout at a given scale with extracted transistors coloured by the
anchored comparison (green = matches a GT device, red = extra/mismatched) and GT devices that
were not recovered (yellow). Purely a visual aid for out/roundtrip/s<scale>/.

CLI: python -I src/rt_overlay.py --scale 0.5 --x 2600 --y 2400 --w 1800 --h 1300
     (x/y/w/h in chip units, y measured downward from the top as in the photos)
"""
from __future__ import annotations

import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import gt6502  # noqa: E402
import compare  # noqa: E402
import render  # noqa: E402


def main(argv=None):
    import cv2
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scale", type=float, default=0.5)
    ap.add_argument("--x", type=int, default=2600)
    ap.add_argument("--y", type=int, default=2400)
    ap.add_argument("--w", type=int, default=1800)
    ap.add_argument("--h", type=int, default=1300)
    ap.add_argument("--zoom", type=int, default=2)
    a = ap.parse_args(argv)
    d = os.path.join(os.path.dirname(HERE), "out", "roundtrip", f"s{a.scale:g}")
    chip = gt6502.load()
    gt, _ = compare.load_gt()
    ext, raw = compare.load_ext(os.path.join(d, "netlist.json"))
    with open(os.path.join(d, "anchored_map.json")) as f:
        M = {int(k): v for k, v in json.load(f).items()}
    miss_gt, miss_ext = compare.unmatched_lists(ext, gt, M)
    miss_ext = set(miss_ext)
    s = a.scale
    m = gt6502.combined(gt6502.rasterize_layers(chip, s))
    r0, c0 = int(a.y * s), int(a.x * s)
    h, w = int(a.h * s), int(a.w * s)
    sl = (slice(r0, r0 + h), slice(c0, c0 + w))
    img = render.compose({k: v[sl] for k, v in m.items()})
    z = a.zoom
    img = cv2.resize(img, (w * z, h * z), interpolation=cv2.INTER_NEAREST)
    for i, t in enumerate(raw["transistors"]):
        x0, x1, rr0, rr1 = t["bb"]
        p0 = (int((x0 * s - c0) * z), int((rr0 * s - r0) * z))
        p1 = (int((x1 * s - c0) * z), int((rr1 * s - r0) * z))
        if -50 < p0[0] < w * z and -50 < p0[1] < h * z:
            cv2.rectangle(img, p0, p1, (255, 0, 0) if i in miss_ext else (0, 255, 0), 2)
    for j in miss_gt:
        x0, x1, y0, y1 = chip.trans[j].bb
        p0 = (int((x0 * s - c0) * z), int(((gt6502.CHIP_SIZE - y1) * s - r0) * z))
        p1 = (int((x1 * s - c0) * z), int(((gt6502.CHIP_SIZE - y0) * s - r0) * z))
        if -50 < p0[0] < w * z and -50 < p0[1] < h * z:
            cv2.rectangle(img, p0, p1, (255, 255, 0), 2)
    out = os.path.join(d, f"roundtrip_overlay_s{a.scale:g}.png")
    render.save_rgb(img, out, max_w=4000)
    print("wrote", out, "extra/mismatched ext:", len(miss_ext), "unrecovered GT:", len(miss_gt))


if __name__ == "__main__":
    sys.exit(main())
