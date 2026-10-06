#!/usr/bin/env python3
"""Create clearly labeled source diagrams for non-visual operator contracts."""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "site" / "media"


def esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;")


def label(x, y, s, size=25, color="#dbe7e7", weight=500):
    return f'<text x="{x}" y="{y}" fill="{color}" font-family="Arial, sans-serif" font-size="{size}" font-weight="{weight}">{esc(s)}</text>'


def panel(x, title, subtitle):
    return (f'<rect x="{x}" y="196" width="310" height="360" rx="20" fill="#17232b" stroke="#344650"/>'
            + label(x+22, 236, title, 22, "#f4dec1", 700)
            + label(x+22, 265, subtitle, 16, "#96aeb8"))


def fibers(x, y, widths, color):
    parts = []
    for i, width in enumerate(widths):
        xx = x + i*18
        bend = (i%5-2)*5
        parts.append(f'<path d="M {xx} {y+185} C {xx+bend} {y+100}, {xx+18+bend} {y+65}, {xx+28+bend} {y}" fill="none" stroke="{color}" stroke-width="{width}" stroke-linecap="round"/>')
    return "".join(parts)


def base(title, subtitle, body, footer):
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="680" viewBox="0 0 1200 680" role="img" aria-label="{esc(title)} source diagram">
<!-- Copyright (c) 2026 Nick Burkard; SPDX-License-Identifier: MIT -->
<defs><linearGradient id="bg" x2="0" y2="1"><stop stop-color="#1e2b34"/><stop offset="1" stop-color="#101b23"/></linearGradient></defs>
<rect width="1200" height="680" fill="url(#bg)"/>
<rect x="48" y="36" width="144" height="27" rx="13" fill="#344a50"/>
{label(62,55,"SOURCE DIAGRAM",13,"#b8d9d7",700)}
{label(49,112,title,48,"#f8e7cf",700)}
{label(51,151,subtitle,20,"#adc0c7")}
{body}
<path d="M 48 610 H 1152" stroke="#3a5158"/>
{label(50,639,footer,16,"#a8bec5")}
</svg>'''


def width_blend():
    a = panel(54, "INPUT A", "Narrow width plane") + fibers(83, 320, [2]*14, "#79bac8")
    b = panel(445, "INPUT B", "Broad width plane") + fibers(474, 320, [9]*14, "#f0ad78")
    c = panel(836, "OUTPUT", "weight = 0.35") + fibers(865, 320, [4.45]*14, "#e8d3ab")
    arrows = '<path d="M 381 375 H 428 M 772 375 H 819" stroke="#a7c5c8" stroke-width="3" marker-end="url(#arr)"/>'
    arr = '<defs><marker id="arr" markerWidth="9" markerHeight="9" refX="8" refY="4.5" orient="auto"><path d="M0 0 L9 4.5 L0 9" fill="#a7c5c8"/></marker></defs>'
    return base("Width Blend", "Two geometry inputs share points; only strand widths change.",
                arr+a+b+c+arrows+label(395,586,"width = A × (1 − weight) + B × weight",20,"#d7e4dc",600),
                "Runtime graph operator · schematic, not a rendered USD prim")


def freeze():
    def shape(x, color, changed=False):
        items=[]
        for i in range(8):
            xx=x+35+i*32
            shift=55 if changed else 16
            items.append(f'<path d="M{xx} 501 C{xx+4} 422 {xx+shift} 374 {xx+shift} 308" fill="none" stroke="{color}" stroke-width="3" opacity=".9"/>')
        return "".join(items)
    body=(panel(54,"CAPTURE","initial upstream cook")+shape(54,"#ddc69e")
          +panel(445,"UPSTREAM EDIT","input geometry changes")+shape(445,"#80b8ca",True)
          +panel(836,"FROZEN OUTPUT","held at captured state")+shape(836,"#ddc69e")
          +label(392,586,"One session · capture → edit → recook",21,"#d7e4dc",600))
    return base("Freeze", "The frozen output reuses a captured result after upstream changes.",body,
                "Same-session state transition · schematic, not a single-frame render")


def part():
    body = '<rect x="140" y="220" width="920" height="330" rx="14" fill="#1c2d33" stroke="#52676a"/>'
    body += '<path d="M600 225 L600 545" stroke="#e3d5ba" stroke-width="4" stroke-dasharray="12 9"/>'
    for row in range(5):
        for col in range(11):
            x=185+col*82+(row%2)*16
            y=270+row*55
            color="#7fc1cb" if x<600 else "#e7ad7d"
            body+=f'<circle cx="{x}" cy="{y}" r="6" fill="{color}"/>'
    body += label(245,584,"partId = 0",22,"#7fc1cb",700)+label(802,584,"partId = 1",22,"#e7ad7d",700)
    return base("Part", "Classifies roots on either side of the guide curve; points stay in place.",body,
                "Uniform partId output plane · illustrative layout of the kernel contract")


def instance():
    body=panel(54,"CURVE INPUT","Hair curves and IDs")+fibers(83,320,[3]*14,"#d3b48b")
    body+=panel(445,"PROTOTYPES","Geometry library")
    for i in range(4):
        x=486+i*60
        body+=f'<rect x="{x}" y="350" width="32" height="100" rx="13" fill="none" stroke="#8eaeb9" stroke-width="4"/>'
    body+=panel(836,"NO OUTPUT","Kernel not registered")+label(878,400,"UNAVAILABLE",31,"#edaa89",700)
    return base("Instance", "Schema controls are reserved; this build has no registered evaluator.",body,
                "Availability diagram · no generated instance result is claimed")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, artwork in (("width-blend",width_blend()),("freeze",freeze()),
                          ("part",part()),("instance",instance())):
        (OUT/f"{name}.svg").write_text(artwork+"\n",encoding="utf-8")
        print(OUT/f"{name}.svg")


if __name__ == "__main__":
    main()
