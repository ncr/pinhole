#!/usr/bin/env python3
"""Draws docs/readme/obscura.svg, the README's banner: a screenshot of Pinhole seen through a camera obscura.

docs/readme/obscura.py <screenshot> [crop], the crop as ImageMagick takes it (3152x736+0+0), for a wide strip of it. Light from the screenshot crosses in the pinhole and lands in the box upside
down, as it would. The screenshot goes into the SVG itself, since GitHub shows no picture an SVG points at.
"""
import base64
import subprocess
import sys
from pathlib import Path

from keys import BOLD, REGULAR, text

YELLOW, INK = "#FFD60A", "#1a1a1c"
WIDTH, PAD, TOP, SHOT_H, FRAME = 1680, 36, 168, 236, 6  # TOP: where the framed screenshot starts, under the two lines
NAME = Path(__file__).with_name("BricolageGrotesque-ExtraBold.ttf")  # the name's typeface, if it is here


def name(x, y, width):
    """The name on the camera's body, as wide as the picture inside it."""
    if NAME.exists():
        from fontTools.ttLib import TTFont
        font = TTFont(NAME)
        cmap, em = font.getBestCmap(), font["head"].unitsPerEm
        return text(x, y, "Pinhole", width * em / sum(font["hmtx"][cmap[ord(c)]][0] for c in "Pinhole"), font, YELLOW)
    # without the file: plain text, right only in a browser that has the typeface
    return (f'<text x="{x:.1f}" y="{y}" font-family="Bricolage Grotesque, sans-serif" font-weight="800" '
            f'font-size="{width / 3.02:.0f}" textLength="{width:.1f}" lengthAdjust="spacingAndGlyphs" '
            f'fill="{YELLOW}">Pinhole</text>')



def camera(top, right, bottom, hole_y):
    """The camera from its side: the box, what sticks out of its top, and the plate the pinhole is in."""
    def block(x, y, w, h, r=6):
        return f'<rect x="{x:g}" y="{y:g}" width="{w:g}" height="{h:g}" rx="{r}" fill="{INK}"/>'
    out = [block(PAD, top, right - PAD, bottom - top, 22), block(right - 8, hole_y - 34, 22, 68)]
    out += [block(PAD + 44, top - 24, 92, 34), block(right - 118, top - 14, 46, 24)]  # the winding knob, the shutter button
    return "\n".join(out)


def main(screenshot, crop=None):
    cut = ["-crop", crop, "+repage"] if crop else []
    jpeg = subprocess.run(["magick", screenshot, *cut, "-resize", "2160x", "-quality", "82", "jpg:-"],
                          check=True, capture_output=True).stdout
    w, h = map(int, subprocess.run(["magick", "identify", "-format", "%w %h", "jpg:-"], input=jpeg,
                                   check=True, capture_output=True).stdout.split())
    shot_h, shot_y, HEIGHT = SHOT_H - 2 * FRAME, TOP + FRAME, TOP + SHOT_H + PAD
    shot_w = shot_h * w / h
    shot_x = WIDTH - PAD - FRAME - shot_w
    hole_x, hole_y = 470, shot_y + shot_h / 2
    slope = (hole_y - shot_y) / (shot_x - hole_x)          # of the ray from the screenshot's top corner
    reach = (hole_x - 14 - PAD - 30) / (1 + 2 * slope * w / h)  # from the pinhole to the picture in the box
    small_h = 2 * slope * reach
    small_w = small_h * w / h
    small_x, small_y = hole_x - 14 - reach - small_w, hole_y - small_h / 2
    data = base64.b64encode(jpeg).decode()
    out = f'''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 {WIDTH} {HEIGHT}" role="img">
<title>Pinhole, seen through a camera obscura</title>
<defs><image id="shot" width="{shot_w:.1f}" height="{shot_h}" preserveAspectRatio="none" xlink:href="data:image/jpeg;base64,{data}"/></defs>
<rect width="{WIDTH}" height="{HEIGHT}" rx="28" fill="{YELLOW}"/>
{text(WIDTH - PAD, 82, "A fast photo browser for Omarchy.", 27, BOLD, INK, "end")}
{text(WIDTH - PAD, 122, "iCloud Photos and folders today. More sources coming.", 27, REGULAR, INK, "end")}
{camera(PAD + 24, hole_x - 14, HEIGHT - PAD, hole_y)}
{name(small_x, small_y - 44, small_w)}
<path d="M{hole_x} {hole_y} L{small_x + small_w:.1f} {small_y:.1f} L{small_x + small_w:.1f} {small_y + small_h:.1f} Z" fill="{YELLOW}" opacity="0.3"/>
<use xlink:href="#shot" opacity="0.8" transform="translate({small_x + small_w:.1f} {small_y + small_h:.1f}) rotate(180) scale({small_w / shot_w:.4f})"/>
<path d="M{shot_x:.1f} {shot_y} L{hole_x} {hole_y} L{shot_x:.1f} {shot_y + shot_h}" stroke="{INK}" stroke-width="2.5" fill="none"/>
<circle cx="{hole_x}" cy="{hole_y}" r="7" fill="{YELLOW}"/>
<rect x="{shot_x - FRAME:.1f}" y="{TOP}" width="{shot_w + 2 * FRAME:.1f}" height="{SHOT_H}" rx="14" fill="{INK}"/>
<use xlink:href="#shot" x="{shot_x:.1f}" y="{shot_y}"/>
</svg>
'''
    Path(__file__).with_name("obscura.svg").write_text(out)


if __name__ == "__main__":
    main(*sys.argv[1:3])
