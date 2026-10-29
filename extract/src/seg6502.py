"""Stage 4 on the 6502: segment the visual6502 photos and score against aligned GT.

Inputs : 6502_top_op10x_BF_4677.png (surface: metal + poly + exposed diffusion)
         6502_sub_op10x_BF_4677.png (delayered substrate: diffusion outlines, contact cuts)
         out/align6502.json (chip units -> photo px, from align6502.py)
GT     : segdefs rasterised into the photo frame. segdefs has NO vcc/vss metal, so the
         metal GT is completed with MOS_6502_layers_metal_for_ground_and_power.png
         (visual6502's 1027-px render of the power buses, upsampled 4.55x); a +-4 px
         band around those upsampled edges is excluded from scoring.
Protocol: one 640x640 training tile (centre of the die) names the k-means clusters;
         all scores are on the rest of the die core (pads/scribe excluded).

Classes on the surface photo: field / exposed diffusion / exposed poly / metal.
Substrate photo: diffusion (edge-bounded regions classified by region statistics),
contact cuts (DoG blob detector vs. visual6502's 'vias to diffusion' layer).

CLI: python -I src/seg6502.py --out out/seg6502
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
import align6502  # noqa: E402
import segment  # noqa: E402
import render  # noqa: E402

IMG = align6502.IMG
FIELD, DIFF, POLY, METAL = range(4)
CLASS_NAMES = ["field", "diffusion(exposed)", "poly(exposed)", "metal"]


def gt_masks(chip):
    import cv2
    from PIL import Image
    A, poly = align6502.load_alignment()
    D = align6502.gt_raster_photo(chip, A, (1, 3, 4), poly=poly)
    P = align6502.gt_raster_photo(chip, A, (5,), poly=poly)
    M = align6502.gt_raster_photo(chip, A, (0,), poly=poly)
    pw = np.asarray(Image.open(os.path.join(IMG, "MOS_6502_layers_metal_for_ground_and_power.png")).convert("L")) > 0
    PW = cv2.resize(pw.astype(np.uint8), (align6502.PHOTO_W, align6502.PHOTO_H),
                    interpolation=cv2.INTER_NEAREST).astype(bool)
    band = cv2.morphologyEx(PW.astype(np.uint8), cv2.MORPH_GRADIENT, np.ones((9, 9), np.uint8)).astype(bool)
    return {"diff": D, "poly": P, "metal_signal": M, "metal_power": PW & ~M, "power_band": band & ~M}


def contacts_gt():
    """Metal->diffusion contact centres from visual6502's 'vias to diffusion' render (1027 px).
    Layer-image px -> chip raster (inverse of the coarse fit) -> photo px (affine + poly)."""
    import cv2
    from PIL import Image
    with open(os.path.join(os.path.dirname(HERE), "out", "align6502.json")) as f:
        al = json.load(f)
    A0 = np.array(al["coarse"])
    A, poly = np.array(al["affine"]), al["poly"]
    v = np.asarray(Image.open(os.path.join(IMG, "MOS_6502_layers_vias_to_diffusion.png")).convert("L")) > 0
    n, lab, st, cen = cv2.connectedComponentsWithStats(v.astype(np.uint8), 8)
    k = align6502.PHOTO_W / v.shape[1]
    c = cen[1:] + 0.5  # pixel-centre convention
    chip_raster = (c * k - A0[:, 2]) / A0[0, 0]  # (x, 10000-y)
    pts = np.stack([chip_raster[:, 0], gt6502.CHIP_SIZE - chip_raster[:, 1]], 1)
    return align6502.chip_to_photo(pts, A, poly), st[1:, cv2.CC_STAT_AREA]


def context_features(rgb):
    """Larger-scale context: smoothed Lab + texture at sigma 3 (helps thin poly vs field)."""
    import cv2
    lab = segment.to_lab(rgb, 3.0)
    gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY)
    return [lab[..., 1], lab[..., 2], segment.texture(gray, 2.0, 4.0) * 2.0]


def fill_holes(mask, max_hole=400):
    import cv2
    inv = (~mask).astype(np.uint8)
    n, lab, st, _ = cv2.connectedComponentsWithStats(inv, 4)
    small = st[:, cv2.CC_STAT_AREA] <= max_hole
    small[0] = False
    return mask | small[lab]


def main(argv=None):
    import cv2
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(HERE), "out", "seg6502"))
    ap.add_argument("--k", type=int, default=10)
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    chip = gt6502.load()
    G = gt_masks(chip)
    top = align6502.load_photo("6502_top_op10x_BF_4677.png")
    sub = align6502.load_photo("6502_sub_op10x_BF_4677.png")
    H, W = top.shape[:2]
    core = np.zeros((H, W), bool)
    core[450:4650, 450:4250] = True  # inside the pad ring
    train = np.zeros((H, W), bool)
    ty, tx = 2400, 2000
    train[ty:ty + 640, tx:tx + 640] = True
    evalm = core & ~train
    report = {"train_tile": [tx, ty, 640, 640], "eval_region": "die core [450:4650, 450:4250] minus train tile"}

    # ---------------- surface photo: 4-class pixel clustering
    metal_gt = G["metal_signal"] | G["metal_power"]
    cls = np.full((H, W), FIELD, np.int32)
    cls[G["diff"]] = DIFF
    cls[G["poly"]] = POLY
    cls[metal_gt] = METAL
    ignore = G["power_band"]
    feat = segment.features(top, extra=context_features(top))
    model = segment.kmeans_fit(feat, k=a.k, mask=core)
    lab = segment.kmeans_predict(feat, model)
    c2c, votes = segment.name_clusters(lab, cls, train, 4, ignore)
    pred = c2c[lab]
    metal_p = segment.cleanup(pred == METAL, open_px=1, close_px=2, min_area=30)
    poly_p = segment.cleanup((pred == POLY) & ~metal_p, open_px=1, close_px=1, min_area=20)
    diffx_p = segment.cleanup((pred == DIFF) & ~metal_p & ~poly_p, open_px=1, close_px=1, min_area=20)
    valid = evalm & ~ignore
    surf = {
        "metal (signal+power)": segment.iou(metal_p, metal_gt, valid),
        "metal, signal nets only (power-bus area excluded)": segment.iou(metal_p, G["metal_signal"],
                                                                        valid & ~G["metal_power"]),
        "poly, exposed (not under metal)": segment.iou(poly_p, G["poly"] & ~metal_gt, valid & ~metal_gt),
        "diffusion, exposed (not under metal/poly)": segment.iou(diffx_p, G["diff"] & ~metal_gt & ~G["poly"],
                                                                valid & ~metal_gt),
    }
    report["surface_kmeans"] = {"k": a.k, "cluster_to_class": [CLASS_NAMES[c] for c in c2c], "scores": surf}
    print(json.dumps(report["surface_kmeans"], indent=1))
    # supervised alternative: per-class Gaussian (QDA) trained on the same single tile
    qm = segment.qda_fit(feat, cls, train, 4, ignore)
    pq = segment.qda_predict(feat, qm)
    metal_q = segment.cleanup(pq == METAL, open_px=1, close_px=2, min_area=30)
    poly_q = segment.cleanup((pq == POLY) & ~metal_q, open_px=1, close_px=1, min_area=20)
    diffx_q = segment.cleanup((pq == DIFF) & ~metal_q & ~poly_q, open_px=1, close_px=1, min_area=20)
    surf_q = {
        "metal (signal+power)": segment.iou(metal_q, metal_gt, valid),
        "metal, signal nets only (power-bus area excluded)": segment.iou(metal_q, G["metal_signal"],
                                                                        valid & ~G["metal_power"]),
        "poly, exposed (not under metal)": segment.iou(poly_q, G["poly"] & ~metal_gt, valid & ~metal_gt),
        "diffusion, exposed (not under metal/poly)": segment.iou(diffx_q, G["diff"] & ~metal_gt & ~G["poly"],
                                                                valid & ~metal_gt),
    }
    report["surface_qda"] = {"scores": surf_q}
    print(json.dumps(report["surface_qda"], indent=1))
    # keep the better (by mean IoU) for overlays / downstream
    mean = lambda d: np.mean([v["iou"] for v in d.values()])
    if mean(surf_q) > mean(surf):
        metal_p, poly_p, diffx_p = metal_q, poly_q, diffx_q
        report["surface_used"] = "qda"
    else:
        report["surface_used"] = "kmeans"

    # ---------------- substrate photo: diffusion via edge-bounded regions
    rl, edges, rel, rarea = segment.diffusion_regions(sub, core=core)
    gtD = G["diff"]
    best = None
    for t in np.arange(-40, 0, 1.0):  # region threshold named on the training tile
        r = segment.iou(segment.regions_to_mask(rl, edges, rel, rarea, t), gtD, train)["iou"]
        if best is None or r > best[0]:
            best = (r, t)
    diff_full = segment.cleanup(segment.regions_to_mask(rl, edges, rel, rarea, best[1]),
                                open_px=1, close_px=1, min_area=40)
    name = [float(best[1])]
    # pixel k-means alternative on the substrate photo
    feat_s = segment.features(sub, extra=context_features(sub))
    model_s = segment.kmeans_fit(feat_s, k=a.k, mask=core)
    lab_s = segment.kmeans_predict(feat_s, model_s)
    c2c_s, _ = segment.name_clusters(lab_s, gtD.astype(np.int32), train, 2)
    diff_px = segment.cleanup(c2c_s[lab_s] == 1, open_px=1, close_px=2, min_area=40)
    qs = segment.qda_fit(feat_s, gtD.astype(np.int32), train, 2)
    diff_q = segment.cleanup(segment.qda_predict(feat_s, qs) == 1, open_px=1, close_px=2, min_area=40)
    diff_q = fill_holes(diff_q, max_hole=400)
    report["substrate_diffusion"] = {
        "edge-bounded regions": segment.iou(diff_full, gtD, evalm),
        "pixel k-means": segment.iou(diff_px, gtD, evalm),
        "pixel QDA (+hole fill)": segment.iou(diff_q, gtD, evalm),
        "region_threshold_rel_L": name,
    }
    sc = {k: v["iou"] for k, v in report["substrate_diffusion"].items() if isinstance(v, dict)}
    diff_full = {"edge-bounded regions": diff_full, "pixel k-means": diff_px,
                 "pixel QDA (+hole fill)": diff_q}[max(sc, key=sc.get)]
    report["substrate_used"] = max(sc, key=sc.get)
    print(json.dumps(report["substrate_diffusion"], indent=1))

    # ---------------- contacts (substrate photo)
    gray = cv2.cvtColor(sub, cv2.COLOR_RGB2GRAY)
    cg, carea = contacts_gt()
    inside = lambda p: core[np.clip(p[:, 1].astype(int), 0, H - 1), np.clip(p[:, 0].astype(int), 0, W - 1)] & \
        ~train[np.clip(p[:, 1].astype(int), 0, H - 1), np.clip(p[:, 0].astype(int), 0, W - 1)]
    cg_e = cg[inside(cg)]
    # threshold from the training tile: maximise F1 there
    best = None
    for pct in (97.0, 98.0, 98.5, 99.0, 99.3, 99.6):
        pts, dog, th = segment.detect_contacts(gray, sigma=2.2, mask=core)
        th = float(np.percentile(dog[core], pct))
        pts = pts[dog[pts[:, 1], pts[:, 0]] > th]
        tin = train[pts[:, 1], pts[:, 0]]
        ctrain = cg[train[np.clip(cg[:, 1].astype(int), 0, H - 1), np.clip(cg[:, 0].astype(int), 0, W - 1)]]
        f = segment.match_points(pts[tin].astype(float), ctrain, 7.0)["f1"]
        if best is None or f > best[0]:
            best = (f, pct, th, pts)
    _, pct, th, pts = best
    pe = pts[inside(pts.astype(float))].astype(float)
    report["contacts"] = {"threshold_pct": pct, "tolerance_px": 7.0,
                          "vs_visual6502_vias_to_diffusion": segment.match_points(pe, cg_e, 7.0),
                          "note": "GT centres come from a 1027-px render (+-2.3 px quantisation)"}
    print(json.dumps(report["contacts"], indent=1))

    with open(os.path.join(a.out, "seg6502_report.json"), "w") as f:
        json.dump(report, f, indent=1)
    np.savez_compressed(os.path.join(a.out, "seg6502_masks.npz"), metal=metal_p, poly_exposed=poly_p,
                        diff_exposed=diffx_p, diff_substrate=diff_full, contacts=pts)

    # ---------------- overlays (crops <= 4000 px wide)
    def crop(img, y, x, h, w):
        return img[y:y + h, x:x + w]
    for tag, (y, x) in {"alu": (2700, 3000), "decode": (700, 1400), "regs": (3500, 700)}.items():
        h, w = 700, 900
        o1 = render.overlay_outlines(crop(top, y, x, h, w), {"metal": crop(metal_p, y, x, h, w),
                                                              "poly": crop(poly_p, y, x, h, w),
                                                              "diff": crop(diffx_p, y, x, h, w)},
                                     {"metal": (0, 80, 255), "poly": (255, 0, 0), "diff": (0, 200, 0)})
        o2 = render.overlay_outlines(crop(top, y, x, h, w), {"metal": crop(metal_gt, y, x, h, w),
                                                              "poly": crop(G["poly"], y, x, h, w)},
                                     {"metal": (0, 80, 255), "poly": (255, 0, 0)})
        o3 = render.overlay_outlines(crop(sub, y, x, h, w), {"pred": crop(diff_full, y, x, h, w),
                                                              "gt": crop(gtD, y, x, h, w)},
                                     {"pred": (255, 0, 255), "gt": (0, 200, 0)})
        for (px, py) in pts:
            if y <= py < y + h and x <= px < x + w:
                cv2.circle(o3, (int(px - x), int(py - y)), 4, (255, 0, 0), 1)
        for (px, py) in cg:
            if y <= py < y + h and x <= px < x + w:
                cv2.circle(o3, (int(px - x), int(py - y)), 6, (0, 0, 255), 1)
        strip = np.concatenate([o1, o2, o3], 1)
        render.save_rgb(strip, os.path.join(a.out, f"6502_{tag}_seg_vs_gt.png"), max_w=4000)
    # whole-die thumbnails: prediction classes
    vis = np.zeros((H, W, 3), np.uint8) + 30
    vis[diff_full] = (200, 170, 30)
    vis[poly_p] = (200, 40, 40)
    vis[metal_p] = (vis[metal_p] * 0.4 + np.array([80, 120, 255]) * 0.6).astype(np.uint8)
    render.save_rgb(vis, os.path.join(a.out, "6502_full_segmentation.png"), max_w=2400)


if __name__ == "__main__":
    sys.exit(main())
