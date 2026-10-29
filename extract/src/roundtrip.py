"""6502 round trip + resolution sweep: GT polygons -> blind masks -> extracted netlist -> comparison.

For each scale (pixels per visual6502 chip unit; 1 unit ~= 0.444 um, from the
3.9 x 4.3 mm die vs. the 8769 x 9628 unit polygon extent):
  1. rasterise the blind extractor inputs (synth.rasterize_inputs)
  2. run extract.extract (no node ids)
  3. build the oracle map (ext node -> GT node owning most of its pixels)
  4. compare.compare  (anchored matching + oracle), write reports

CLI:
    python -I src/roundtrip.py --scales 1.0 0.5 0.33 0.25 0.2 0.15 0.1 --out out/roundtrip
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import gt6502  # noqa: E402
import synth  # noqa: E402
import extract  # noqa: E402
import compare  # noqa: E402

UM_PER_UNIT = 0.444


def oracle_map(chip, aux, scale, shape):
    _, lab = gt6502.rasterize_layers(chip, scale, shape=shape, label_nodes=True)
    gdiff = np.maximum.reduce([lab["switched_diffusion"], lab["inputdiode"], lab["grounded_diffusion"],
                               lab["powered_diffusion"]])
    gt_by = {"metal": lab["metal"], "poly": lab["polysilicon"], "diff": gdiff}
    votes = {}
    for layer in ("metal", "poly", "diff"):
        er = extract.node_raster(aux, layer)
        gr = gt_by[layer]
        sel = (er > 0) & (gr > 0)
        e = er[sel].astype(np.int64) - 1
        g = gr[sel].astype(np.int64) - 1
        key = e * 4096 + g
        u, c = np.unique(key, return_counts=True)
        for k, n in zip(u, c):
            ee, gg = divmod(int(k), 4096)
            votes.setdefault(ee, {}).setdefault(gg, 0)
            votes[ee][gg] += int(n)
    return {e: max(v.items(), key=lambda kv: kv[1])[0] for e, v in votes.items()}


def run_scale(chip, vec, scale, outdir, lmax=26.0, segdefs=False, log=print):
    t0 = time.time()
    d = os.path.join(outdir, f"s{scale:g}")
    os.makedirs(d, exist_ok=True)
    masks, meta = synth.rasterize_inputs(chip, vec, scale)
    res, aux = extract.extract(masks, meta, lmax_units=lmax, log=log)
    extract.write_netlist(res, d, aux, segdefs=segdefs, ymax_units=gt6502.CHIP_SIZE)
    om = oracle_map(chip, aux, scale, masks["metal"].shape)
    with open(os.path.join(d, "oracle_map.json"), "w") as f:
        json.dump({str(k): v for k, v in om.items()}, f)
    rep, M = compare.compare(os.path.join(d, "netlist.json"), os.path.join(d, "oracle_map.json"), log=log)
    rep["scale_px_per_unit"] = scale
    rep["um_per_px"] = round(UM_PER_UNIT / scale, 3)
    rep["image_px"] = list(masks["metal"].shape)
    rep["extract_meta"] = res["meta"]
    rep["seconds"] = round(time.time() - t0, 1)
    with open(os.path.join(d, "compare.json"), "w") as f:
        json.dump(rep, f, indent=1)
    with open(os.path.join(d, "anchored_map.json"), "w") as f:
        json.dump({str(k): v for k, v in M.items()}, f)
    return rep, (masks, res, aux, om, M)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scales", type=float, nargs="+", default=[1.0, 0.5, 0.33, 0.25, 0.2, 0.15, 0.1])
    ap.add_argument("--out", default=os.path.join(os.path.dirname(HERE), "out", "roundtrip"))
    ap.add_argument("--lmax", type=float, default=26.0)
    ap.add_argument("--segdefs-at", type=float, default=None, help="also write segdefs.js at this scale")
    a = ap.parse_args(argv)
    chip = gt6502.load()
    os.makedirs(a.out, exist_ok=True)
    vpath = os.path.join(a.out, "synth_vectors.json")
    if not os.path.exists(vpath):
        vec = synth.derive(chip)
        with open(vpath, "w") as f:
            json.dump(vec, f)
    with open(vpath) as f:
        vec = json.load(f)
    rows = []
    for s in a.scales:
        print(f"== scale {s} px/unit ({UM_PER_UNIT / s:.2f} um/px)")
        rep, _ = run_scale(chip, vec, s, a.out, lmax=a.lmax, segdefs=(a.segdefs_at == s))
        an, orc = rep["anchored"], rep.get("oracle", {})
        row = {"scale": s, "um_per_px": rep["um_per_px"], "image": "x".join(map(str, rep["image_px"][::-1])),
               "ext_T": an["ext_transistors"], "gt_T": an["gt_transistors"],
               "anchored_recall": an["recall_pct"], "anchored_precision": an["precision_pct"],
               "oracle_recall": orc.get("recall_pct"), "oracle_precision": orc.get("precision_pct"),
               "ext_pullups": an["ext_pullups"], "pullups_matched_oracle": orc.get("pullups_matched"),
               "forced": an["forced_choices"], "agree_oracle": an.get("node_agreement_with_oracle_pct"),
               "vss_vcc_short": rep["extract_meta"]["vss_vcc_short"]}
        rows.append(row)
        print(json.dumps(row))
    with open(os.path.join(a.out, "sweep.json"), "w") as f:
        json.dump(rows, f, indent=1)
    # markdown table
    hdr = ("| px/unit | um/px | raster | ext T | anchored recall % | anchored precision % | oracle recall % "
           "| oracle precision % | ext pull-ups (GT 1018) | pull-ups matched (oracle) |")
    lines = [hdr, "|" + "---|" * 10]
    for r in rows:
        lines.append(f"| {r['scale']} | {r['um_per_px']} | {r['image']} | {r['ext_T']} | {r['anchored_recall']} | "
                     f"{r['anchored_precision']} | {r['oracle_recall']} | {r['oracle_precision']} | "
                     f"{r['ext_pullups']} | {r['pullups_matched_oracle']} |")
    with open(os.path.join(a.out, "sweep.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    sys.exit(main())
