# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the dedication card BMP from a photo and the player's own disc.

The name uses the game's fight-screen capitals, read from the disc when the
card is built, so no game bytes are committed. Composition needs Pillow.
"""

import hashlib
from pathlib import Path
import struct

# SHA-256 of the decoded RGBA capitals texture (512x256, 8-bit paletted).
FONT_SHA256 = "da6b43697b1baa3dbc97e801d86469af2071c20a2ef0a7912cc45e4c976760e7"
FONT_ROWS = ("ABCDEFGHIJ", "KLMNOPQRS", "TUVWXYZ")
WIDTH, HEIGHT, S = 3840, 2160, 3  # S: card units are 1280x720


def morton_tables(width, height):
    """Per-axis offsets of Xbox swizzled texels: offset = xs[x] | ys[y]."""
    xbits, ybits, bit, sx, sy = [], [], 0, 1, 1
    while sx < width or sy < height:
        if sx < width:
            xbits.append(bit); bit += 1; sx <<= 1
        if sy < height:
            ybits.append(bit); bit += 1; sy <<= 1
    spread = lambda n, bits: sum(((n >> i) & 1) << b for i, b in enumerate(bits))
    return [spread(x, xbits) for x in range(width)], [spread(y, ybits) for y in range(height)]


def paletted_textures(blob):
    """Yield (width, height, rgba) for each 8-bit paletted texture in an XPR0
    bundle, paired with the next palette resource (else the previous one)."""
    if blob[:4] != b"XPR0" or len(blob) < 12:
        return
    header = struct.unpack_from("<I", blob, 8)[0]
    resources, p = [], 12
    while p + 12 <= min(header, len(blob)):
        common = struct.unpack_from("<I", blob, p)[0]
        if common == 0xFFFFFFFF:
            break
        kind = (common >> 16) & 7
        if kind == 4 and p + 20 <= header:
            resources.append(("texture", struct.unpack_from("<5I", blob, p)))
            p += 20
            continue
        if kind == 3:
            count = 256 >> ((common >> 30) & 3) if (common >> 30) & 3 else 256
            resources.append(("palette", (struct.unpack_from("<I", blob, p + 4)[0], count)))
        p += 12
    for i, (kind, fields) in enumerate(resources):
        _, data, _, fmt, size = fields if kind == "texture" else (0,) * 5
        # Explicitly sized textures are linear; only swizzled power-of-two ones are read.
        if kind != "texture" or (fmt >> 8) & 0xFF != 0x0B or size:
            continue
        palette = next((r[1] for r in resources[i + 1:] if r[0] == "palette"), None) or \
            next((r[1] for r in reversed(resources[:i]) if r[0] == "palette"), None)
        if palette is None:
            continue
        width, height = 1 << ((fmt >> 20) & 15), 1 << ((fmt >> 24) & 15)
        texels = blob[header + data: header + data + width * height]
        colors = blob[header + palette[0]: header + palette[0] + palette[1] * 4]
        if len(texels) != width * height or len(colors) != palette[1] * 4:
            continue
        lut = [bytes((colors[j + 2], colors[j + 1], colors[j], colors[j + 3]))
               for j in range(0, len(colors), 4)] + [bytes(4)] * (256 - palette[1])
        xs, ys = morton_tables(width, height)
        yield width, height, b"".join(lut[texels[xs[x] | ys[y]]]
                                      for y in range(height) for x in range(width))


def afs_xpr_blobs(path):
    """Yield every XPR0 entry of an AFS archive."""
    with open(path, "rb") as stream:
        magic, count = struct.unpack("<4sI", stream.read(8))
        if magic != b"AFS\0" or count > 65536:
            return
        toc = [struct.unpack("<II", stream.read(8)) for _ in range(count)]
        for offset, size in toc:
            stream.seek(offset)
            if size >= 12 and stream.read(4) == b"XPR0":
                stream.seek(offset)
                yield stream.read(size)


def find_font(disc_root, digest=FONT_SHA256):
    for archive in sorted(Path(disc_root).rglob("*.afs")):
        for blob in afs_xpr_blobs(archive):
            for width, height, rgba in paletted_textures(blob):
                if (width, height) == (512, 256) and hashlib.sha256(rgba).hexdigest() == digest:
                    return rgba
    return None


def build(disc_root, photo, output):
    from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont, ImageOps

    rgba = find_font(disc_root)
    if rgba is None:
        raise ValueError("the disc has no fight-screen capitals texture")
    atlas = Image.frombytes("RGBA", (512, 256), rgba)
    card = ImageOps.fit(Image.open(photo).convert("RGB"), (WIDTH, HEIGHT), Image.LANCZOS)

    # Darken toward the empty right side so text reads; fade top and bottom.
    smooth = lambda t: t * t * (3 - 2 * t)
    across = Image.new("L", (WIDTH, 1))
    across.putdata([round(255 * 0.75 * smooth(min(max((x / S - 760) / 300, 0), 1)))
                    for x in range(WIDTH)])
    down = Image.new("L", (1, HEIGHT))
    down.putdata([round(255 * 0.5 * (max(abs(y / S - 360) / 360 - 0.55, 0) / 0.45) ** 2)
                  for y in range(HEIGHT)])
    shade = ImageChops.add(ImageChops.add(across.resize((WIDTH, HEIGHT)), down.resize((WIDTH, HEIGHT))),
                           Image.new("L", (WIDTH, HEIGHT), 38))
    card = Image.composite(Image.new("RGB", card.size), card, shade)

    alpha = atlas.getchannel("A").load()
    def runs(values):
        spans, start = [], None
        for i, value in enumerate(values + [0]):
            if value and start is None:
                start = i
            elif not value and start is not None:
                spans.append((start, i)); start = None
        return spans
    glyphs = {}
    bands = runs([sum(alpha[x, y] > 5 for x in range(512)) for y in range(256)])
    for (y0, y1), row in zip(bands, FONT_ROWS):
        spans = runs([sum(alpha[x, y] > 5 for y in range(y0, y1)) for x in range(512)])
        if len(spans) != len(row):
            raise ValueError("unexpected capitals layout")
        for (x0, x1), letter in zip(spans, row):
            glyphs[letter] = atlas.crop((x0, y0, x1, y1))
    band = max(g.height for g in glyphs.values())
    cx = 1070 * S

    def caps(text, cap_px, top):
        widths = [band // 3 if c == " " else glyphs[c].width - 1 for c in text]
        line = Image.new("RGBA", (sum(widths) + 1, band), (0, 0, 0, 0))
        x = 0
        for c, w in zip(text, widths):
            if c != " ":
                line.alpha_composite(glyphs[c], (x, band - glyphs[c].height))
            x += w
        k = cap_px / band
        line = line.resize((round(line.width * k), round(band * k)), Image.LANCZOS)
        # Re-sharpen edges the enlargement softened.
        mask = line.getchannel("A").point(
            lambda a: round(255 * smooth(min(max((a / 255 - 0.3) / 0.4, 0), 1))))
        fill = line.convert("RGB").point(lambda v: v * 245 // 255)
        x, y = int(cx - line.width / 2), top * S
        shadow = mask.filter(ImageFilter.GaussianBlur(cap_px * 0.06)).point(lambda a: a * 7 // 10)
        drop = int(cap_px * 0.05)
        card.paste((0, 0, 0), (x + drop, y + drop, x + drop + line.width, y + drop + line.height), shadow)
        card.paste(fill, (x, y), mask)

    top = 262
    caps("TOMONOBU", 118, top + 48)
    caps("ITAGAKI", 118, top + 108)
    draw = ImageDraw.Draw(card)
    font = lambda size, name="georgia.ttf": ImageFont.truetype(name, size * S)
    def center(text, face, y, fill):
        draw.text((cx - draw.textlength(text, font=face) / 2, y * S), text, font=face, fill=fill)
    center("In Memory of", font(28), top, (175, 175, 175))
    draw.line((cx - 60 * S, (top + 168) * S, cx + 60 * S, (top + 168) * S), fill=(120, 30, 30), width=2 * S)
    center("1967 \u2013 2025", font(28), top + 182, (175, 175, 175))
    center("Creator of Dead or Alive", font(24, "georgiai.ttf"), top + 228, (150, 150, 150))
    partial = Path(output).with_suffix(".tmp")
    card.save(partial, "BMP")
    partial.replace(output)
