"""Small rendering helpers shared by the stages (colour layer composites, overlays)."""
from __future__ import annotations

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

COLORS = {
    "diff": (230, 200, 40),
    "poly": (220, 40, 40),
    "metal": (120, 160, 255),
    "contact": (0, 0, 0),
    "buried": (0, 200, 0),
    "channel": (255, 0, 255),
}


def compose(layers: dict, alpha_metal=0.45, base=None):
    """Composite boolean layer masks into an RGB uint8 image."""
    any_mask = next(iter(layers.values()))
    h, w = any_mask.shape
    img = np.zeros((h, w, 3), np.float32) if base is None else base.astype(np.float32).copy()
    if base is None:
        img[:] = 30
    for name, a in (("diff", 1.0), ("poly", 0.85), ("channel", 1.0), ("metal", alpha_metal),
                    ("buried", 1.0), ("contact", 1.0)):
        m = layers.get(name)
        if m is None:
            continue
        c = np.array(COLORS[name], np.float32)
        img[m] = img[m] * (1 - a) + c * a
    return np.clip(img, 0, 255).astype(np.uint8)


def layer_png(layers: dict, path: str, max_w=4000, **kw):
    img = compose(layers, **kw)
    save_rgb(img, path, max_w)


def save_rgb(img, path, max_w=4000):
    im = Image.fromarray(img)
    if im.width > max_w:
        im = im.resize((max_w, int(im.height * max_w / im.width)), Image.LANCZOS)
    im.save(path, optimize=True)


def outline(mask):
    import cv2
    k = np.ones((3, 3), np.uint8)
    m = mask.astype(np.uint8)
    return (cv2.dilate(m, k) - cv2.erode(m, k)).astype(bool)


def overlay_outlines(rgb, layers: dict, colors=None):
    """Draw coloured outlines of masks over an RGB image."""
    colors = colors or {}
    out = rgb.copy()
    for name, m in layers.items():
        c = colors.get(name, COLORS.get(name, (255, 255, 255)))
        out[outline(m)] = c
    return out
