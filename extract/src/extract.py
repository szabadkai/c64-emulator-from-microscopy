"""Stage 2: geometry -> transistor netlist extractor (NMOS, silicon gate, buried contacts).

Input  : per-layer boolean masks (no node IDs):
           metal, poly, diff        conductors (diff = *doped* diffusion; under a
                                    self-aligned gate there is NO diff -> channel gap)
           contacts                 metal contact cuts (connect metal to the layer
                                    below: poly if the cut sits mostly on poly,
                                    else diffusion)
           buried                   buried contacts (poly <-> diffusion)
           vss_seed, vcc_seed       optional: pixels known to be on the power nets
                                    (traced power buses / their diffusion contacts)
         plus a JSON sidecar: {"scale": px per chip unit, "anchors": {name: {row,col,layer}}}
Output : netlist directory with
           netlist.json   nodes (id, names, pullup, layers, area) + transistors
           transdefs.js   visual6502-format transistor list
                          ['tN', gate, c1, c2, [xmin,xmax,ymin,ymax], [w1,w2,len,0,area]]
           nodenames.js   visual6502-format names for anchored nodes
           segdefs.js     (optional, --segdefs) vectorised polygons per node/layer

Algorithm
 1. Channel detection. A transistor channel is poly lying in the *gap* between two
    different diffusion components. Scan lines at 0/45/90/135 deg find runs of
    non-diffusion pixels that are entirely under poly, bounded by two different
    diffusion components and at most --lmax chip units long (--lmax-pullup if one
    side is VCC diffusion: depletion loads are long-channel). Each pixel keeps the
    pair of components it bridges; one channel = one connected region of one pair,
    with >= min_border on both sides (see find_channels).
 2. Conductor components: connected components of metal, poly, diffusion (+ buried
    contact area counts as diffusion).
 3. Union-find over components: contacts (metal<->poly or metal<->diff),
    buried contacts (poly<->diff), power seeds (diff<->VSS/VCC).
 4. Each channel component -> gate = poly component, terminals = distinct electrical
    nodes of the diffusion components bordering the channel. 2 terminals = one
    transistor; >2 = one transistor per terminal pair touching the channel
    (reported); 1 = degenerate (c1==c2).
 5. Depletion loads: a device whose gate equals one terminal while the other terminal
    is VCC is a depletion pull-up -> node flagged pullup ('+'), device dropped
    (visual6502 convention).
 6. Anchors: the node under each anchor pixel on the given layer gets the name.

CLI:
    python -I src/extract.py --masks in.npz --out out/netlist_dir [--lmax 26] [--segdefs]
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np
from scipy import ndimage as ndi

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

S8 = np.ones((3, 3), bool)
S4 = ndi.generate_binary_structure(2, 1)


class UF:
    def __init__(self, n):
        self.p = np.arange(n, dtype=np.int64)

    def find(self, a):
        p = self.p
        r = a
        while p[r] != r:
            r = p[r]
        while p[a] != r:
            p[a], a = r, p[a]
        return r

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            if ra < rb:
                self.p[rb] = ra
            else:
                self.p[ra] = rb


def _line_se(length_px, angle):
    L = max(int(round(length_px)), 2)
    if angle == 0:
        return np.ones((1, L), bool)
    if angle == 90:
        return np.ones((L, 1), bool)
    n = max(int(round(L / np.sqrt(2))), 2)
    se = np.eye(n, dtype=bool)
    return se if angle == 45 else se[::-1]


def _scan_rows(L, P, maxgap):
    """For every row: non-diffusion runs bounded by two *different* diffusion
    components, no longer than maxgap px, and entirely covered by poly.
    Returns (ok mask, label at left end, label at right end)."""
    H, W = L.shape
    col = np.arange(W, dtype=np.int32)[None, :]
    isd = L > 0
    li = np.maximum.accumulate(np.where(isd, col, -1), axis=1)
    ri = np.minimum.accumulate(np.where(isd, col, W)[:, ::-1], axis=1)[:, ::-1]
    ok = (~isd) & P & (li >= 0) & (ri < W) & ((ri - li - 1) <= maxgap)
    rows = np.arange(H)[:, None]
    lic, ric = np.clip(li, 0, W - 1), np.clip(ri, 0, W - 1)
    ll, rl = L[rows, lic], L[rows, ric]
    ok &= ll != rl
    cs = np.cumsum((~isd & ~P).astype(np.int32), axis=1)
    ok &= (cs[rows, np.clip(ri - 1, 0, W - 1)] - cs[rows, lic]) == 0
    return ok, ll, rl, (ri - li - 1)


def _scan_dir(L, P, maxgap, d):
    h, w = L.shape
    if d == "h":
        return _scan_rows(L, P, maxgap)
    if d == "v":
        return tuple(x.T for x in _scan_rows(L.T, P.T, maxgap))
    rr, cc = np.indices((h, w))
    j = (cc - rr + h - 1) if d == "d1" else (cc + rr)
    B = np.zeros((h, w + h), L.dtype)
    BP = np.zeros((h, w + h), bool)
    B[rr, j] = L
    BP[rr, j] = P
    return tuple(x.T[rr, j] for x in _scan_rows(B.T, BP.T, maxgap))


def find_channels(diff, poly, lmax_px, min_area_px=1, tile=768, labD=None, min_border_px=1,
                  lmax_pullup_px=None, vcc_comp=None, hv_frac=0.3):
    """Find transistor channels.

    A channel pixel is poly that fills a short gap (<= lmax_px along one of the
    0/90/45/135 deg directions) between two *different* diffusion components; each
    pixel remembers the pair of diffusion components it bridges. A channel is a
    connected component of the pixels of one pair (after a 3x3 closing inside
    poly-minus-diffusion, to heal breaks at gate corners). A channel must share a
    border of >= min_border_px with BOTH diffusion components (kills the 1-px
    "field slivers" where poly edges overlap neighbouring diffusion fingers). Keeping pairs separate
    stops neighbouring devices that share a gate from merging through the field
    gaps between their diffusion fingers. Same-component gaps (concave diffusion
    corners, U-shapes) are rejected, and >= hv_frac of a channel must come from the
    0/90 deg scans (diagonals only complete corners). Tile-by-tile scanning with a margin > lmax_px
    is exact because valid runs are shorter than the margin.

    Depletion loads have long channels (L up to ~55 units on the 6502), so gaps
    with a VCC diffusion component at one end may be up to lmax_pullup_px long
    (vcc_comp: bool lookup per diffusion component).

    Returns (list of channels {a, b, rows, cols}, union mask).
    """
    import cv2
    if labD is None:
        labD, _ = ndi.label(diff, S8)
    H, W = diff.shape
    nD = int(labD.max())
    maxgap = int(np.ceil(lmax_px))
    maxgap_pu = int(np.ceil(lmax_pullup_px)) if (lmax_pullup_px and vcc_comp is not None) else maxgap
    scan_gap = max(maxgap, maxgap_pu)
    m = scan_gap + 3
    keys, flats, hvs = [], [], []
    for r0 in range(0, H, tile):
        for c0 in range(0, W, tile):
            R0, C0 = max(r0 - m, 0), max(c0 - m, 0)
            R1, C1 = min(r0 + tile + m, H), min(c0 + tile + m, W)
            L = labD[R0:R1, C0:C1]
            P = poly[R0:R1, C0:C1]
            if not P.any() or not (L > 0).any():
                continue
            core = np.zeros(L.shape, bool)
            core[r0 - R0:r0 - R0 + min(tile, H - r0), c0 - C0:c0 - C0 + min(tile, W - c0)] = True
            for d in ("h", "v", "d1", "d2"):
                ok, ll, rl, gap = _scan_dir(L, P, scan_gap, d)
                ok &= core
                if maxgap_pu > maxgap:
                    ok &= (gap <= maxgap) | (vcc_comp[ll] | vcc_comp[rl])
                rr, cc = np.nonzero(ok)
                if rr.size == 0:
                    continue
                a = np.minimum(ll[rr, cc], rl[rr, cc]).astype(np.int64)
                b = np.maximum(ll[rr, cc], rl[rr, cc]).astype(np.int64)
                keys.append(a * (nD + 1) + b)
                flats.append((rr + R0).astype(np.int64) * W + (cc + C0))
                hvs.append(np.full(rr.size, d in ("h", "v")))
    chans = []
    union = np.zeros((H, W), bool)
    if not keys:
        return chans, union
    keys = np.concatenate(keys)
    flats = np.concatenate(flats)
    hvs = np.concatenate(hvs)
    order = np.lexsort((~hvs, flats, keys))  # h/v entries first within a (pair, pixel)
    keys, flats, hvs = keys[order], flats[order], hvs[order]
    uniq = np.ones(len(keys), bool)
    uniq[1:] = (keys[1:] != keys[:-1]) | (flats[1:] != flats[:-1])
    keys, flats, hvs = keys[uniq], flats[uniq], hvs[uniq]
    bounds = np.flatnonzero(np.diff(keys)) + 1
    starts = np.concatenate([[0], bounds])
    ends = np.concatenate([bounds, [len(keys)]])
    k3 = np.ones((3, 3), np.uint8)
    for s0, s1 in zip(starts, ends):
        key = int(keys[s0])
        a, b = divmod(key, nD + 1)
        rr, cc = np.divmod(flats[s0:s1], W)
        R0, C0 = max(int(rr.min()) - 2, 0), max(int(cc.min()) - 2, 0)
        R1, C1 = min(int(rr.max()) + 3, H), min(int(cc.max()) + 3, W)
        mk = np.zeros((R1 - R0, C1 - C0), np.uint8)
        mk[rr - R0, cc - C0] = 1
        hvm = np.zeros((R1 - R0, C1 - C0), bool)
        hvm[rr - R0, cc - C0] = hvs[s0:s1]
        mk = cv2.morphologyEx(mk, cv2.MORPH_CLOSE, k3).astype(bool)
        mk &= poly[R0:R1, C0:C1] & ~diff[R0:R1, C0:C1]
        lab, n = ndi.label(mk, S8)
        LD = labD[R0:R1, C0:C1]
        for i, sl in enumerate(ndi.find_objects(lab), start=1):
            if sl is None:
                continue
            sel = lab == i
            area = int(sel.sum())
            if area < min_area_px:
                continue
            # diagonal scans may only complete channels (gate corners, 45-degree gates);
            # a region found by diagonals alone is a poly junction / corner artefact
            if hvm[sel].sum() < hv_frac * area:
                continue
            ring = cv2.dilate(sel.astype(np.uint8), k3).astype(bool) & ~sel
            ba, bb_ = int((LD[ring] == a).sum()), int((LD[ring] == b).sum())
            if min(ba, bb_) < min_border_px:
                continue
            r_, c_ = np.nonzero(sel)
            r_ = r_ + R0
            c_ = c_ + C0
            chans.append({"a": a, "b": b, "rows": r_, "cols": c_, "border": (ba, bb_)})
            union[r_, c_] = True
    return chans, union


def extract(masks: dict, meta: dict, lmax_units=26.0, conn=8, min_channel_area_units=120.0,
            min_border_units=14.0, lmax_pullup_units=64.0, log=print):
    t0 = time.time()
    scale = float(meta["scale"])
    metal = masks["metal"].astype(bool)
    poly = masks["poly"].astype(bool)
    buried = masks.get("buried", np.zeros_like(metal)).astype(bool)
    diff = masks["diff"].astype(bool) | buried
    contacts = masks.get("contacts", np.zeros_like(metal)).astype(bool)
    vss_seed = masks.get("vss_seed")
    vcc_seed = masks.get("vcc_seed")
    st = S8 if conn == 8 else S4

    lmax_px = lmax_units * scale
    labM, nM = ndi.label(metal, st)
    labP, nP = ndi.label(poly, st)
    labD, nD = ndi.label(diff, st)
    vcc_comp = None
    if vcc_seed is not None:
        vcc_comp = np.zeros(nD + 1, bool)
        vcc_comp[np.unique(labD[vcc_seed.astype(bool)])] = True
        vcc_comp[0] = False
    chans, ch = find_channels(diff, poly, lmax_px, min_area_px=max(1, int(min_channel_area_units * scale * scale)),
                              labD=labD, min_border_px=max(1, int(round(min_border_units * scale))),
                              lmax_pullup_px=lmax_pullup_units * scale, vcc_comp=vcc_comp)
    nC = len(chans)
    log(f"  components: metal {nM}, poly {nP}, diff {nD}, channels {nC}  ({time.time()-t0:.1f}s)")
    # global component index: [1..nM] metal, then poly, then diff, then VSS, VCC
    offP, offD = nM, nM + nP
    VSS, VCC = nM + nP + nD + 1, nM + nP + nD + 2
    uf = UF(VCC + 1)

    def gid_metal(l): return l
    def gid_poly(l): return offP + l
    def gid_diff(l): return offD + l

    # contacts
    labK, nK = ndi.label(contacts, S8)
    objs = ndi.find_objects(labK)
    n_mp = n_md = n_float = 0
    for k, sl in enumerate(objs, start=1):
        if sl is None:
            continue
        sel = labK[sl] == k
        ms = labM[sl][sel]
        ms = ms[ms > 0]
        if ms.size == 0:
            n_float += 1
            continue
        mcomp = np.bincount(ms).argmax()
        ps = labP[sl][sel]
        ds = labD[sl][sel]
        frac_p = (ps > 0).mean()
        if frac_p >= 0.5:
            pc = np.bincount(ps[ps > 0]).argmax()
            uf.union(gid_metal(mcomp), gid_poly(pc))
            n_mp += 1
            # a cut on top of a buried contact also reaches the diffusion; poly over any
            # other diffusion is a gate, the cut stops on the poly
            if buried[sl][sel].mean() >= 0.5 and (ds > 0).any():
                uf.union(gid_metal(mcomp), gid_diff(np.bincount(ds[ds > 0]).argmax()))
        elif (ds > 0).any():
            dc = np.bincount(ds[ds > 0]).argmax()
            uf.union(gid_metal(mcomp), gid_diff(dc))
            n_md += 1
        else:
            n_float += 1
    log(f"  contacts: {nK} cuts -> metal-poly {n_mp}, metal-diff {n_md}, landing on nothing {n_float}")

    # buried contacts
    labB, nB = ndi.label(buried & poly, S8)
    for k, sl in enumerate(ndi.find_objects(labB), start=1):
        if sl is None:
            continue
        sel = labB[sl] == k
        ps = labP[sl][sel]
        ds = labD[sl][sel]
        if (ps > 0).any() and (ds > 0).any():
            uf.union(gid_poly(np.bincount(ps[ps > 0]).argmax()), gid_diff(np.bincount(ds[ds > 0]).argmax()))
    # power seeds
    for seed, rail in ((vss_seed, VSS), (vcc_seed, VCC)):
        if seed is None:
            continue
        ds = labD[seed.astype(bool) & (labD > 0)]
        for dc in np.unique(ds):
            uf.union(gid_diff(int(dc)), rail)
    log(f"  union-find done ({time.time()-t0:.1f}s)")

    # resolve roots for all components (vectorised)
    roots = np.array([uf.find(i) for i in range(VCC + 1)], np.int64)
    if roots[VSS] == roots[VCC]:
        log("  WARNING: VSS and VCC are shorted together in the extracted geometry")

    # transistors: one per channel (pair of diffusion components + connected region)
    trans = []
    multi = degenerate = 0
    for chn in chans:
        rr, cc = chn["rows"], chn["cols"]
        ps = labP[rr, cc]
        ps = ps[ps > 0]
        if ps.size == 0:
            continue
        gate = int(roots[gid_poly(int(np.bincount(ps).argmax()))])
        n1 = int(roots[gid_diff(int(chn["a"]))])
        n2 = int(roots[gid_diff(int(chn["b"]))])
        if n1 == n2:
            degenerate += 1
        bb = [cc.min() / scale, (cc.max() + 1) / scale, rr.min() / scale, (rr.max() + 1) / scale]
        trans.append({"gate": gate, "c1": n1, "c2": n2, "bb": bb, "area": float(len(rr)) / (scale * scale),
                      "w": [round(x / scale, 1) for x in chn["border"]]})
    log(f"  channels -> {len(trans)} devices ({degenerate} degenerate c1==c2)"
        f" ({time.time()-t0:.1f}s)")

    # node table: only nodes that appear somewhere (all components)
    vss_r, vcc_r = int(roots[VSS]), int(roots[VCC])
    # pullups (depletion loads)
    pullup = set()
    enh = []
    for t in trans:
        terms = {t["c1"], t["c2"]}
        if vcc_r in terms and t["gate"] in terms and t["gate"] != vcc_r:
            pullup.add(t["gate"])
            continue
        enh.append(t)
    log(f"  depletion pull-ups: {len(pullup)}, enhancement devices: {len(enh)}")

    # renumber nodes compactly: vss=0? keep visual6502-like: give ids in order of first appearance
    used = sorted(set(int(r) for r in roots[1:]))
    newid = {}
    nid = 0
    for r in [vss_r, vcc_r] + used:
        if r not in newid:
            newid[r] = nid
            nid += 1
    # per-node layer areas (pixels)
    node_info = {}
    for name, lab, off, n in (("metal", labM, 0, nM), ("poly", labP, offP, nP), ("diff", labD, offD, nD)):
        if n == 0:
            continue
        areas = np.bincount(lab.ravel(), minlength=n + 1)[1:]
        for i in range(n):
            r = newid[int(roots[off + 1 + i])]
            ni = node_info.setdefault(r, {"layers": {}, "area_px": 0})
            ni["layers"][name] = ni["layers"].get(name, 0) + int(areas[i])
            ni["area_px"] += int(areas[i])
    names = {newid[vss_r]: ["vss"], newid[vcc_r]: ["vcc"]}
    labs = {"metal": (labM, 0), "poly": (labP, offP), "diff": (labD, offD)}
    for nm, a in meta.get("anchors", {}).items():
        lab, off = labs[a.get("layer", "metal")]
        rr, cc = a["row"], a["col"]
        if not (0 <= rr < lab.shape[0] and 0 <= cc < lab.shape[1]):
            continue
        l = lab[rr, cc]
        if l == 0:  # search a small neighbourhood
            win = lab[max(rr - 3, 0):rr + 4, max(cc - 3, 0):cc + 4]
            nz = win[win > 0]
            if nz.size == 0:
                log(f"  anchor {nm}: nothing on {a.get('layer')} at ({rr},{cc})")
                continue
            l = np.bincount(nz).argmax()
        names.setdefault(newid[int(roots[off + int(l)])], []).append(nm)
    out_t = []
    for i, t in enumerate(enh):
        out_t.append({"name": f"t{i}", "gate": newid[t["gate"]], "c1": newid[t["c1"]], "c2": newid[t["c2"]],
                      "bb": [round(v, 1) for v in t["bb"]], "area": round(t["area"], 1), "w": t["w"]})
    nodes = []
    for r, i in sorted(newid.items(), key=lambda kv: kv[1]):
        info = node_info.get(i, {"layers": {}, "area_px": 0})
        nodes.append({"id": i, "names": names.get(i, []), "pullup": r in pullup, **info})
    result = {
        "meta": {"scale": scale, "lmax_units": lmax_units, "conn": conn,
                 "components": {"metal": nM, "poly": nP, "diff": nD, "channels": nC, "contacts": nK},
                 "contacts_mp": n_mp, "contacts_md": n_md, "contacts_floating": n_float,
                 "degenerate": degenerate,
                 "vss_vcc_short": bool(vss_r == vcc_r), "seconds": round(time.time() - t0, 1)},
        "nodes": nodes, "transistors": out_t,
    }
    aux = {"labM": labM, "labP": labP, "labD": labD, "chans": chans, "roots": roots, "newid": newid,
           "offP": offP, "offD": offD, "channels": ch}
    return result, aux


def node_raster(aux, layer):
    """int32 raster of extracted node ids (+1; 0 = empty) for one layer."""
    lab = {"metal": aux["labM"], "poly": aux["labP"], "diff": aux["labD"]}[layer]
    off = {"metal": 0, "poly": aux["offP"], "diff": aux["offD"]}[layer]
    n = int(lab.max())
    lut = np.zeros(n + 1, np.int32)
    for i in range(1, n + 1):
        lut[i] = aux["newid"][int(aux["roots"][off + i])] + 1
    return lut[lab]


def write_netlist(result, outdir, aux=None, segdefs=False, ymax_units=None):
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "netlist.json"), "w") as f:
        json.dump(result, f)
    # visual6502-format transdefs (bbox in chip units; y converted back to y-up if ymax given)
    with open(os.path.join(outdir, "transdefs.js"), "w") as f:
        f.write("// extracted by extract.py -- visual6502 transdefs format\n")
        f.write("// ['name', gate, c1, c2, [xmin, xmax, ymin, ymax], [w1, w2, len, 0, area]]\n")
        f.write("var transdefs = [\n")
        for t in result["transistors"]:
            x0, x1, r0, r1 = t["bb"]
            if ymax_units is not None:
                y0, y1 = ymax_units - r1, ymax_units - r0
            else:
                y0, y1 = r0, r1
            f.write(f"['{t['name']}', {t['gate']}, {t['c1']}, {t['c2']}, "
                    f"[{int(x0)}, {int(x1)}, {int(y0)}, {int(y1)}],[{int(t['w'][0])}, {int(t['w'][1])}, "
                    f"{int(round(2 * t['area'] / max(t['w'][0] + t['w'][1], 1)))}, 0, {int(t['area'])}] ],\n")
        f.write("]\n")
    with open(os.path.join(outdir, "nodenames.js"), "w") as f:
        f.write("var nodenames ={\n")
        for n in result["nodes"]:
            for nm in n["names"]:
                f.write(f"{nm}: {n['id']},\n")
        f.write("}\n")
    if segdefs and aux is not None:
        write_segdefs(result, aux, os.path.join(outdir, "segdefs.js"), ymax_units)


def write_segdefs(result, aux, path, ymax_units=None):
    import cv2
    scale = result["meta"]["scale"]
    pull = {n["id"] for n in result["nodes"] if n["pullup"]}
    layer_idx = {"metal": 0, "diff": 1, "poly": 5}
    with open(path, "w") as f:
        f.write("// extracted by extract.py -- visual6502 segdefs format [node, pullup, layer, x0,y0,...]\n")
        f.write("var segdefs = [\n")
        for layer in ("diff", "poly", "metal"):
            ras = node_raster(aux, layer)
            for sl_i, sl in enumerate(ndi.find_objects(ras), start=1):
                if sl is None:
                    continue
                m = (ras[sl] == sl_i).astype(np.uint8)
                cs, _ = cv2.findContours(m, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
                for c in cs:
                    c = cv2.approxPolyDP(c, 0.7, True).reshape(-1, 2)
                    if len(c) < 3:
                        continue
                    xs = (c[:, 0] + sl[1].start) / scale
                    ys = (c[:, 1] + sl[0].start) / scale
                    if ymax_units is not None:
                        ys = ymax_units - ys
                    coords = ",".join(f"{int(round(x))},{int(round(y))}" for x, y in zip(xs, ys))
                    nid = sl_i - 1
                    f.write(f"[{nid:4d},'{'+' if nid in pull else '-'}',{layer_idx[layer]},{coords}],\n")
        f.write("]\n")


def load_masks(path):
    d = np.load(path)
    masks = {k: d[k] for k in d.files}
    with open(os.path.splitext(path)[0] + ".json") as f:
        meta = json.load(f)
    return masks, meta


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--masks", required=True, help="npz with metal/poly/diff/contacts/buried[/vss_seed/vcc_seed]")
    ap.add_argument("--out", required=True)
    ap.add_argument("--lmax", type=float, default=26.0, help="max gate length in chip units")
    ap.add_argument("--lmax-pullup", type=float, default=64.0,
                    help="max channel length when one side is a VCC diffusion (depletion loads)")
    ap.add_argument("--conn", type=int, default=8, choices=(4, 8))
    ap.add_argument("--segdefs", action="store_true")
    ap.add_argument("--ymax", type=float, default=None, help="flip y back to y-up chip coords (6502: 10000)")
    a = ap.parse_args(argv)
    masks, meta = load_masks(a.masks)
    res, aux = extract(masks, meta, lmax_units=a.lmax, conn=a.conn, lmax_pullup_units=a.lmax_pullup)
    write_netlist(res, a.out, aux, segdefs=a.segdefs, ymax_units=a.ymax)
    print(json.dumps(res["meta"]))


if __name__ == "__main__":
    sys.exit(main())
