#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Icone della barra degli strumenti di Heptazip File Manager (7zFM).

    make_toolbar_icons.py <dir_uscita> [anteprima.png]

Sette icone a linee, nello stile del logo (docs/logo): tratto d'inchiostro, una sola nota
verde acqua per l'azione. Disegnate su una griglia di 24 unità e calcolate come geometria
(shapely: i tratti diventano poligoni, i distintivi ritagliano ciò che sta sotto), poi
rese a ogni misura usata dalla barra: 24 e 32 px per le scale 100-200% di Windows.
Per ogni icona anche la variante per il tema scuro (*_dark.ico, tratto chiaro).

Richiede: shapely, cairosvg, Pillow (nel venv di ~/devtools).
"""

import io
import os
import sys

from shapely.geometry import LineString, Point, Polygon, box
from shapely.ops import unary_union

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_logo import CORE, INK, geom_path, save_ico  # noqa: E402

LIGHT_INK = "#E6E4DF"   # il tratto sul tema scuro

W = 1.7            # spessore del tratto (unità su 24)
PAPER = "#FFFFFF"
# misure nell'ICO: barra piccola 24 px e grande 32 px alle scale di Windows
# 100, 125, 150, 175, 200, 225, 250, 300%
SIZES = (24, 30, 32, 36, 40, 42, 48, 54, 56, 60, 64, 72, 80, 96)


def stroke(points, closed=False, w=W):
    line = LineString(list(points) + ([points[0]] if closed else []))
    return line.buffer(w / 2, cap_style="round", join_style="round", quad_segs=8)


def rrect(x0, y0, x1, y1, r):
    return box(x0 + r, y0 + r, x1 - r, y1 - r).buffer(r, quad_segs=8)


def outline(shape, w=W):
    return shape.exterior.buffer(w / 2, quad_segs=8)


def archive():
    """la scatola dell'archivio: coperchio, corpo e la fessura della maniglia"""
    lid = outline(rrect(3, 4, 21, 8.5, 1.2))
    body = outline(rrect(4.5, 8.5, 19.5, 20.5, 1.2)).difference(box(5.5, 7, 18.5, 9.4))
    slot = stroke([(10, 12.5), (14, 12.5)])
    return unary_union([lid, body, slot])


def badge(base, cx=17.5, cy=17.5, r=5.2):
    """il distintivo verde acqua in basso a destra, che ritaglia la figura sotto"""
    disc = Point(cx, cy).buffer(r, quad_segs=16)
    return base.difference(disc.buffer(1.3)), disc


def plus_mark(cx, cy, s=2.6):
    return unary_union([stroke([(cx - s, cy), (cx + s, cy)], w=1.6), stroke([(cx, cy - s), (cx, cy + s)], w=1.6)])


def arrow_down(cx, cy, s=2.8):
    return unary_union([stroke([(cx, cy - s), (cx, cy + s)], w=1.6),
                        stroke([(cx - 2.3, cy + s - 2.3), (cx, cy + s), (cx + 2.3, cy + s - 2.3)], w=1.6)])


def check_mark(cx, cy):
    return stroke([(cx - 2.6, cy + 0.1), (cx - 0.8, cy + 2), (cx + 2.7, cy - 1.9)], w=1.6)


def icon_add():
    base, disc = badge(archive())
    return [(base, INK), (disc.difference(plus_mark(17.5, 17.5)), CORE)]


def icon_extract():
    base, disc = badge(archive())
    return [(base, INK), (disc.difference(arrow_down(17.5, 17.5)), CORE)]


def icon_test():
    base, disc = badge(archive())
    return [(base, INK), (disc.difference(check_mark(17.5, 17.5)), CORE)]


def document(x0, y0, x1, y1, fold=4):
    pts = [(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)]
    shape = Polygon(pts)
    return shape, unary_union([stroke(pts, closed=True), stroke([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)])])


def icon_copy():
    front, front_line = document(9, 7.5, 20, 21)
    back, back_line = document(4, 3, 15, 16.5)
    back_line = back_line.difference(front.buffer(1.4))
    return [(back_line, INK), (front_line, CORE)]


def icon_move():
    doc, doc_line = document(3.5, 3, 14.5, 19)
    arrow = unary_union([stroke([(9.5, 15), (20.5, 15)]), stroke([(17, 11.5), (20.5, 15), (17, 18.5)])])
    doc_line = doc_line.difference(arrow.buffer(1.4))
    return [(doc_line, INK), (arrow, CORE)]


def icon_delete():
    lid = stroke([(3.5, 6), (20.5, 6)])
    handle = stroke([(9, 6), (9.8, 3.2), (14.2, 3.2), (15, 6)])
    can = stroke([(5.5, 6), (6.8, 20.5), (17.2, 20.5), (18.5, 6)])
    ribs = unary_union([stroke([(10, 10), (10.3, 17)]), stroke([(14, 10), (13.7, 17)])])
    return [(unary_union([lid, handle, can]), INK), (ribs, CORE)]


def icon_info():
    ring = Point(12, 12).buffer(9, quad_segs=24).exterior.buffer(W / 2, quad_segs=8)
    dot = Point(12, 7.6).buffer(1.35, quad_segs=12)
    bar = stroke([(12, 11), (12, 17)], w=2.0)
    return [(ring, INK), (unary_union([dot, bar]), CORE)]


ICONS = {
    "tb_add": icon_add,
    "tb_extract": icon_extract,
    "tb_test": icon_test,
    "tb_copy": icon_copy,
    "tb_move": icon_move,
    "tb_delete": icon_delete,
    "tb_info": icon_info,
}


def icon_svg(parts, size, x0=0.0, y0=0.0, dark=False):
    k = size / 24.0
    body = []
    for geom, color in parts:
        if dark and color == INK:
            color = LIGHT_INK
        body.append('<path transform="translate(%.3f,%.3f) scale(%.5f)" fill="%s" fill-rule="evenodd" d="%s"/>'
                    % (x0, y0, k, color, geom_path(geom)))
    return "\n".join(body)


def render(parts, size, dark=False):
    import cairosvg
    from PIL import Image
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">%s</svg>'
           % (size, size, icon_svg(parts, size, dark=dark)))
    return Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg.encode()))).convert("RGBA")


def preview(path):
    """anteprima: le sette icone a 16, 24, 32, 48 px su fondo chiaro, e a 24 px
    ingrandite 4 volte (pixel per pixel) per controllare la resa"""
    from PIL import Image
    names = list(ICONS)
    sizes = (16, 24, 32, 48)
    cell = 104
    W_ = cell * len(names) + 40
    H_ = cell * len(sizes) + 40 + 96 + 20
    sheet = Image.new("RGBA", (W_, H_), (243, 243, 243, 255))
    for i, n in enumerate(names):
        parts = ICONS[n]()
        for j, s in enumerate(sizes):
            im = render(parts, s)
            sheet.alpha_composite(im, (20 + i * cell + (cell - s) // 2, 20 + j * cell + (cell - s) // 2))
        big = render(parts, 24).resize((96, 96), Image.NEAREST)
        bg = Image.new("RGBA", (96, 96), (255, 255, 255, 255))
        bg.alpha_composite(big)
        sheet.alpha_composite(bg, (20 + i * cell + (cell - 96) // 2, 20 + len(sizes) * cell))
    sheet.save(path)


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for name, fn in ICONS.items():
        parts = fn()
        save_ico([render(parts, s) for s in SIZES], os.path.join(outdir, name + ".ico"))
        # per il tema scuro: il tratto chiaro
        save_ico([render(parts, s, dark=True) for s in SIZES], os.path.join(outdir, name + "_dark.ico"))
    if len(sys.argv) > 2:
        preview(sys.argv[2])


if __name__ == "__main__":
    main()
