#!/usr/bin/env python3
"""Draws docs/readme/keys.svg, the README's picture: Pinhole's shortcuts on a keyboard.

Run it after a shortcut changes: docs/readme/keys.py. The text is drawn as outlines
of JetBrains Mono, so the picture looks the same on every computer.
"""
from pathlib import Path
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

FONTS = Path("/usr/share/fonts/TTF")
BOLD = TTFont(FONTS / "JetBrainsMonoNerdFont-Bold.ttf")
REGULAR = TTFont(FONTS / "JetBrainsMonoNerdFont-Regular.ttf")

YELLOW, INK, GREY, EDGE, DIM = "#FFD60A", "#1a1a1c", "#8d8c86", "#3a3a3d", "#4a4a4e"

ROWS = ["QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"]
STAGGER = [0, 0.25, 0.75]  # of a key and its gap, as on a real keyboard
KEYS = {
    "R": "Turn", "T": "Theme", "Y": "Calendar", "I": "Info", "O": "Send", "P": "Frame",
    "S": "Select", "D": "This day", "F": "Favourite", "G": "Series", "H": "Mosaic", "K": "Crop", "L": "Live",
    "C": "Copy", "B": "Background", "N": "Newest", "M": "Map",
}
MORE = [("Tab", "Settings"), ("?", "Shortcuts"), ("Space", "Open"), ("Esc", "Back"),
        ("Ctrl F", "Search"), ("⇧ F", "People"), ("⇧ S", "Screenshots")]

WIDTH, LEFT, KEY, GAP = 1680, 108, 132, 16


def text(x, y, string, size, font, fill, anchor="start"):
    glyphs, cmap, scale = font.getGlyphSet(), font.getBestCmap(), size / font["head"].unitsPerEm
    advance = sum(font["hmtx"][cmap[ord(c)]][0] for c in string) * scale
    if anchor == "end":
        x -= advance
    pen = SVGPathPen(glyphs)
    for c in string:
        name = cmap[ord(c)]
        glyphs[name].draw(TransformPen(pen, (scale, 0, 0, -scale, x, y)))
        x += font["hmtx"][name][0] * scale
    return f'<path d="{pen.getCommands()}" fill="{fill}"/>'


def key(x, y, w, h, top, label, size):
    lit = label is not None
    out = [f'<rect x="{x:g}" y="{y}" width="{w:g}" height="{h}" rx="14" fill="{YELLOW if lit else "none"}" '
           f'stroke="{YELLOW if lit else EDGE}" stroke-width="2"/>',
           text(x + 16, y + 16 + size * 0.73, top, size, BOLD, INK if lit else DIM)]
    if lit:
        out.append(text(x + 16, y + h - 18, label, 17, REGULAR, INK))
    return out


def main():
    rows_top, more_top, more_h = 120, 596, 96
    height = more_top + more_h + 56
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {height}" role="img">',
           "<title>Every view of Pinhole is one key away</title>",
           f'<rect width="{WIDTH}" height="{height}" rx="28" fill="{INK}"/>',
           text(LEFT, 84, "every view is one key away", 26, REGULAR, GREY)]
    for r, row in enumerate(ROWS):
        for i, letter in enumerate(row):
            x = LEFT + (STAGGER[r] + i) * (KEY + GAP)
            out += key(x, rows_top + r * (KEY + GAP), KEY, KEY, letter, KEYS.get(letter), 40)
    w = (10 * KEY + 9 * GAP - (len(MORE) - 1) * GAP) / len(MORE)
    for i, (keys, label) in enumerate(MORE):
        out += key(LEFT + i * (w + GAP), more_top, w, more_h, keys, label, 30)
    out.append("</svg>")
    Path(__file__).with_name("keys.svg").write_text("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
