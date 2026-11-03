#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Logo di Heptazip: genera SVG, PNG e ICO dalla geometria (docs/logo/).

    make_logo.py <Outfit-Regular.ttf> <dir_uscita> [IBMPlexMono-Regular.ttf]

Il segno: un ettagono (hepta = sette) e sette lame che convergono verso un nucleo, come
un diaframma: i dati raccolti in un centro denso, sette flussi che lavorano insieme. La
geometria è calcolata (vertici esatti, tangenze), non disegnata a mano. Sotto i 48 px il
segno è ridisegnato con meno dettagli (le lame sparirebbero).

Richiede: shapely, fonttools, cairosvg, Pillow (nel venv di ~/devtools).
"""

import io
import math
import os
import sys

INK = "#0F1D2E"       # inchiostro profondo
LIGHT = "#F4F1EA"     # carta (fondo chiaro)
CORE = "#2EC4B6"      # nucleo: la sola nota di luce
N = 7


def poly(cx, cy, r, rot, n=N):
    """vertici di un poligono regolare (rot in radianti, 0 = un vertice in alto)"""
    return [(cx + r * math.sin(rot + 2 * math.pi * i / n), cy - r * math.cos(rot + 2 * math.pi * i / n))
            for i in range(n)]


def fmt(pts):
    return " ".join("%.3f,%.3f" % p for p in pts)




PALETTES = {
    # lame, nucleo, scritta
    "light": ("#0F1D2E", CORE, "#0F1D2E"),   # su fondo chiaro
    "dark": ("#ECE8DF", CORE, "#ECE8DF"),    # su fondo scuro
}


def rounded_poly(pts, radius, steps=24):
    """punti di un poligono con gli angoli arrotondati (archi campionati), per shapely"""
    n = len(pts)
    out = []
    for i in range(n):
        p0, p1, p2 = pts[i - 1], pts[i], pts[(i + 1) % n]
        v1 = (p0[0] - p1[0], p0[1] - p1[1])
        v2 = (p2[0] - p1[0], p2[1] - p1[1])
        l1, l2 = math.hypot(*v1), math.hypot(*v2)
        u1, u2 = (v1[0] / l1, v1[1] / l1), (v2[0] / l2, v2[1] / l2)
        ang = math.acos(u1[0] * u2[0] + u1[1] * u2[1])
        t = radius / math.tan(ang / 2)
        # centro dell'arco sulla bisettrice
        bis = (u1[0] + u2[0], u1[1] + u2[1])
        bl = math.hypot(*bis)
        dist = radius / math.sin(ang / 2)
        cx, cy = p1[0] + bis[0] / bl * dist, p1[1] + bis[1] / bl * dist
        a = (p1[0] + u1[0] * t, p1[1] + u1[1] * t)
        b = (p1[0] + u2[0] * t, p1[1] + u2[1] * t)
        a0 = math.atan2(a[1] - cy, a[0] - cx)
        a1 = math.atan2(b[1] - cy, b[0] - cx)
        da = (a1 - a0 + math.pi) % (2 * math.pi) - math.pi
        for k in range(steps + 1):
            th = a0 + da * k / steps
            out.append((cx + radius * math.cos(th), cy + radius * math.sin(th)))
    return out


def geom_path(geom):
    """tracciato SVG di un (Multi)Polygon di shapely, con i buchi (evenodd)"""
    polys = getattr(geom, "geoms", [geom])
    d = []
    for p in polys:
        for ring in [p.exterior] + list(p.interiors):
            pts = list(ring.coords)[:-1]
            d.append("M" + " L".join("%.3f,%.3f" % q for q in pts) + " Z")
    return " ".join(d)


def mark_shapes(size, detail=True, x0=0.0, y0=0.0):
    """(lame, nucleo) come geometrie di shapely: le lame sono la sagoma meno le fenditure
    e meno l'apertura; niente maschere né ritagli (si vedono uguali ovunque)"""
    from shapely.geometry import LineString, Polygon
    from shapely.ops import unary_union
    c = size / 2
    R = size * 0.48                       # raggio dell'ettagono esterno
    rc = R * (0.34 if detail else 0.40)   # raggio del nucleo
    gap = R * 0.050                       # larghezza delle fenditure
    outer = Polygon(rounded_poly(poly(x0 + c, y0 + c, R, 0), R * 0.10))
    core_pts = poly(x0 + c, y0 + c, rc, math.pi / N)   # ruotato di mezzo passo rispetto al bordo
    core = Polygon(rounded_poly(core_pts, rc * 0.08))
    if not detail:
        return outer.difference(core), Polygon(rounded_poly(poly(x0 + c, y0 + c, rc * 0.92, math.pi / N), rc * 0.12))
    # sette fenditure: ognuna prolunga un lato del nucleo (tangente all'apertura, come le
    # lame di un diaframma) fino al bordo
    cuts = []
    for i in range(N):
        a, b = core_pts[(i + 1) % N], core_pts[i]
        dx, dy = b[0] - a[0], b[1] - a[1]
        k = (2.2 * R) / math.hypot(dx, dy)
        cuts.append(LineString([a, (b[0] + dx * k, b[1] + dy * k)]).buffer(gap / 2, cap_style=2))
    blades = outer.difference(unary_union(cuts)).difference(core.buffer(gap * 0.9, join_style=2))
    return blades, core


def mark_svg_body(size, detail=True, palette="light", x0=0.0, y0=0.0):
    """il segno in un riquadro size x size con l'angolo in (x0, y0), fondo trasparente.
    detail=False: versione per le icone piccole (senza fenditure)"""
    blade, core_col, _ = PALETTES[palette]
    blades, core = mark_shapes(size, detail, x0, y0)
    return ('<path d="%s" fill="%s" fill-rule="evenodd"/>\n  <path d="%s" fill="%s"/>'
            % (geom_path(blades), blade, geom_path(core), core_col))


def text_path(font_path, text, x, baseline, size, tracking=0.0):
    """il testo come tracciato SVG (non dipende dai font installati); ritorna (d, larghezza)"""
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.ttLib import TTFont
    font = TTFont(font_path)
    gs = font.getGlyphSet()
    cmap = font.getBestCmap()
    scale = size / font["head"].unitsPerEm
    pen = SVGPathPen(gs)
    cx = x
    for ch in text:
        g = cmap[ord(ch)]
        tp = TransformPen(pen, (scale, 0, 0, -scale, cx, baseline))
        gs[g].draw(tp)
        cx += gs[g].width * scale + tracking * size
    return pen.getCommands(), cx - x - tracking * size


def x_height(font_path, size):
    from fontTools.ttLib import TTFont
    f = TTFont(font_path)
    return f["OS/2"].sxHeight * size / f["head"].unitsPerEm


def logo_svg(font_path, palette="light", mark=256):
    """segno + scritta in orizzontale; la x-height della scritta è metà del segno"""
    _, _, word_col = PALETTES[palette]
    size = mark * 0.62                          # corpo del testo
    xh = x_height(font_path, size)
    gap = mark * 0.20
    baseline = mark / 2 + xh / 2                # la x-height centrata sul segno
    d, w = text_path(font_path, "heptazip", mark + gap, baseline, size, tracking=-0.01)
    width = mark + gap + w + mark * 0.04
    body = mark_svg_body(mark, True, palette)
    body += '\n  <path d="%s" fill="%s"/>' % (d, word_col)
    return width, mark, body


def svg(width, height, body):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%g" height="%g" viewBox="0 0 %g %g">\n  %s\n</svg>\n'
            % (width, height, width, height, body))


def sheet_svg(font_word, font_mono):
    """tavola di presentazione: il segno con la sua costruzione, colori, misure"""
    W, H = 2000, 2600
    paper, ink, faint = "#F4F1EA", "#0F1D2E", "#9AA3AD"
    out = ['<rect width="%d" height="%d" fill="%s"/>' % (W, H, paper)]

    def mono(text, x, y, size, color=faint, anchor="start", tracking=0.06):
        d, w = text_path(font_mono, text, 0, 0, size, tracking)
        if anchor == "end":
            x -= w
        elif anchor == "middle":
            x -= w / 2
        out.append('<path transform="translate(%.2f,%.2f)" d="%s" fill="%s"/>' % (x, y, d, color))

    # il segno, grande, con la costruzione
    M, mx, my = 1100, (W - 1100) / 2, 330
    c = (mx + M / 2, my + M / 2)
    R = M * 0.48
    rc = R * 0.34
    out.append('<g fill="none" stroke="%s" stroke-width="1.6">' % faint)
    out.append('<circle cx="%.2f" cy="%.2f" r="%.2f" stroke-dasharray="3 9"/>' % (c[0], c[1], R))
    out.append('<circle cx="%.2f" cy="%.2f" r="%.2f" stroke-dasharray="3 9"/>' % (c[0], c[1], rc))
    for i in range(N):
        vx, vy = poly(c[0], c[1], R * 1.09, 0)[i]
        out.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke-dasharray="1 7"/>' % (c[0], c[1], vx, vy))
    out.append('</g>')
    out.append(mark_svg_body(M, True, "light", x0=mx, y0=my))
    # quote della costruzione
    mono("R = 0,48 L  ·  r = 0,34 R  ·  360° / 7 = 51,43°", c[0], my + M + 70, 22, anchor="middle")
    mono("FENDITURE TANGENTI ALL'APERTURA · SPESSORE 0,05 R", c[0], my + M + 108, 18, anchor="middle")
    # intestazione
    mono("HEPTAZIP", 140, 150, 26, ink, tracking=0.32)
    mono("SEGNO · TAVOLA 01", W - 140, 150, 22, faint, anchor="end", tracking=0.2)
    out.append('<line x1="140" y1="190" x2="%d" y2="190" stroke="%s" stroke-width="1.2"/>' % (W - 140, faint))
    # la scritta
    lw, lh, body = logo_svg(font_word, "light", mark=200)
    sc = 1.0
    out.append('<g transform="translate(%.2f,%.2f)">%s</g>' % ((W - lw * sc) / 2, 1660, body))
    # colori
    y = 2030
    for i, (name, hexc) in enumerate((("INCHIOSTRO", "#0F1D2E"), ("NUCLEO", CORE), ("CARTA", "#ECE8DF"))):
        x = 140 + i * 590
        out.append('<rect x="%d" y="%d" width="120" height="120" fill="%s" stroke="%s" stroke-width="1"/>' % (x, y, hexc, faint))
        mono(name, x + 150, y + 50, 22, ink, tracking=0.2)
        mono(hexc.upper(), x + 150, y + 90, 22, faint)
    # le icone alle misure reali
    y = 2350
    x = 140
    for s_ in (16, 24, 32, 48, 64, 128):
        out.append(mark_svg_body(s_, s_ >= 48, "light", x0=x, y0=y + 128 - s_))
        mono("%d" % s_, x + s_ / 2, y + 170, 18, anchor="middle")
        x += s_ + 90
    mono("SOTTO 48 PX: SENZA FENDITURE", W - 140, y + 170, 18, anchor="end")
    out.append('<line x1="140" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="1.2"/>' % (y - 60, W - 140, y - 60, faint))
    return W, H, "\n  ".join(out)


# segno a 1 bit, 13x13, per il menu contestuale di Explorer (MenuLogo.bmp): a questa
# misura il segno è disegnato a mano, pixel per pixel
MENU_BITMAP = (
    ".....###.....",
    "...#######...",
    ".###########.",
    ".###########.",
    ".####...####.",
    ".###.....###.",
    ".###.....###.",
    ".####...####.",
    ".###########.",
    ".###########.",
    "..#########..",
    "...#######...",
    ".............",
)


def save_ico(images, path):
    """ICO come quelli di Windows: le misure fino a 128 in BMP a 32 bit (con la maschera
    AND), 256 in PNG. Ogni misura è un'immagine disegnata a quella misura."""
    import struct
    entries = []
    for im in images:
        w, h = im.size
        if w >= 256:
            buf = io.BytesIO()
            im.save(buf, "PNG")
            data = buf.getvalue()
        else:
            px = im.tobytes("raw", "BGRA")
            rows = [px[y * w * 4:(y + 1) * w * 4] for y in range(h)][::-1]   # dal basso
            mask_stride = ((w + 31) // 32) * 4
            mask = b""
            for y in range(h - 1, -1, -1):
                bits = bytearray(mask_stride)
                for x in range(w):
                    if im.getpixel((x, y))[3] == 0:
                        bits[x // 8] |= 0x80 >> (x % 8)
                mask += bytes(bits)
            hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(px) + len(mask), 0, 0, 0, 0)
            data = hdr + b"".join(rows) + mask
        entries.append((w, h, data))
    out = struct.pack("<HHH", 0, 1, len(entries))
    off = 6 + 16 * len(entries)
    for w, h, data in entries:
        out += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
    with open(path, "wb") as f:
        f.write(out + b"".join(d for _, _, d in entries))


def main():
    font, outdir = sys.argv[1], sys.argv[2]
    font_mono = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(outdir, exist_ok=True)
    import cairosvg
    from PIL import Image

    def write(name, text):
        with open(os.path.join(outdir, name), "w") as f:
            f.write(text)

    def png(svg_text, name, scale=1.0):
        cairosvg.svg2png(bytestring=svg_text.encode(), write_to=os.path.join(outdir, name), scale=scale)

    for pal in ("light", "dark"):
        sfx = "" if pal == "light" else "-dark"
        mark = svg(512, 512, mark_svg_body(512, True, pal))
        write("heptazip-mark%s.svg" % sfx, mark)
        w, h, body = logo_svg(font, pal)
        logo = svg(round(w, 1), h, body)
        write("heptazip-logo%s.svg" % sfx, logo)
        png(logo, "heptazip-logo%s.png" % sfx, scale=1600 / w)
    # icone: sotto i 48 px la versione senza fenditure
    sizes = (16, 24, 32, 48, 64, 128, 256, 512)
    images = []
    for s_ in sizes:
        t = svg(s_, s_, mark_svg_body(s_, s_ >= 48, "light"))
        png(t, "heptazip-mark-%d.png" % s_)
        images.append(Image.open(os.path.join(outdir, "heptazip-mark-%d.png" % s_)).convert("RGBA"))
    if font_mono:
        W, H, body = sheet_svg(font, font_mono)
        png(svg(W, H, body), "heptazip-tavola.png")
    # ICO per Windows (7zFM, 7zG, estensione di Explorer): ogni misura dalla sua immagine
    save_ico([im for im in images if im.size[0] <= 256], os.path.join(outdir, "heptazip.ico"))
    # logo della finestra Informazioni (7zipLogo.ico): segno e nome, alto 63 px come
    # l'originale (l'immagine è mostrata alla sua misura reale)
    w, h, body = logo_svg(font, "light", mark=56)
    about = svg(math.ceil(w), 63, '<g transform="translate(0,3.5)">%s</g>' % body)
    im = Image.open(io.BytesIO(cairosvg.svg2png(bytestring=about.encode()))).convert("RGBA")
    save_ico([im], os.path.join(outdir, "heptazip-about.ico"))
    # menu contestuale: bitmap a 1 bit (0 = nero)
    bmp = Image.new("1", (13, 13), 1)
    for y, row in enumerate(MENU_BITMAP):
        for x, ch in enumerate(row):
            if ch == "#":
                bmp.putpixel((x, y), 0)
    bmp.save(os.path.join(outdir, "heptazip-menu.bmp"))


if __name__ == "__main__":
    main()
