"""Compare emulator frames with a reference screenshot at the C64 colour-index level.

usage: python3 -I cmpshot.py REF.png FRAMES_DIR [--search R] [--step N] [--align DX,DY] [--diff OUT.png]

Both images are mapped to the nearest of the 16 C64 colours (Pepto PAL palette), so
palette differences between emulators don't count. The best alignment (dx, dy) is
searched once on a coarse grid of frames, then every frame is scored at that alignment.
Reports the best-matching frames and writes a side-by-side diff image of the best one.
"""
import os
import struct
import sys
import zlib

PEPTO = [0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2, 0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
         0x6F4F25, 0x433900, 0x9A6759, 0x444444, 0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595]
PAL_RGB = [((c >> 16) & 255, (c >> 8) & 255, c & 255) for c in PEPTO]
_nearest = {}


def nearest(rgb):
    i = _nearest.get(rgb)
    if i is None:
        r, g, b = rgb
        i = min(range(16), key=lambda k: (PAL_RGB[k][0] - r) ** 2 + (PAL_RGB[k][1] - g) ** 2 + (PAL_RGB[k][2] - b) ** 2)
        _nearest[rgb] = i
    return i


def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', path + ': not a PNG'
    p, idat, plte = 8, [], None
    while p < len(d):
        n = struct.unpack('>I', d[p:p + 4])[0]
        t, c = d[p + 4:p + 8], d[p + 8:p + 8 + n]
        p += 12 + n
        if t == b'IHDR':
            w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', c)
        elif t == b'PLTE':
            plte = c
        elif t == b'IDAT':
            idat.append(c)
        elif t == b'IEND':
            break
    assert bd == 8 and il == 0, path + ': only 8-bit non-interlaced PNGs'
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    prev = bytearray(stride)
    rows = []
    i = 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if f == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif f == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif f == 3:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    idx = bytearray(w * h)
    for y, line in enumerate(rows):
        for x in range(w):
            if ct == 3:
                k = line[x] * 3
                rgb = (plte[k], plte[k + 1], plte[k + 2])
            elif ct in (0, 4):
                v = line[x * bpp]
                rgb = (v, v, v)
            else:
                k = x * bpp
                rgb = (line[k], line[k + 1], line[k + 2])
            idx[y * w + x] = nearest(rgb)
    return w, h, bytes(idx)


def score(ref, rw, rh, frm, fw, fh, dx, dy, step=1):
    """Fraction of reference pixels whose colour index equals frame pixel at (x+dx, y+dy)."""
    hit = tot = 0
    for y in range(0, rh, step):
        fy = y + dy
        if fy < 0 or fy >= fh:
            tot += (rw + step - 1) // step
            continue
        for x in range(0, rw, step):
            fx = x + dx
            tot += 1
            if 0 <= fx < fw and ref[y * rw + x] == frm[fy * fw + fx]:
                hit += 1
    return hit / tot


def write_png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ch = lambda t, x: struct.pack('>I', len(x)) + t + x + struct.pack('>I', zlib.crc32(t + x) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))


def main():
    args = sys.argv[1:]
    opt = {'--search': '40', '--step': '1', '--diff': None, '--align': None}
    pos = []
    while args:
        a = args.pop(0)
        if a in opt:
            opt[a] = args.pop(0)
        else:
            pos.append(a)
    ref_path, frames_dir = pos
    search, step = int(opt['--search']), int(opt['--step'])
    rw, rh, ref = read_png(ref_path)
    names = sorted(n for n in os.listdir(frames_dir) if n.lower().endswith('.png'))[::step]
    print(f'reference {rw}x{rh}; {len(names)} frames to score')

    if opt['--align']:
        dx, dy = (int(v) for v in opt['--align'].split(','))
    else:
        # 1. Find the alignment on a coarse subset of frames with a sparse pixel grid.
        probe = names[::max(1, len(names) // 60)]
        best = (-1, 0, 0, None)
        for n in probe:
            fw, fh, frm = read_png(os.path.join(frames_dir, n))
            for dy in range(-search, search + 1, 2):
                for dx in range(-search, search + 1, 2):
                    s = score(ref, rw, rh, frm, fw, fh, dx, dy, step=4)
                    if s > best[0]:
                        best = (s, dx, dy, n)
        _, bdx, bdy, bn = best
        fw, fh, frm = read_png(os.path.join(frames_dir, bn))
        best = max((score(ref, rw, rh, frm, fw, fh, dx, dy, step=2), dx, dy)
                   for dy in range(bdy - 2, bdy + 3) for dx in range(bdx - 2, bdx + 3))
        _, dx, dy = best
    print(f'alignment: frame pixel = reference pixel + ({dx}, {dy})')

    # 2. Score every frame at that alignment (sparse screen, then exact for the top ones).
    coarse = []
    for n in names:
        fw, fh, frm = read_png(os.path.join(frames_dir, n))
        coarse.append((score(ref, rw, rh, frm, fw, fh, dx, dy, step=3), n))
    coarse.sort(reverse=True)
    exact = []
    for _, n in coarse[:8]:
        fw, fh, frm = read_png(os.path.join(frames_dir, n))
        exact.append((score(ref, rw, rh, frm, fw, fh, dx, dy), n))
    exact.sort(reverse=True)
    for s, n in exact:
        print(f'{n}: {100 * s:.3f}% of reference pixels match')

    if opt['--diff']:
        s, n = exact[0]
        fw, fh, frm = read_png(os.path.join(frames_dir, n))
        out = bytearray()
        for y in range(rh):
            a, b, c = bytearray(), bytearray(), bytearray()
            for x in range(rw):
                r = ref[y * rw + x]
                fx, fy = x + dx, y + dy
                f = frm[fy * fw + fx] if 0 <= fx < fw and 0 <= fy < fh else 0
                a += bytes(PAL_RGB[r])
                b += bytes(PAL_RGB[f])
                c += b'\x00\x00\x00' if r == f else b'\xff\x30\x30'
            out += a + b + c
        write_png(opt['--diff'], rw * 3, rh, bytes(out))
        print(f'wrote {opt["--diff"]} (reference | ours: {n} | mismatches in red)')


if __name__ == '__main__':
    main()
