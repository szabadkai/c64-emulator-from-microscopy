"""Align the visual6502 6502 die photographs to the segdefs coordinate system.

visual6502.org publishes 4677x5097 'surface' (top, metal+poly) and 'substrate'
(delayered, diffusion) photos, already aligned to each other, plus 1027x1119
renders of the vector model in the same frame (MOS_6502_layers_*.png). We:

 1. coarse: register the rasterised GT diffusion against MOS_6502_layers_diffusion.png
    (scale sweep + phase correlation)  -> chip units -> layer-image px
 2. scale to the 4677 px photo frame and refine with an affine ECC fit between the
    GT diffusion *outline* map and the substrate photo's dark-outline map
 3. measure residual misalignment per 512 px tile (phase correlation) and fit a
    smooth (2nd-order polynomial) correction -> final mapping chip -> photo px

Output: out/align6502.json with the affine + polynomial terms and residual stats.

CLI:  python -I src/align6502.py --out out/align6502.json
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

IMG = os.path.join(os.path.dirname(HERE), "data", "visual6502", "images")
PHOTO_W, PHOTO_H = 4677, 5097


def load_photo(name):
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    return np.asarray(Image.open(os.path.join(IMG, name)).convert("RGB"))


def coarse(chip):
    import cv2
    from PIL import Image
    L = np.asarray(Image.open(os.path.join(IMG, "MOS_6502_layers_diffusion.png")).convert("RGB")).astype(np.float32)
    lm = (L[..., 1] > 100).astype(np.float32)
    H, W = lm.shape
    best = None
    for s in np.arange(0.108, 0.117, 0.0005):
        g = gt6502.combined(gt6502.rasterize_layers(chip, s, layers=(1, 2, 3, 4)))["diff"].astype(np.float32)
        G = np.zeros((H, W), np.float32)
        h, w = min(H, g.shape[0]), min(W, g.shape[1])
        G[:h, :w] = g[:h, :w]
        (dx, dy), _ = cv2.phaseCorrelate(lm - lm.mean(), G - G.mean())
        Gs = cv2.warpAffine(G, np.float32([[1, 0, -dx], [0, 1, -dy]]), (W, H))
        iou = ((Gs > .5) & (lm > .5)).sum() / max(((Gs > .5) | (lm > .5)).sum(), 1)
        if best is None or iou > best[0]:
            best = (iou, s, dx, dy)
    iou, s, dx, dy = best
    k = PHOTO_W / W
    # photo_px = A @ [s_px_x, s_px_y, 1] where s_px = chip raster at scale 1 (x, 10000-y)
    A = np.array([[s * k, 0, -dx * k], [0, s * k, -dy * k]], np.float64)
    return A, {"layer_image_iou": round(float(iou), 4), "layer_scale": float(s)}


def outline_map(mask, blur=1.5):
    import cv2
    m = mask.astype(np.uint8)
    e = cv2.morphologyEx(m, cv2.MORPH_GRADIENT, np.ones((3, 3), np.uint8)).astype(np.float32)
    return cv2.GaussianBlur(e, (0, 0), blur)


def photo_outline(sub_rgb, blur=1.5):
    import cv2
    g = cv2.cvtColor(sub_rgb, cv2.COLOR_RGB2GRAY).astype(np.float32)
    bg = cv2.GaussianBlur(g, (0, 0), 6)
    d = np.clip(bg - g, 0, None)  # dark lines on light background
    return cv2.GaussianBlur(d, (0, 0), blur)


def gt_raster_photo(chip, A, layers, shape=(PHOTO_H, PHOTO_W), poly=None):
    """Rasterise GT layers directly in photo pixel space with affine A (and optional poly correction)."""
    import cv2
    SH = 4
    out = np.zeros(shape, np.uint8)
    for sgm in chip.segs:
        if sgm.layer not in layers:
            continue
        p = chip_to_photo(sgm.pts, A, poly)
        pi = np.round((p - 0.5) * (1 << SH)).astype(np.int32).reshape(-1, 1, 2)
        cv2.fillPoly(out, [pi], 1, lineType=cv2.LINE_8, shift=SH)
    return out.astype(bool)


def chip_to_photo(pts, A, poly=None):
    x = pts[:, 0]
    y = gt6502.CHIP_SIZE - pts[:, 1]
    px = A[0, 0] * x + A[0, 1] * y + A[0, 2]
    py = A[1, 0] * x + A[1, 1] * y + A[1, 2]
    if poly is not None:
        cx, cy = poly["cx"], poly["cy"]
        u, v = (px - PHOTO_W / 2) / PHOTO_W, (py - PHOTO_H / 2) / PHOTO_H
        basis = np.stack([np.ones_like(u), u, v, u * u, u * v, v * v], 1)
        px = px + basis @ np.asarray(cx)
        py = py + basis @ np.asarray(cy)
    return np.stack([px, py], 1)


def refine_ecc(chip, A, sub):
    import cv2
    tmpl = outline_map(gt_raster_photo(chip, A, (1, 2, 3, 4)))
    img = photo_outline(sub)
    # work at half resolution for speed
    f = 0.5
    t2 = cv2.resize(tmpl, None, fx=f, fy=f, interpolation=cv2.INTER_AREA)
    i2 = cv2.resize(img, None, fx=f, fy=f, interpolation=cv2.INTER_AREA)
    warp = np.eye(2, 3, dtype=np.float32)
    crit = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 200, 1e-6)
    cc, warp = cv2.findTransformECC(t2, i2, warp, cv2.MOTION_AFFINE, crit, None, 5)
    warp[:, 2] /= f
    # compose: new_photo = warp applied to template coords. template coords = A @ chip.
    # ECC finds warp such that img(warp(x)) ~ tmpl(x) -> photo point of template pixel x is warp(x)
    W3 = np.vstack([warp.astype(np.float64), [0, 0, 1]])
    A3 = np.vstack([A, [0, 0, 1]])
    return (W3 @ A3)[:2], float(cc)


def tile_residuals(chip, A, sub, tile=512, poly=None, maxd=25):
    import cv2
    tmpl = outline_map(gt_raster_photo(chip, A, (1, 2, 3, 4), poly=poly))
    img = photo_outline(sub)
    pts, d = [], []
    win = cv2.createHanningWindow((tile, tile), cv2.CV_32F)
    for r in range(0, PHOTO_H - tile + 1, tile // 2):
        for c in range(0, PHOTO_W - tile + 1, tile // 2):
            a = tmpl[r:r + tile, c:c + tile]
            b = img[r:r + tile, c:c + tile]
            if a.std() < 0.02 or b.std() < 0.5:
                continue
            (dx, dy), resp = cv2.phaseCorrelate(a, b, win)
            if resp < 0.08 or abs(dx) > maxd or abs(dy) > maxd:
                continue
            pts.append((c + tile / 2, r + tile / 2))
            d.append((dx, dy, resp))
    return np.array(pts), np.array(d)


def fit_poly(pts, d, prev=None):
    """Weighted LSQ 2nd-order polynomial of the residual shift field; adds to prev."""
    u = (pts[:, 0] - PHOTO_W / 2) / PHOTO_W
    v = (pts[:, 1] - PHOTO_H / 2) / PHOTO_H
    B = np.stack([np.ones_like(u), u, v, u * u, u * v, v * v], 1)
    w = d[:, 2]
    # robust: drop the worst 15 % after a first fit
    keep = np.ones(len(d), bool)
    for _ in range(2):
        cx = np.linalg.lstsq(B[keep] * w[keep, None], d[keep, 0] * w[keep], rcond=None)[0]
        cy = np.linalg.lstsq(B[keep] * w[keep, None], d[keep, 1] * w[keep], rcond=None)[0]
        res = np.hypot(B @ cx - d[:, 0], B @ cy - d[:, 1])
        keep = res <= np.percentile(res, 85)
    if prev is not None:
        cx = cx + np.asarray(prev["cx"])
        cy = cy + np.asarray(prev["cy"])
    return {"cx": cx.tolist(), "cy": cy.tolist()}


def load_alignment(path=None):
    path = path or os.path.join(os.path.dirname(HERE), "out", "align6502.json")
    with open(path) as f:
        a = json.load(f)
    return np.array(a["affine"]), a.get("poly")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(HERE), "out", "align6502.json"))
    a = ap.parse_args(argv)
    chip = gt6502.load()
    A0, info = coarse(chip)
    print("coarse", A0.tolist(), info)
    sub = load_photo("6502_sub_op10x_BF_4677.png")
    pts, d = tile_residuals(chip, A0, sub)
    print(f"coarse residuals: {len(d)} tiles, median |d| = {np.median(np.hypot(d[:, 0], d[:, 1])):.2f} px")
    A1, cc = refine_ecc(chip, A0, sub)
    print("ecc", A1.tolist(), "cc", cc)
    pts, d = tile_residuals(chip, A1, sub)
    r1 = np.hypot(d[:, 0], d[:, 1])
    print(f"affine residuals: {len(d)} tiles, median {np.median(r1):.2f} px, p90 {np.percentile(r1, 90):.2f} px")
    poly = None
    for it in range(4):
        poly = fit_poly(pts, d, poly)
        pts, d = tile_residuals(chip, A1, sub, poly=poly)
        r2 = np.hypot(d[:, 0], d[:, 1])
        print(f"poly iter {it}: {len(d)} tiles, median {np.median(r2):.2f} px, p90 {np.percentile(r2, 90):.2f} px")
    d2 = d
    out = {"affine": A1.tolist(), "poly": poly, "coarse": A0.tolist(), "coarse_info": info, "ecc_cc": cc,
           "units_per_photo_px": float(1 / np.sqrt(abs(np.linalg.det(A1[:, :2])))),
           "residual_affine_px": {"median": float(np.median(r1)), "p90": float(np.percentile(r1, 90)), "tiles": len(d)},
           "residual_poly_px": {"median": float(np.median(r2)), "p90": float(np.percentile(r2, 90)), "tiles": len(d2)},
           "note": "photo_px = A @ [x, 10000 - y, 1] + poly(u,v) correction; chip coords from segdefs"}
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)
    print(json.dumps({k: v for k, v in out.items() if k.startswith("resid") or k in ("ecc_cc", "units_per_photo_px")}))


if __name__ == "__main__":
    sys.exit(main())
