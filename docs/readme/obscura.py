#!/usr/bin/env python3
"""Draws docs/readme/obscura.svg, the README's banner: a screenshot of Pinhole seen through a camera obscura.

docs/readme/obscura.py <screenshot> [crop], the crop as ImageMagick takes it (3152x736+0+0), for a wide strip of it. Light from the screenshot crosses in the pinhole and lands in the box upside
down, as it would. The screenshot goes into the SVG itself, since GitHub shows no picture an SVG points at.
"""
import base64
import subprocess
import sys
from pathlib import Path

from keys import BOLD, text

YELLOW, INK = "#FFD60A", "#1a1a1c"
WIDTH, PAD, TOP, SHOT_H = 1680, 36, 92, 250  # TOP: where the drawing starts, under the line of text


def main(screenshot, crop=None):
    cut = ["-crop", crop, "+repage"] if crop else []
    jpeg = subprocess.run(["magick", screenshot, *cut, "-resize", "2160x", "-quality", "82", "jpg:-"],
                          check=True, capture_output=True).stdout
    w, h = map(int, subprocess.run(["magick", "identify", "-format", "%w %h", "jpg:-"], input=jpeg,
                                   check=True, capture_output=True).stdout.split())
    shot_h, HEIGHT = SHOT_H, TOP + SHOT_H + PAD
    shot_w = shot_h * w / h
    shot_x = WIDTH - PAD - shot_w
    hole_x, hole_y = 456, TOP + shot_h / 2
    slope = (hole_y - TOP) / (shot_x - hole_x)          # of the ray from the screenshot's top corner
    reach = (hole_x - PAD - 30) / (1 + 2 * slope * w / h)  # from the pinhole to the picture in the box
    small_h = 2 * slope * reach
    small_w = small_h * w / h
    small_x, small_y = hole_x - reach - small_w, hole_y - small_h / 2
    data = base64.b64encode(jpeg).decode()
    out = f'''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 {WIDTH} {HEIGHT}" role="img">
<title>Pinhole, seen through a camera obscura</title>
<defs><image id="shot" width="{shot_w:.1f}" height="{shot_h}" preserveAspectRatio="none" xlink:href="data:image/jpeg;base64,{data}"/></defs>
<rect width="{WIDTH}" height="{HEIGHT}" rx="28" fill="{YELLOW}"/>
{text(PAD, 62, "iCloud Photos and folders today.", 30, BOLD, INK)}
{text(WIDTH - PAD, 62, "More sources coming.", 30, BOLD, INK, "end")}
<rect x="{PAD}" y="{TOP}" width="{hole_x - PAD}" height="{shot_h}" rx="22" fill="{INK}"/>
<path d="M{hole_x} {hole_y} L{hole_x - reach:.1f} {small_y:.1f} L{hole_x - reach:.1f} {small_y + small_h:.1f} Z" fill="{YELLOW}" opacity="0.3"/>
<use xlink:href="#shot" opacity="0.8" transform="translate({small_x + small_w:.1f} {small_y + small_h:.1f}) rotate(180) scale({small_w / shot_w:.4f})"/>
<path d="M{shot_x:.1f} {TOP} L{hole_x} {hole_y} L{shot_x:.1f} {HEIGHT - PAD}" stroke="{INK}" stroke-width="2.5" fill="none"/>
<circle cx="{hole_x}" cy="{hole_y}" r="7" fill="{YELLOW}"/>
<rect x="{shot_x - 6:.1f}" y="{TOP - 6}" width="{shot_w + 12:.1f}" height="{shot_h + 12}" rx="14" fill="{INK}"/>
<use xlink:href="#shot" x="{shot_x:.1f}" y="{TOP}"/>
</svg>
'''
    Path(__file__).with_name("obscura.svg").write_text(out)


if __name__ == "__main__":
    main(*sys.argv[1:3])
