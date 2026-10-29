"""Turn the visual6502 ground truth into *blind* extractor inputs ("what a perfect
photo segmentation would give"), so the round trip is not circular.

visual6502 does not store contacts; polygons are pre-grouped by node. We derive
the inter-layer connections ONCE, in vector space, and then throw node IDs away:

  * metal-diffusion and metal-poly contacts: for every connected overlap region of
    a node's metal with the *same node's* diffusion (or poly) we emit ONE contact
    "cut" -- a disc of radius <= R_CONTACT at the pole of inaccessibility of the
    overlap. This is what a contact detector on a photo would output (centre +
    size). It never connects two different GT nodes by construction, but the
    extractor still has to find which metal/poly/diffusion *component* the cut
    lands on in the rasterised union masks, so shorts/opens caused by geometry
    and resolution show up.
  * buried contacts (poly-diffusion): same-node poly/diffusion overlap polygons.
    (Different-node poly/diffusion overlaps are 1-unit drawing slivers at gate
    edges; they are dropped.)
  * power rails: segdefs contains NO vcc/vss metal at all. Node vss is only the
    'grounded diffusion' layer, node vcc only the 'powered
    diffusion' layer. We therefore emit those polygons as 'vss_seed'/'vcc_seed'
    masks -- equivalent to a human tracing the (wide, obvious) power buses and
    their diffusion contacts in a photo.
  * pad anchors: a point inside each named pad's largest metal polygon (what a
    user would click on when labelling bond pads from the pinout).
  * the 17 'inputdiode' polygons (pad protection, node vss in visual6502) abut both
    pad diffusion and ground diffusion; as a conductor they would short every input
    pad to vss, so they are left out of the diffusion mask.

Everything else the extractor sees is the plain union of per-layer polygons:
metal, poly, diffusion (all four diffusion layers merged), with no node IDs.

CLI:
    python -I src/synth.py --out out/roundtrip/synth_vectors.json
    python -I src/synth.py --rasterize out/roundtrip/synth_vectors.json --scale 0.5 --out out/roundtrip/in_s0.5.npz
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import defaultdict

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import gt6502  # noqa: E402

R_CONTACT = 6.0  # chip units (~2.7 um radius, ~5 um cut) -- typical 6502 contact size
PAD_NAMES = (["res", "rw", "sync", "so", "clk0", "clk1out", "clk2out", "rdy", "nmi", "irq"]
             + [f"db{i}" for i in range(8)] + [f"ab{i}" for i in range(16)])


def _poly(pts):
    from shapely.geometry import Polygon
    from shapely.validation import make_valid
    p = Polygon(pts)
    if not p.is_valid:
        p = make_valid(p)
    return p


def _parts(geom):
    from shapely.geometry import Polygon, MultiPolygon, GeometryCollection
    if geom.is_empty:
        return []
    if isinstance(geom, Polygon):
        return [geom]
    if isinstance(geom, (MultiPolygon, GeometryCollection)):
        out = []
        for g in geom.geoms:
            out.extend(_parts(g))
        return out
    return []


def derive(chip: gt6502.Chip):
    from shapely.ops import unary_union
    from shapely.ops import polylabel
    by = defaultdict(lambda: defaultdict(list))
    for s in chip.segs:
        lay = "metal" if s.layer == gt6502.METAL else "poly" if s.layer == gt6502.POLY else "diff"
        by[s.node][lay].append(_poly(s.pts))
    contacts, buried = [], []
    stats = defaultdict(int)
    for node, d in by.items():
        U = {k: unary_union(v) for k, v in d.items()}
        for a, b, kind in (("metal", "diff", "md"), ("metal", "poly", "mp")):
            if a in U and b in U:
                for part in _parts(U[a].intersection(U[b])):
                    if part.area < 4.0:
                        stats[kind + "_tiny"] += 1
                        continue
                    pt = polylabel(part, tolerance=0.5)
                    r = min(R_CONTACT, part.exterior.distance(pt))
                    contacts.append({"x": pt.x, "y": pt.y, "r": max(r, 1.0), "kind": kind})
                    stats[kind] += 1
        if "poly" in U and "diff" in U:
            for part in _parts(U["poly"].intersection(U["diff"])):
                if part.area < 4.0:
                    stats["pd_tiny"] += 1
                    continue
                buried.append([list(c) for c in part.exterior.coords])
                stats["buried"] += 1
    anchors = {}
    for name in PAD_NAMES:
        n = chip.names[name]
        mp = [p for p in by[n].get("metal", [])]
        if not mp:
            continue
        big = max(mp, key=lambda p: p.area)
        pt = polylabel(big if big.geom_type == "Polygon" else max(_parts(big), key=lambda p: p.area), 0.5)
        anchors[name] = {"x": pt.x, "y": pt.y, "layer": "metal"}
    return {"contacts": contacts, "buried": buried, "anchors": anchors, "stats": dict(stats),
            "r_contact": R_CONTACT}


def rasterize_inputs(chip: gt6502.Chip, vec: dict, scale: float):
    """Build the blind extractor input masks at `scale` px/unit."""
    import cv2
    m = gt6502.rasterize_layers(chip, scale)
    shape = m["metal"].shape
    SH = 4
    contacts = np.zeros(shape, np.uint8)
    kinds = {"md": 1, "mp": 2}
    for c in vec["contacts"]:
        cx, cy = c["x"] * scale, (gt6502.CHIP_SIZE - c["y"]) * scale
        r = max(c["r"] * scale, 0.5)
        cv2.circle(contacts, (int(round((cx - 0.5) * (1 << SH))), int(round((cy - 0.5) * (1 << SH)))),
                   int(round(r * (1 << SH))), 1, -1, lineType=cv2.LINE_8, shift=SH)
        # guarantee the centre pixel is marked even when r < 1 px
        rr, cc = int(np.clip(np.floor(cy), 0, shape[0] - 1)), int(np.clip(np.floor(cx), 0, shape[1] - 1))
        contacts[rr, cc] = 1
    buried = np.zeros(shape, np.uint8)
    for poly in vec["buried"]:
        p = gt6502.to_px(np.asarray(poly, float), scale)
        pi = np.round((p - 0.5) * (1 << SH)).astype(np.int32).reshape(-1, 1, 2)
        cv2.fillPoly(buried, [pi], 1, lineType=cv2.LINE_8, shift=SH)
    # 'inputdiode' (17 polygons, node vss in visual6502) is the pad input-protection
    # structure. Its polygons abut both the pad diffusion and grounded diffusion, so as a
    # conductor it would short every input pad to vss. It is a junction, not a wire:
    # leave it out of the conductor masks (a human marks pad protection in a photo flow).
    diff = (m["switched_diffusion"] | m["grounded_diffusion"] | m["powered_diffusion"])
    anchors = {}
    for k, a in vec["anchors"].items():
        anchors[k] = {"row": int((gt6502.CHIP_SIZE - a["y"]) * scale), "col": int(a["x"] * scale),
                      "layer": a["layer"]}
    return {
        "metal": m["metal"], "poly": m["polysilicon"], "diff": diff,
        "contacts": contacts.astype(bool), "buried": buried.astype(bool),
        "vss_seed": m["grounded_diffusion"], "vcc_seed": m["powered_diffusion"],
    }, {"scale": scale, "units_per_px": 1.0 / scale, "anchors": anchors,
        "origin": "visual6502 chip units, row = (10000 - y) * scale, col = x * scale"}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rasterize", help="vector json from a previous run; write masks npz instead")
    ap.add_argument("--scale", type=float, default=0.5)
    a = ap.parse_args(argv)
    chip = gt6502.load()
    if a.rasterize:
        with open(a.rasterize) as f:
            vec = json.load(f)
        masks, meta = rasterize_inputs(chip, vec, a.scale)
        np.savez_compressed(a.out, **masks)
        with open(os.path.splitext(a.out)[0] + ".json", "w") as f:
            json.dump(meta, f, indent=1)
        print("wrote", a.out, masks["metal"].shape)
        return
    vec = derive(chip)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w") as f:
        json.dump(vec, f)
    print(json.dumps(vec["stats"]), "anchors", len(vec["anchors"]))


if __name__ == "__main__":
    sys.exit(main())
