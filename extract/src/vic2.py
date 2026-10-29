"""Stage 5: first attempt on the VIC-II 6569 R1 (Michael Huth's reflected-light die photo).

Data (downloaded, see README):
  data/retronn/6569R1.jpg               17547 x 19667 stitched overview (64 MB JPEG)
  data/retronn/raw/0707.png ...         3 raw 2048x1536 tiles, only used for scale calibration
  data/wmhost/MOS_6569R1_L1_metal.png   Tommi Lempinen's 1-bit metal-layer trace (19723 x 17609,
                                        rotated 90 deg w.r.t. the photo) -> independent metal reference

Scale: raw tiles were shot with 0.5 mm (X) / 0.45 mm (Y) stage steps; locating three raw tiles
in the stitched JPEG gives 1576 px / 0.5 mm and 1416 px / 0.45 mm -> 0.317 um/px (both axes),
i.e. the JPEG is the raw scale (no down-sampling). Die image = 5.57 x 6.24 mm.

Sub-commands:
  tiles    --tile 2048 [--write i,j ...]  overview PNG with tile grid + index json (+ selected tiles)
  align                                    register Tommi's metal map to the photo (rot90, scale,
                                           affine ECC, per-tile residuals)  -> out/vic2/align_metal.json
  region   --x --y --w --h                 segment one region, score metal against Tommi's map,
                                           extract a netlist from what is visible, write overlays

CLI examples:
  python -I src/vic2.py tiles
  python -I src/vic2.py align
  python -I src/vic2.py region --x 2600 --y 1700 --w 1200 --h 1000 --name array_tl
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
import segment  # noqa: E402
import render  # noqa: E402

ROOT = os.path.dirname(HERE)
JPG = os.path.join(ROOT, "data", "retronn", "6569R1.jpg")
METAL = os.path.join(ROOT, "data", "wmhost", "MOS_6569R1_L1_metal.png")
OUT = os.path.join(ROOT, "out", "vic2")
UM_PER_PX = 0.317


def load_photo(scale_div=1):
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    im = Image.open(JPG)
    if scale_div > 1:
        im.draft("RGB", (im.width // scale_div, im.height // scale_div))
    return np.asarray(im.convert("RGB"))


def load_metal_rotated():
    """Tommi's metal map rotated into the photo orientation (True = metal)."""
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    m = np.asarray(Image.open(METAL).convert("L")) < 128  # black = metal
    return np.ascontiguousarray(np.rot90(m, k=-1))


def cmd_tiles(a):
    from PIL import Image, ImageDraw
    os.makedirs(OUT, exist_ok=True)
    full = load_photo(1)
    H, W = full.shape[:2]
    T = a.tile
    idx = []
    for j, y in enumerate(range(0, H, T)):
        for i, x in enumerate(range(0, W, T)):
            idx.append({"i": i, "j": j, "x": x, "y": y, "w": min(T, W - x), "h": min(T, H - y)})
    small = Image.fromarray(full[::8, ::8])
    d = ImageDraw.Draw(small)
    for t in idx:
        d.rectangle([t["x"] // 8, t["y"] // 8, (t["x"] + t["w"]) // 8, (t["y"] + t["h"]) // 8], outline=(255, 0, 0))
        d.text((t["x"] // 8 + 3, t["y"] // 8 + 3), f"{t['i']},{t['j']}", fill=(255, 255, 0))
    small.save(os.path.join(OUT, "6569R1_overview_tiles.jpg"), quality=90)
    with open(os.path.join(OUT, "tiles_index.json"), "w") as f:
        json.dump({"tile": T, "image": [W, H], "um_per_px": UM_PER_PX, "tiles": idx}, f)
    tdir = os.path.join(ROOT, "data", "retronn", "tiles")
    for spec in a.write or []:
        i, j = map(int, spec.split(","))
        t = next(t for t in idx if t["i"] == i and t["j"] == j)
        os.makedirs(tdir, exist_ok=True)
        Image.fromarray(full[t["y"]:t["y"] + t["h"], t["x"]:t["x"] + t["w"]]).save(
            os.path.join(tdir, f"tile_{i:02d}_{j:02d}.png"))
    print(f"{len(idx)} tiles of {T}px; overview + index written to {OUT}")


def metal_likelihood(rgb):
    """Bright, low-saturation-blue = metal in these photos; return smoothed L channel."""
    import cv2
    lab = cv2.cvtColor(rgb, cv2.COLOR_RGB2LAB).astype(np.float32)
    return lab[..., 0]


def cmd_align(a):
    import cv2
    os.makedirs(OUT, exist_ok=True)
    photo = load_photo(1)
    H, W = photo.shape[:2]
    M = load_metal_rotated()
    print("photo", (W, H), "metal(rotated)", M.shape[::-1])
    # 1) coarse at 1/8: resize metal to the photo grid (uniform scale), phase-correlate
    f = 8
    P8 = cv2.resize(metal_likelihood(photo[::2, ::2]), (W // f, H // f), interpolation=cv2.INTER_AREA)
    best = None
    for s in np.arange(0.990, 1.006, 0.001):
        m8 = cv2.resize(M.astype(np.float32), (int(M.shape[1] * s / f), int(M.shape[0] * s / f)),
                        interpolation=cv2.INTER_AREA)
        G = np.zeros_like(P8)
        h, w = min(G.shape[0], m8.shape[0]), min(G.shape[1], m8.shape[1])
        G[:h, :w] = m8[:h, :w]
        (dx, dy), resp = cv2.phaseCorrelate(G - G.mean(), P8 - P8.mean())
        Gs = cv2.warpAffine(G, np.float32([[1, 0, dx], [0, 1, dy]]), (P8.shape[1], P8.shape[0]))
        cc = np.corrcoef(Gs.ravel(), P8.ravel())[0, 1]
        if best is None or cc > best[0]:
            best = (cc, s, dx * f, dy * f)
    cc0, s, dx, dy = best
    A = np.array([[s, 0, dx], [0, s, dy]], np.float64)  # metal px -> photo px
    print(f"coarse: scale {s:.4f} shift ({dx:.1f},{dy:.1f}) corr {cc0:.3f}")
    # 2) affine ECC at 1/4
    f = 4
    P4 = cv2.resize(metal_likelihood(photo[::2, ::2]), (W // f, H // f), interpolation=cv2.INTER_AREA)
    A4 = A.copy()
    A4[:, 2] /= f
    M4 = cv2.resize(M.astype(np.float32), (M.shape[1] // f, M.shape[0] // f), interpolation=cv2.INTER_AREA)
    tmpl = cv2.warpAffine(M4, A4.astype(np.float32), (P4.shape[1], P4.shape[0]))
    warp = np.eye(2, 3, dtype=np.float32)
    crit = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 200, 1e-6)
    cc1, warp = cv2.findTransformECC(cv2.GaussianBlur(tmpl, (0, 0), 1), cv2.GaussianBlur(P4, (0, 0), 1),
                                     warp, cv2.MOTION_AFFINE, crit, None, 5)
    warp = warp.astype(np.float64)
    warp[:, 2] *= f
    A1 = (np.vstack([warp, [0, 0, 1]]) @ np.vstack([A, [0, 0, 1]]))[:2]
    print(f"ECC affine corr {cc1:.3f}: {A1.tolist()}")
    # 3) residuals on 1024 px tiles at full res (phase correlation)
    res = tile_residuals(photo, M, A1)
    r = np.hypot(res[:, 2], res[:, 3])
    print(f"tile residuals: n={len(r)} median {np.median(r):.2f} px p90 {np.percentile(r, 90):.2f} px")
    out = {"metal_to_photo_affine": A1.tolist(), "rot90_k": -1, "coarse_corr": cc0, "ecc_corr": cc1,
           "residual_px": {"median": float(np.median(r)), "p90": float(np.percentile(r, 90)), "tiles": int(len(r))},
           "residual_tiles": res.tolist()}
    with open(os.path.join(OUT, "align_metal.json"), "w") as fo:
        json.dump(out, fo)


def warp_metal(M, A, x, y, w, h):
    """Metal reference resampled into the photo window [x:x+w, y:y+h]."""
    import cv2
    A = np.array(A, np.float64)
    Ai = cv2.invertAffineTransform(A)  # photo -> metal
    # window offset
    T = np.array([[1, 0, x], [0, 1, y], [0, 0, 1]], np.float64)
    Mw = (np.vstack([Ai, [0, 0, 1]]) @ T)[:2]
    return cv2.warpAffine(M.astype(np.uint8), Mw.astype(np.float32), (w, h),
                          flags=cv2.INTER_NEAREST | cv2.WARP_INVERSE_MAP).astype(bool)


def tile_residuals(photo, M, A, tile=1024, step=2048):
    import cv2
    H, W = photo.shape[:2]
    win = cv2.createHanningWindow((tile, tile), cv2.CV_32F)
    out = []
    for y in range(512, H - tile - 512, step):
        for x in range(512, W - tile - 512, step):
            m = warp_metal(M, A, x, y, tile, tile).astype(np.float32)
            p = metal_likelihood(photo[y:y + tile, x:x + tile])
            if m.std() < 0.1:
                continue
            (dx, dy), resp = cv2.phaseCorrelate(cv2.GaussianBlur(m, (0, 0), 1.5),
                                                cv2.GaussianBlur(p, (0, 0), 1.5), win)
            if resp > 0.05:
                out.append((x + tile / 2, y + tile / 2, dx, dy, resp))
    return np.array(out)


def local_refine(photo_win, metal_win):
    """Sub-window translation refinement (phase correlation) of the metal reference."""
    import cv2
    m = metal_win.astype(np.float32)
    p = metal_likelihood(photo_win)
    win = cv2.createHanningWindow((m.shape[1], m.shape[0]), cv2.CV_32F)
    (dx, dy), resp = cv2.phaseCorrelate(cv2.GaussianBlur(m, (0, 0), 1.5), cv2.GaussianBlur(p, (0, 0), 1.5), win)
    return dx, dy, resp


def cmd_region(a):
    import cv2
    sys.path.insert(0, HERE)
    import extract
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "align_metal.json")) as f:
        al = json.load(f)
    A = np.array(al["metal_to_photo_affine"])
    photo = load_photo(1)
    H, W = photo.shape[:2]
    M = load_metal_rotated()
    x, y, w, h = a.x, a.y, a.w, a.h
    # training tile for naming metal clusters: a *different* area of the die
    tx, ty, tw, th = a.train
    rgn = np.ascontiguousarray(photo[y:y + h, x:x + w])
    trn = np.ascontiguousarray(photo[ty:ty + th, tx:tx + tw])
    m_rgn = warp_metal(M, A, x, y, w, h)
    m_trn = warp_metal(M, A, tx, ty, tw, th)
    # local translation refinement of the reference in both windows
    rep = {"region": [x, y, w, h], "train_tile": [tx, ty, tw, th], "um_per_px": UM_PER_PX,
           "region_um": [round(w * UM_PER_PX, 1), round(h * UM_PER_PX, 1)]}
    for name, (win_img, ox, oy) in {"region": (rgn, x, y), "train": (trn, tx, ty)}.items():
        mm = warp_metal(M, A, ox, oy, win_img.shape[1], win_img.shape[0])
        dx, dy, resp = local_refine(win_img, mm)
        used = bool(resp >= 0.05 and abs(dx) <= 8 and abs(dy) <= 8)  # periodic arrays give no reliable peak
        A2 = A.copy()
        if used:
            A2[:, 2] += (dx, dy)
        rep[f"local_shift_{name}"] = {"dx": round(dx, 2), "dy": round(dy, 2), "resp": round(resp, 3), "applied": used}
        if name == "region":
            m_rgn = warp_metal(M, A2, ox, oy, w, h)
        else:
            m_trn = warp_metal(M, A2, ox, oy, tw, th)
    # ---- metal: QDA on Lab+texture, trained on the training tile with Tommi's labels
    f_rgn = segment.features(rgn)
    f_trn = segment.features(trn)
    q = segment.qda_fit(f_trn, m_trn.astype(np.int32), np.ones(m_trn.shape, bool), 2)
    # close_px=4 (~2.5 um): bridges the dark step-edge lines that underlying poly/diffusion
    # edges print onto the metal surface; metal spacing here is >= ~6 um so lines do not merge
    metal_p = segment.cleanup(segment.qda_predict(f_rgn, q) == 1, open_px=1, close_px=4, min_area=40)
    # boundary-tolerant score as well (Tommi's map is hand-cleaned, edges are approximate)
    band = cv2.morphologyEx(m_rgn.astype(np.uint8), cv2.MORPH_GRADIENT, np.ones((5, 5), np.uint8)).astype(bool)
    rep["metal_vs_tommi"] = {"strict": segment.iou(metal_p, m_rgn),
                             "ignoring_2px_boundary_band": segment.iou(metal_p, m_rgn, ~band)}
    # ---- non-metal pixels: unsupervised 3-way split (there is no reference for poly/diffusion)
    nm = ~cv2.dilate(metal_p.astype(np.uint8), np.ones((5, 5), np.uint8)).astype(bool)
    lab = cv2.cvtColor(rgn, cv2.COLOR_RGB2LAB).astype(np.float32)
    lab = cv2.GaussianBlur(lab, (0, 0), 1.5)
    X = lab[nm][:, 1:3]  # chroma only: the three under-oxide materials differ in hue, not brightness
    crit = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-3)
    cv2.setRNGSeed(0)
    k = 3
    _, lb, cen = cv2.kmeans(X.astype(np.float32), k, None, crit, 5, cv2.KMEANS_PP_CENTERS)
    cl = np.full((h, w), -1, np.int32)
    cl[nm] = lb.ravel()
    # name clusters by brightness: darkest = field oxide, then poly / diffusion are ambiguous
    Lmean = [float(lab[..., 0][cl == c].mean()) for c in range(k)]
    order = np.argsort(Lmean)
    names = {int(order[0]): "field?", int(order[1]): "poly?", int(order[2]): "diffusion?"}
    rep["nonmetal_clusters"] = {names[c]: {"L": round(Lmean[c], 1), "a": round(float(cen[c][0]), 1),
                                           "b": round(float(cen[c][1]), 1),
                                           "area_frac": round(float((cl == c).mean()), 4)} for c in range(k)}
    poly_p = segment.cleanup(cl == order[1], open_px=1, close_px=1, min_area=40)
    diff_p = segment.cleanup(cl == order[2], open_px=1, close_px=1, min_area=40)
    # ---- contacts: ring-shaped cuts; detect bright centre + dark ring with DoG on the L channel
    gray = cv2.cvtColor(rgn, cv2.COLOR_RGB2GRAY)
    pts, dog, thr = segment.detect_contacts(255 - gray, sigma=3.0, mask=metal_p, min_dist=8)
    rep["contacts_detected"] = int(len(pts))
    cmask = np.zeros((h, w), np.uint8)
    for (px, py) in pts:
        cv2.circle(cmask, (int(px), int(py)), 6, 1, -1)
    # ---- netlist attempt: what is visible only (no buried contacts, no diffusion under metal)
    s_units = 1.0  # extractor works in "units"; use 1 unit = 1 px here
    masks = {"metal": metal_p, "poly": poly_p, "diff": diff_p, "contacts": cmask.astype(bool),
             "buried": np.zeros((h, w), bool)}
    meta = {"scale": s_units, "anchors": {}}
    # 6502 thresholds are in 6502 units (0.444 um). Convert: px = um / 0.317
    k_um = 0.444 / UM_PER_PX
    res, aux = extract.extract(masks, meta, lmax_units=26 * k_um, min_channel_area_units=120 * k_um ** 2,
                               min_border_units=14 * k_um, lmax_pullup_units=64 * k_um, log=print)
    netdir = os.path.join(OUT, f"netlist_{a.name}")
    extract.write_netlist(res, netdir, aux)
    rep["netlist"] = {k2: v for k2, v in res["meta"].items() if k2 != "seconds"}
    rep["netlist"]["transistors"] = len(res["transistors"])
    rep["netlist"]["nodes"] = len(res["nodes"])
    rep["netlist"]["metal_nets_from_reference_map"] = int(
        cv2.connectedComponents(m_rgn.astype(np.uint8), connectivity=8)[0] - 1)
    rep["netlist"]["metal_nets_from_segmentation"] = int(
        cv2.connectedComponents(metal_p.astype(np.uint8), connectivity=8)[0] - 1)
    with open(os.path.join(OUT, f"region_{a.name}.json"), "w") as f:
        json.dump(rep, f, indent=1)
    print(json.dumps(rep, indent=1))
    # ---- overlays
    o1 = render.overlay_outlines(rgn, {"seg": metal_p, "ref": m_rgn}, {"seg": (255, 0, 255), "ref": (0, 255, 255)})
    o2 = rgn.copy()
    o2 = render.overlay_outlines(o2, {"poly": poly_p, "diff": diff_p}, {"poly": (255, 0, 0), "diff": (255, 220, 0)})
    for (px, py) in pts:
        cv2.circle(o2, (int(px), int(py)), 7, (0, 255, 0), 2)
    for t in res["transistors"]:
        x0, x1, r0, r1 = t["bb"]
        cv2.rectangle(o2, (int(x0), int(r0)), (int(x1), int(r1)), (255, 0, 255), 1)
    comp = render.compose({"diff": diff_p, "poly": poly_p, "metal": metal_p, "contact": cmask.astype(bool)})
    strip = np.concatenate([o1, o2, comp], 1)
    render.save_rgb(strip, os.path.join(OUT, f"vic2_{a.name}_overlays.png"), max_w=3900)
    render.save_rgb(rgn, os.path.join(OUT, f"vic2_{a.name}_photo.png"), max_w=3900)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tiles")
    t.add_argument("--tile", type=int, default=2048)
    t.add_argument("--write", nargs="*", help="tile ids i,j to write at full resolution")
    sub.add_parser("align")
    r = sub.add_parser("region")
    r.add_argument("--x", type=int, required=True)
    r.add_argument("--y", type=int, required=True)
    r.add_argument("--w", type=int, default=1200)
    r.add_argument("--h", type=int, default=1000)
    r.add_argument("--train", type=int, nargs=4, default=[2600, 4200, 1200, 1000],
                   help="x y w h of the tile used to name metal clusters (must not overlap the region)")
    r.add_argument("--name", default="region")
    a = ap.parse_args(argv)
    {"tiles": cmd_tiles, "align": cmd_align, "region": cmd_region}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
