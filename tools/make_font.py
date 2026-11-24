"""
Builds src/font_data.h: one 8-bit alpha atlas (used on PSP as a T8 texture with an
alpha CLUT) containing every font size, the PlayStation button glyphs and the shape
textures (anti-aliased disk, ring, soft blob) used for rounded rects, rings and shadows.

usage: python make_font.py <folder with SFProText-*.ttf> <output .h> [preview.png]
"""
import sys, os, math
import numpy as np
from PIL import Image, ImageDraw, ImageFont

FONT_DIR, OUT_H = sys.argv[1], sys.argv[2]
PREVIEW = sys.argv[3] if len(sys.argv) > 3 else None
ATLAS_W = 512

# id, file, pixel size, charset
ASCII = [chr(c) for c in range(32, 127)]
CYR = [chr(c) for c in range(0x410, 0x450)] + ["Ё", "ё"]
EXTRA = list("•…—«»№°→←↑↓‹›✕")
PS = ["", "", "", ""]  # cross circle triangle square
CHARS = ASCII + CYR + EXTRA + PS

FONTS = [
    ("F_SMALL",   "SFProText-Regular.ttf",  10, CHARS),
    ("F_BODY",    "SFProText-Regular.ttf",  12, CHARS),
    ("F_LABEL",   "SFProText-Semibold.ttf", 13, CHARS),
    ("F_TITLE",   "SFProText-Semibold.ttf", 20, CHARS),
    ("F_DISPLAY", "SFProText-Bold.ttf",     30, CHARS),
    ("F_LOGO",    "SFProText-Heavy.ttf",    52, list(" GHPSL0123456789-.")),
]


def ps_glyph(ch, px):
    """Anti-aliased PlayStation button symbol, 4x supersampled."""
    s = max(6, round(px * 0.66))
    S = s * 4
    im = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(im)
    lw = max(4, round(S * 0.13))
    m = lw // 2 + 1
    if ch == "":
        d.line([(m, m), (S - m, S - m)], fill=255, width=lw)
        d.line([(S - m, m), (m, S - m)], fill=255, width=lw)
    elif ch == "":
        d.ellipse([m, m, S - m, S - m], outline=255, width=lw)
    elif ch == "":
        d.polygon([(S / 2, m), (S - m, S - m * 1.2), (m, S - m * 1.2)], outline=255, width=lw)
    else:
        d.rectangle([m, m, S - m, S - m], outline=255, width=lw)
    return np.array(im.resize((s, s), Image.LANCZOS))


def shapes():
    out = {}
    def field(n, fn):
        a = np.zeros((n, n), np.float32)
        c = n / 2
        for y in range(n):
            for x in range(n):
                a[y, x] = fn(math.hypot(x + 0.5 - c, y + 0.5 - c), c)
        return (np.clip(a, 0, 1) * 255).astype(np.uint8)
    out["disk64"] = field(64, lambda d, r: r - d + 0.5)
    out["ring64"] = field(64, lambda d, r: min(r - d + 0.5, d - (r - 6) + 0.5))
    out["blob64"] = field(64, lambda d, r: (lambda t: t * t * (3 - 2 * t))(max(0.0, 1 - d / r)))
    out["disk16"] = field(16, lambda d, r: r - d + 0.5)
    # inverse rounded corner (top-left), used to round image corners on opaque panels
    c = np.zeros((32, 32), np.float32)
    for y in range(32):
        for x in range(32):
            c[y, x] = math.hypot(x + 0.5 - 32, y + 0.5 - 32) - 32 + 0.5
    out["corner32"] = (np.clip(c, 0, 1) * 255).astype(np.uint8)
    return out


def main():
    atlas = np.zeros((1024, ATLAS_W), np.uint8)
    sh = shapes()
    shape_pos = {"disk64": (0, 0), "ring64": (64, 0), "blob64": (128, 0), "disk16": (192, 0), "corner32": (208, 0)}
    for k, (x, y) in shape_pos.items():
        a = sh[k]
        atlas[y:y + a.shape[0], x:x + a.shape[1]] = a
    # shelf packer below the shapes
    cx, cy, shelf = 0, 66, 0

    def place(w, h):
        nonlocal cx, cy, shelf
        if cx + w + 1 > ATLAS_W:
            cx, cy, shelf = 0, cy + shelf + 1, 0
        pos = (cx, cy)
        cx += w + 1
        shelf = max(shelf, h)
        return pos

    fonts_out = []
    pending = []  # (font index, char, bitmap, ox, oy, adv) packed tallest-first
    for fid, fname, px, charset in FONTS:
        font = ImageFont.truetype(os.path.join(FONT_DIR, fname), px)
        asc, desc = font.getmetrics()
        cap = -font.getbbox("H", anchor="ls")[1]
        glyphs = {}
        for ch in CHARS:
            if ch not in charset:
                glyphs[ch] = (0, 0, 0, 0, 0, 0, 0)
                continue
            if ch in PS:
                a = ps_glyph(ch, px)
                h, w = a.shape
                ox, oy = 1, -(cap + h) // 2
                adv = w + 2
            else:
                l, t, r, b = font.getbbox(ch, anchor="ls")
                adv = round(font.getlength(ch))
                w, h = r - l, b - t
                if w <= 0 or h <= 0:
                    glyphs[ch] = (0, 0, 0, 0, 0, 0, adv)
                    continue
                im = Image.new("L", (w, h), 0)
                ImageDraw.Draw(im).text((-l, -t), ch, fill=255, font=font, anchor="ls")
                a = np.array(im)
                ox, oy = l, t
            pending.append((len(fonts_out), ch, a, ox, oy, adv))
        fonts_out.append((fid, asc, desc, cap, glyphs))

    pending.sort(key=lambda p: -p[2].shape[0])
    for fi, ch, a, ox, oy, adv in pending:
        h, w = a.shape
        x, y = place(w, h)
        atlas[y:y + h, x:x + w] = a
        fonts_out[fi][4][ch] = (x, y, w, h, ox, oy, adv)

    used_h = cy + shelf + 1
    atlas_h = 1 << math.ceil(math.log2(used_h))
    if atlas_h > 512:
        raise SystemExit(f"atlas too tall: {used_h}")
    atlas = atlas[:atlas_h]

    with open(OUT_H, "w", encoding="ascii") as f:
        f.write("/* generated by tools/make_font.py - do not edit */\n#pragma once\n\n")
        f.write(f"#define ATLAS_W {ATLAS_W}\n#define ATLAS_H {atlas_h}\n")
        f.write(f"#define NGLYPHS {len(CHARS)}\n#define NFONTS {len(FONTS)}\n")
        for i, fo in enumerate(fonts_out):
            f.write(f"#define {fo[0]} {i}\n")
        for k, (x, y) in shape_pos.items():
            f.write(f"#define SHAPE_{k.upper()}_X {x}\n#define SHAPE_{k.upper()}_Y {y}\n")
        f.write("\ntypedef struct { unsigned short x, y; unsigned char w, h; signed char ox, oy; unsigned char adv; } GlyphInfo;\n")
        f.write("typedef struct { int ascent, descent, cap; } FontMetrics;\n\n")
        f.write("static const unsigned short glyph_cps[NGLYPHS] = {")
        f.write(",".join(str(ord(c)) for c in CHARS))
        f.write("};\n\nstatic const FontMetrics font_metrics[NFONTS] = {")
        f.write(",".join(f"{{{a},{d},{c}}}" for _, a, d, c, _ in fonts_out))
        f.write("};\n\nstatic const GlyphInfo font_glyphs[NFONTS][NGLYPHS] = {\n")
        for _, _, _, _, g in fonts_out:
            f.write(" {" + ",".join("{%d,%d,%d,%d,%d,%d,%d}" % g[c] for c in CHARS) + "},\n")
        f.write("};\n\nstatic const unsigned char atlas_data[ATLAS_W * ATLAS_H] __attribute__((aligned(16))) = {\n")
        flat = atlas.flatten()
        for i in range(0, len(flat), 64):
            f.write(",".join(str(v) for v in flat[i:i + 64]) + ",\n")
        f.write("};\n")
    if PREVIEW:
        Image.fromarray(atlas).save(PREVIEW)
    print(f"atlas {ATLAS_W}x{atlas_h} (used {used_h}), glyphs {len(CHARS)} x {len(FONTS)} fonts")


main()
