#!/usr/bin/env python3
"""Draws docs/readme/plugins.svg: the plugins on one line, from the ones used every day to the ones made for the joy of it.

Run it after a plugin comes, goes or changes its key: docs/readme/plugins.py. The order of PLUGINS is the order
on the line.
"""
from pathlib import Path

from keys import BOLD, EDGE, GAP, INK, KEY, LEFT, REGULAR, WIDTH, YELLOW, text

PLUGINS = [
    ("Favourites", "F"), ("Turn and crop", "R K"), ("Info", "I"), ("Send to phone", "O"), ("Screenshots", "⇧ S"), ("Photos of screens", "⇧ D"),
    ("Search", "Ctrl F"), ("People", "⇧ F"), ("Live Photos", "L"), ("Map", "M"), ("On this day", "D"),
    ("Series", "G"), ("Contact sheet", "K"), ("Year calendar", "Y"), ("Photo frame", "P"), ("Background", "B"),
    ("Theme", "T"), ("Rainbow wall", "W"), ("Mosaic", "H"),
]
PAPER = "#f4f3ee"
HEIGHT, LINE, CAP, NAME, KEYSIZE = 404, 218, 44, 17, 20


def width(string, size, font):
    cmap = font.getBestCmap()
    return sum(font["hmtx"][cmap[ord(c)]][0] for c in string) * size / font["head"].unitsPerEm


def split(name):
    """A name of more than one short word on two lines, at the space nearest its middle, so that it does not run
    into its neighbour on the same side of the line."""
    spaces = [i for i, c in enumerate(name) if c == " "]
    if len(name) <= 10 or not spaces:
        return [name]
    cut = min(spaces, key=lambda i: abs(i - len(name) / 2))
    return [name[:cut], name[cut + 1:]]


def main():
    right = LEFT + 10 * KEY + 9 * GAP
    first, last = LEFT + 74, right - 74
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {HEIGHT}" role="img">',
           "<title>Pinhole's plugins, from useful every day to imagined it, so I built it</title>",
           f'<rect width="{WIDTH}" height="{HEIGHT}" rx="28" fill="{INK}"/>',
           text(LEFT, 62, "useful every day", 26, BOLD, PAPER),
           text(right, 62, "imagined it, so I built it", 26, BOLD, PAPER, "end"),
           f'<path d="M{LEFT} {LINE} H{right - 4} M{right - 18} {LINE - 10} L{right - 4} {LINE} L{right - 18} {LINE + 10}" '
           f'stroke="{EDGE}" stroke-width="3" fill="none" stroke-linecap="round" stroke-linejoin="round"/>']
    for i, (name, keys) in enumerate(PLUGINS):
        x = first + i * (last - first) / (len(PLUGINS) - 1)
        up = i % 2 == 0
        w = max(CAP, width(keys, KEYSIZE, BOLD) + 22)
        cap_y = LINE - 26 - CAP if up else LINE + 26
        out += [f'<path d="M{x:.1f} {LINE} V{cap_y + CAP if up else cap_y}" stroke="{EDGE}" stroke-width="3"/>',
                f'<circle cx="{x:.1f}" cy="{LINE}" r="6" fill="{YELLOW}"/>',
                f'<rect x="{x - w / 2:.1f}" y="{cap_y}" width="{w:.1f}" height="{CAP}" rx="9" fill="{YELLOW}"/>',
                text(x - width(keys, KEYSIZE, BOLD) / 2, cap_y + 30, keys, KEYSIZE, BOLD, INK),
                ]
        lines = split(name)
        y = cap_y - 14 - (len(lines) - 1) * 22 if up else cap_y + CAP + 28
        for k, line in enumerate(lines):
            out.append(text(x - width(line, NAME, REGULAR) / 2, y + k * 22, line, NAME, REGULAR, PAPER))
    out.append("</svg>")
    Path(__file__).with_name("plugins.svg").write_text("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
