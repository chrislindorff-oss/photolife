#!/usr/bin/env python3
"""Writes the Getting Started guide's diagrams (*.svg here).

Simple shapes on a light card each, so they read in light and dark themes.
Edit this script rather than the SVGs, run it from this folder, then render
the PNGs as described in ../README.md.
"""
import math

HEAD = '''<?xml version="1.0" encoding="UTF-8"?>
<!-- Getting Started guide diagram, written by make_svgs.py. Rendered to resources/guide/{name}.png: see ../README.md. -->
<svg xmlns="http://www.w3.org/2000/svg" width="{w2}" height="{h2}" viewBox="0 0 {w} {h}">
<style>
  text {{ font-family: "Noto Sans", "Segoe UI", "Helvetica Neue", Arial, sans-serif; fill: #1f2933; }}
  .t {{ font-size: 13px; font-weight: 600; }}
  .s {{ font-size: 11px; fill: #52606d; }}
  .w {{ fill: #ffffff; font-size: 12px; font-weight: 700; }}
  .num {{ fill: #ffffff; font-size: 13px; font-weight: 700; }}
</style>
<rect x="1" y="1" width="{iw}" height="{ih}" rx="14" fill="#f6f9f7" stroke="#d6e2da" stroke-width="2"/>
'''
G, B, A, R, GR, LG, LB = "#2e7d4f", "#3b6fb6", "#d08a1c", "#c0392b", "#9aa5b1", "#e3efe7", "#e2ebf7"


def svg(name, w, h, body):
    return HEAD.format(name=name, w=w, h=h, w2=2 * w, h2=2 * h, iw=w - 2, ih=h - 2) + body + "</svg>\n"


def arrow(x1, y1, x2, y2, c="#7b8794"):
    a = math.atan2(y2 - y1, x2 - x1)
    L = 9
    p1 = (x2 - L * math.cos(a - 0.45), y2 - L * math.sin(a - 0.45))
    p2 = (x2 - L * math.cos(a + 0.45), y2 - L * math.sin(a + 0.45))
    return (f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{c}" stroke-width="2.5" stroke-linecap="round"/>'
            f'<polygon points="{x2},{y2} {p1[0]:.1f},{p1[1]:.1f} {p2[0]:.1f},{p2[1]:.1f}" fill="{c}"/>')


def folder(x, y, w=74, h=52, c=A):
    return (f'<path d="M{x} {y + 8} q0 -8 8 -8 h{w * 0.32:.1f} l8 8 h{w * 0.68 - 16:.1f} q8 0 8 8 v{h - 16} '
            f'q0 8 -8 8 h{-(w - 8)} q-8 0 -8 -8 z" fill="{c}" opacity="0.9"/>')


def photo(x, y, w=34, h=26, c=G, sun=True):
    s = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="3" fill="#ffffff" stroke="{GR}" stroke-width="1.5"/>'
    s += (f'<path d="M{x + 3} {y + h - 3} l{w * 0.3:.1f} {-h * 0.45:.1f} l{w * 0.2:.1f} {h * 0.25:.1f} '
          f'l{w * 0.15:.1f} {-h * 0.15:.1f} l{w * 0.29:.1f} {h * 0.35:.1f} z" fill="{c}" opacity="0.75"/>')
    if sun:
        s += f'<circle cx="{x + w * 0.75:.1f}" cy="{y + h * 0.3:.1f}" r="{h * 0.12:.1f}" fill="{A}"/>'
    return s


def pin(x, y, c=R):
    return f'<path d="M{x} {y} c-7 -9 -9 -12 -9 -15 a9 9 0 0 1 18 0 c0 3 -2 6 -9 15z" fill="{c}"/>'


def badge(x, y, n, c=G):
    return f'<circle cx="{x}" cy="{y}" r="12" fill="{c}"/><text x="{x}" y="{y + 4.5}" text-anchor="middle" class="num">{n}</text>'


def tick(x, y, c=G, r=9):
    return (f'<circle cx="{x}" cy="{y}" r="{r}" fill="{c}"/><path d="M{x - 4} {y} l3 3 l5 -6" stroke="#fff" '
            f'stroke-width="2.2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')


def button(x, y, w, label, c=G):
    return (f'<rect x="{x}" y="{y}" width="{w}" height="24" rx="6" fill="{c}"/>'
            f'<text x="{x + w / 2}" y="{y + 16}" text-anchor="middle" class="w">{label}</text>')


def text_lines(x, y, lines, cls="s", step=16, anchor="start"):
    return "".join(f'<text x="{x}" y="{y + i * step}" text-anchor="{anchor}" class="{cls}">{t}</text>'
                   for i, t in enumerate(lines))


def flow():
    steps = [("Add your", "photos"), ("Build a", "reference tree"), ("Match", "your photos"),
             ("Review the", "uncertain ones"), ("Explore", "your coverage")]
    x0, bw, gap, top = 16, 110, 14, 40
    b = ""
    for i, (a, l2) in enumerate(steps):
        x = x0 + i * (bw + gap)
        b += f'<rect x="{x}" y="{top}" width="{bw}" height="96" rx="10" fill="#ffffff" stroke="{G}" stroke-width="2"/>'
        b += badge(x + bw / 2, top, i + 1)
        b += text_lines(x + bw / 2, top + 72, [a, l2], "t", 16, "middle")
        if i < 4:
            b += arrow(x + bw + 2, top + 48, x + bw + gap - 2, top + 48)
    cx = [x0 + i * (bw + gap) + bw / 2 for i in range(5)]
    iy = top + 18   # icon band: top+18 .. top+52
    b += folder(cx[0] - 22, iy, 44, 30)
    b += (f'<path d="M{cx[1]} {iy + 2} v30 M{cx[1]} {iy + 12} l-15 12 M{cx[1]} {iy + 12} l15 12" stroke="{G}" '
          f'stroke-width="3" fill="none"/><circle cx="{cx[1]}" cy="{iy + 2}" r="5" fill="{G}"/>'
          f'<circle cx="{cx[1] - 15}" cy="{iy + 26}" r="5" fill="{G}"/><circle cx="{cx[1] + 15}" cy="{iy + 26}" r="5" fill="{G}"/>')
    b += photo(cx[2] - 38, iy + 2, 32, 24) + arrow(cx[2] - 2, iy + 14, cx[2] + 12, iy + 14) + tick(cx[2] + 24, iy + 14)
    b += photo(cx[3] - 36, iy + 2, 32, 24) + photo(cx[3] + 4, iy + 2, 32, 24, B, False)
    b += (f'<rect x="{cx[4] - 30}" y="{iy + 4}" width="60" height="9" rx="4.5" fill="{LG}"/>'
          f'<rect x="{cx[4] - 30}" y="{iy + 4}" width="36" height="9" rx="4.5" fill="{G}"/>') + pin(cx[4], iy + 34)
    b += f'<text x="320" y="22" text-anchor="middle" class="s">The basics, in order</text>'
    for j, (l1, l2, c) in enumerate([("iNaturalist:", "download your observations", B),
                                     ("After a field trip:", "organise new photos", A)]):
        x = 60 + j * 290
        b += (f'<rect x="{x}" y="176" width="230" height="48" rx="10" fill="#ffffff" stroke="{c}" '
              f'stroke-width="2" stroke-dasharray="6 4"/>')
        b += (f'<text x="{x + 115}" y="196" text-anchor="middle" class="t" style="fill:{c}">{l1}</text>'
              f'<text x="{x + 115}" y="213" text-anchor="middle" class="s">{l2}</text>')
        b += arrow(x + 115, 174, cx[0] - 12 + j * 24, top + 100, c)
    b += f'<text x="320" y="246" text-anchor="middle" class="s">…new photos from either feed back into step 1</text>'
    return svg("flow", 640, 260, b)


def library():
    b = folder(26, 34, 92, 64) + photo(36, 52, 30, 22) + photo(72, 56, 30, 22, B, False)
    b += folder(26, 112, 70, 44)
    b += f'<text x="72" y="184" text-anchor="middle" class="s">Your photo folders</text>'
    b += arrow(138, 100, 196, 100)
    b += (f'<rect x="214" y="30" width="190" height="150" rx="10" fill="#ffffff" stroke="{GR}" stroke-width="1.5"/>'
          f'<text x="309" y="50" text-anchor="middle" class="t">PhotoLife</text>')
    for r in range(3):
        for c in range(4):
            b += photo(226 + c * 44, 62 + r * 36, 38, 28, [G, B, A][(r + c) % 3], (r + c) % 2 == 0)
    b += text_lines(424, 80, ["Reads your photos", "where they are —", "nothing is moved", "or changed"])
    return svg("library", 560, 200, b)


def tree():
    b = f'<rect x="20" y="40" width="160" height="110" rx="10" fill="#ffffff" stroke="{B}" stroke-width="2"/>'
    b += text_lines(100, 64, ["Group: Orchids", "Region: Victoria"], "t", 22, "middle")
    b += pin(100, 132) + f'<text x="100" y="168" text-anchor="middle" class="s">from iNaturalist</text>'
    b += arrow(190, 95, 236, 95)
    b += f'<path d="M262 36 v104 M262 64 h70 M262 88 h70 M262 112 h70 M262 136 h70" stroke="{GR}" stroke-width="2" fill="none"/>'

    def node(x, y, label, fill=True):
        return (f'<circle cx="{x}" cy="{y}" r="7" fill="{G if fill else "#ffffff"}" stroke="{G}" stroke-width="2"/>'
                f'<text x="{x + 12}" y="{y + 4}" class="s">{label}</text>')
    b += f'<circle cx="262" cy="30" r="7" fill="{G}"/><text x="276" y="34" class="t">Orchidaceae</text>'
    b += node(340, 64, "Diuris pardina") + node(340, 88, "Diuris sulphurea", False)
    b += node(340, 112, "Caladenia carnea") + node(340, 136, "Caladenia …", False)
    b += text_lines(262, 168, ["every species recorded there —", "filled = you have a photo of it"])
    return svg("tree", 560, 200, b)


def match():
    b = folder(24, 48, 96, 66)
    b += (f'<text x="72" y="134" text-anchor="middle" class="t">Flame Robin/</text>'
          f'<text x="72" y="152" text-anchor="middle" class="s">IMG_1234.jpg</text>')
    b += arrow(140, 96, 200, 96) + f'<text x="170" y="84" text-anchor="middle" class="s">names</text>'
    b += f'<rect x="214" y="58" width="220" height="76" rx="10" fill="#ffffff" stroke="{G}" stroke-width="2"/>'
    b += f'<text x="234" y="86" class="t">Petroica phoenicea</text><text x="234" y="106" class="s">Flame Robin · species</text>'
    b += tick(412, 82)
    b += text_lines(214, 160, ["Folder and file names (scientific or common) are",
                               "matched automatically; unsure ones wait for you"])
    return svg("match", 560, 200, b)


def review():
    b = f'<text x="96" y="36" text-anchor="middle" class="s">Your photo</text><text x="252" y="36" text-anchor="middle" class="s">iNaturalist reference</text>'
    b += photo(30, 44, 132, 96, G) + photo(186, 44, 132, 96, B, False)
    b += button(30, 152, 96, "Confirm") + button(136, 152, 80, "Reject", R) + button(226, 152, 92, "Genus only", B)
    b += text_lines(350, 70, ["Compare the two,", "pick the right taxon,", "and confirm.", "", "PhotoLife remembers",
                              "the names you confirm."])
    return svg("review", 560, 200, b)


def explore():
    b = f'<rect x="24" y="30" width="250" height="150" rx="10" fill="#ffffff" stroke="{GR}" stroke-width="1.5"/>'
    for i, (n, h, t) in enumerate([("Orchidaceae", 113, 383), ("Caladenia", 41, 120), ("Diuris", 18, 30),
                                   ("Pterostylis", 30, 95)]):
        y = 54 + i * 32
        b += f'<text x="38" y="{y + 4}" class="t">{n}</text><text x="262" y="{y + 4}" text-anchor="end" class="s">{h} / {t}</text>'
        b += (f'<rect x="38" y="{y + 10}" width="222" height="7" rx="3.5" fill="{LG}"/>'
              f'<rect x="38" y="{y + 10}" width="{222 * h / t:.0f}" height="7" rx="3.5" fill="{G}"/>')
    b += f'<rect x="300" y="30" width="236" height="150" rx="10" fill="{LB}" stroke="{GR}" stroke-width="1.5"/>'
    b += f'<path d="M320 160 q40 -60 80 -40 t70 -50 t50 -30" stroke="#b9c9e0" stroke-width="10" fill="none" stroke-linecap="round"/>'
    for x, y, c in [(350, 96, G), (420, 118, G), (470, 70, R), (392, 64, G), (500, 132, R)]:
        b += pin(x, y, c)
    b += f'<text x="418" y="174" text-anchor="middle" class="s">map: green have · red still to find</text>'
    return svg("explore", 560, 200, b)


def inat():
    b = f'<rect x="24" y="36" width="170" height="130" rx="10" fill="#ffffff" stroke="{B}" stroke-width="2"/>'
    b += photo(36, 48, 146, 70, G) + text_lines(36, 138, ["Your iNaturalist", "observation"], "t")
    b += arrow(204, 100, 262, 100, B) + f'<text x="233" y="88" text-anchor="middle" class="s">download</text>'
    b += folder(282, 56, 104, 72) + photo(296, 76, 36, 26) + photo(338, 80, 36, 26, B, False)
    b += f'<text x="334" y="150" text-anchor="middle" class="s">into your library</text>'
    b += text_lines(410, 76, ["Sign in with your", "iNaturalist username,", "choose a tree, see what",
                              "your library is missing,", "and download it."])
    return svg("inat", 560, 200, b)


def organise():
    b = f'<rect x="20" y="40" width="300" height="74" rx="8" fill="#2b2f33"/>'
    runs = [G, G, B, B, B, A]
    for i, c in enumerate(runs):
        x = 30 + i * 48
        b += f'<rect x="{x}" y="48" width="40" height="44" rx="3" fill="#ffffff"/>' + photo(x + 3, 58, 34, 26, c, False)
        b += f'<rect x="{x}" y="96" width="40" height="8" rx="3" fill="{c}"/>'
    b += f'<text x="30" y="134" class="s">Name the first photo of each taxon — the rest follow</text>'
    b += arrow(330, 78, 374, 78)
    for i, (n, c) in enumerate([("Flame Robin", G), ("Superb Fairywren", B), ("Rejects", A)]):
        b += folder(392, 30 + i * 52, 46, 34, c) + f'<text x="446" y="{54 + i * 52}" class="s">{n}</text>'
    return svg("organise", 560, 200, b)


if __name__ == "__main__":
    for fn in (flow, library, tree, match, review, explore, inat, organise):
        with open(f"{fn.__name__}.svg", "w") as f:
            f.write(fn())
        print(f"{fn.__name__}.svg")
