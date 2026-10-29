"""Stage 4: photo -> layer masks (classical CV), shared by the 6502 and VIC-II runs.

Building blocks (no learning beyond k-means + naming clusters from a small
labelled tile):
  features()        Lab colour (lightly smoothed) + local texture energy
  kmeans_fit()      k-means on a pixel sample (OpenCV), predict = nearest centre
  name_clusters()   map clusters -> layer classes by majority vote inside ONE
                    labelled training tile (a human would label such a tile)
  cleanup()         per-class morphology (open/close, min area)
  detect_contacts() dark-blob (DoG) detector for contact cuts
  diffusion_regions() edge-bounded region segmentation for delayered photos:
                    dark outline map -> regions -> region features -> class
"""
from __future__ import annotations

import numpy as np


def to_lab(rgb, smooth=1.0):
    import cv2
    lab = cv2.cvtColor(rgb, cv2.COLOR_RGB2LAB).astype(np.float32)
    if smooth:
        lab = cv2.GaussianBlur(lab, (0, 0), smooth)
    return lab


def texture(gray, s=1.0, avg=3.0):
    import cv2
    g = gray.astype(np.float32)
    return cv2.GaussianBlur(np.abs(g - cv2.GaussianBlur(g, (0, 0), s)), (0, 0), avg)


def darkness(gray, bg_sigma=6.0, s=1.0):
    """Dark-line/dot response: local background minus intensity (>= 0)."""
    import cv2
    g = gray.astype(np.float32)
    d = np.clip(cv2.GaussianBlur(g, (0, 0), bg_sigma) - g, 0, None)
    return cv2.GaussianBlur(d, (0, 0), s) if s else d


def features(rgb, extra=()):
    import cv2
    lab = to_lab(rgb, 1.0)
    gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY)
    f = [lab[..., 0], lab[..., 1], lab[..., 2], texture(gray, 1.0, 3.0) * 3.0, darkness(gray, 6.0, 1.5) * 2.0]
    f.extend(extra)
    return np.stack(f, -1).astype(np.float32)


def kmeans_fit(feat, k=8, sample=250000, seed=0, mask=None):
    import cv2
    rng = np.random.default_rng(seed)
    X = feat.reshape(-1, feat.shape[-1])
    idx = np.flatnonzero(mask.ravel()) if mask is not None else np.arange(len(X))
    idx = rng.choice(idx, size=min(sample, len(idx)), replace=False)
    mu, sd = X[idx].mean(0), X[idx].std(0) + 1e-6
    Z = (X[idx] - mu) / sd
    crit = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-3)
    cv2.setRNGSeed(seed)
    _, _, centers = cv2.kmeans(Z, k, None, crit, 4, cv2.KMEANS_PP_CENTERS)
    return {"mu": mu, "sd": sd, "centers": centers}


def kmeans_predict(feat, model, chunk=2_000_000):
    X = feat.reshape(-1, feat.shape[-1])
    out = np.empty(len(X), np.int32)
    C = model["centers"]
    for i in range(0, len(X), chunk):
        Z = (X[i:i + chunk] - model["mu"]) / model["sd"]
        d = ((Z[:, None, :] - C[None]) ** 2).sum(-1)
        out[i:i + chunk] = d.argmin(1)
    return out.reshape(feat.shape[:2])


def name_clusters(labels, gt_class, region, n_classes, ignore=None):
    """cluster -> class by majority vote inside `region` (bool mask) of the GT class map."""
    sel = region.copy()
    if ignore is not None:
        sel &= ~ignore
    k = labels.max() + 1
    M = np.zeros((k, n_classes), np.int64)
    np.add.at(M, (labels[sel], gt_class[sel]), 1)
    return M.argmax(1), M


def cleanup(mask, open_px=1, close_px=1, min_area=0):
    import cv2
    m = mask.astype(np.uint8)
    if close_px:
        m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * close_px + 1,) * 2))
    if open_px:
        m = cv2.morphologyEx(m, cv2.MORPH_OPEN, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * open_px + 1,) * 2))
    if min_area:
        n, lab, st, _ = cv2.connectedComponentsWithStats(m, 8)
        keep = st[:, cv2.CC_STAT_AREA] >= min_area
        keep[0] = False
        m = keep[lab].astype(np.uint8)
    return m.astype(bool)


def iou(pred, gt, valid=None):
    if valid is not None:
        pred, gt = pred[valid], gt[valid]
    inter = np.logical_and(pred, gt).sum()
    uni = np.logical_or(pred, gt).sum()
    tp = inter
    return {"iou": round(float(inter / max(uni, 1)), 4),
            "precision": round(float(tp / max(pred.sum(), 1)), 4),
            "recall": round(float(tp / max(gt.sum(), 1)), 4)}


def detect_contacts(gray, sigma=2.2, thresh=None, mask=None, min_dist=5):
    """Dark-blob detector (difference of Gaussians on the inverted image).
    Returns (N,2) array of (x, y) centres and the response map."""
    import cv2
    g = 255.0 - gray.astype(np.float32)
    dog = cv2.GaussianBlur(g, (0, 0), sigma) - cv2.GaussianBlur(g, (0, 0), sigma * 2.5)
    if thresh is None:
        v = dog[mask] if mask is not None else dog
        thresh = float(np.percentile(v, 99.0))
    k = 2 * min_dist + 1
    mx = cv2.dilate(dog, np.ones((k, k), np.uint8))
    peaks = (dog >= mx) & (dog > thresh)
    if mask is not None:
        peaks &= mask
    ys, xs = np.nonzero(peaks)
    return np.stack([xs, ys], 1), dog, thresh


def match_points(pred, gt, tol):
    """Greedy one-to-one matching within tol px. Returns precision/recall/F1."""
    from scipy.spatial import cKDTree
    if len(pred) == 0 or len(gt) == 0:
        return {"precision": 0.0, "recall": 0.0, "f1": 0.0, "pred": len(pred), "gt": len(gt)}
    t = cKDTree(gt)
    d, j = t.query(pred, k=1, distance_upper_bound=tol)
    order = np.argsort(d)
    used = set()
    tp = 0
    for i in order:
        if np.isfinite(d[i]) and j[i] not in used:
            used.add(j[i])
            tp += 1
    p, r = tp / len(pred), tp / len(gt)
    return {"precision": round(p, 4), "recall": round(r, 4), "f1": round(2 * p * r / max(p + r, 1e-9), 4),
            "pred": int(len(pred)), "gt": int(len(gt))}


def diffusion_regions(rgb, edge_pct=75.0, close_px=5, core=None):
    """Edge-bounded regions of a delayered (substrate) photo.

    Diffusion outlines are thin dark lines, clearest in the Lab b channel. The
    outline map (dark-line response > edge_pct percentile, closed with a
    close_px ellipse to bridge gaps) splits the image into regions; each region
    gets its mean lightness relative to the local field level (a 61 px max-filter
    of the smoothed L channel). Returns (labels, edges, rel_lightness, area).
    Regions that still leak into the background end up as one huge region.
    """
    import cv2
    lab = cv2.cvtColor(rgb, cv2.COLOR_RGB2LAB).astype(np.float32)
    b, L = lab[..., 2], lab[..., 0]
    dk = np.clip(cv2.GaussianBlur(b, (0, 0), 3) - b, 0, None)
    dk = cv2.GaussianBlur(dk, (0, 0), 0.7)
    ref_px = dk[core] if core is not None else dk
    e = dk > np.percentile(ref_px, edge_pct)
    e = cv2.morphologyEx(e.astype(np.uint8), cv2.MORPH_CLOSE,
                         cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (close_px, close_px))).astype(bool)
    n, rl = cv2.connectedComponents((~e).astype(np.uint8), connectivity=4)
    f = rl.ravel()
    area = np.bincount(f, minlength=n).astype(np.float64)
    mL = np.bincount(f, weights=L.ravel().astype(np.float64), minlength=n) / np.maximum(area, 1)
    ref = cv2.dilate(cv2.GaussianBlur(L, (0, 0), 3), np.ones((61, 61), np.uint8))
    mref = np.bincount(f, weights=ref.ravel().astype(np.float64), minlength=n) / np.maximum(area, 1)
    return rl, e, mL - mref, area


def regions_to_mask(rl, edges, rel, area, t, max_area=200000):
    import cv2
    isd = (rel < t) & (area >= 8) & (area < max_area)
    isd[0] = False
    pred = isd[rl]
    return pred | (edges & (cv2.dilate(pred.astype(np.uint8), np.ones((3, 3), np.uint8)) > 0))


def qda_fit(feat, cls, region, n_classes, ignore=None, sample=200000, seed=0, reg=1e-2):
    """Per-class Gaussian (quadratic discriminant) fitted on the labelled training tile."""
    rng = np.random.default_rng(seed)
    sel = region.copy()
    if ignore is not None:
        sel &= ~ignore
    X = feat[sel]
    y = cls[sel]
    if len(X) > sample:
        i = rng.choice(len(X), sample, replace=False)
        X, y = X[i], y[i]
    model = []
    for c in range(n_classes):
        Xc = X[y == c]
        if len(Xc) < 10:
            model.append(None)
            continue
        mu = Xc.mean(0)
        cov = np.cov(Xc.T) + reg * np.eye(X.shape[1]) * np.trace(np.cov(Xc.T)) / X.shape[1]
        inv = np.linalg.inv(cov)
        _, logdet = np.linalg.slogdet(cov)
        model.append((mu, inv, logdet, np.log(len(Xc) / len(X))))
    return model


def qda_predict(feat, model, chunk=2_000_000, prior_weight=1.0):
    X = feat.reshape(-1, feat.shape[-1])
    out = np.empty(len(X), np.int32)
    for i in range(0, len(X), chunk):
        x = X[i:i + chunk]
        scores = []
        for m in model:
            if m is None:
                scores.append(np.full(len(x), -np.inf, np.float32))
                continue
            mu, inv, logdet, lp = m
            d = x - mu
            scores.append(-0.5 * np.einsum("ij,jk,ik->i", d, inv, d) - 0.5 * logdet + prior_weight * lp)
        out[i:i + chunk] = np.argmax(np.stack(scores, 1), 1)
    return out.reshape(feat.shape[:2])
