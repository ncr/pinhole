#!/usr/bin/env python3
"""Draws the cards of docs/tour/README.md: each plugin's name, its keys and one line, over its clip.

Run it after a plugin comes, goes, changes its key or its line: docs/tour/cards.py. The plugins and their keys,
in their order, are docs/readme/plugins.py's; the lines are LINES below. A plugin whose clip (docs/tour/<slug>.webp)
is not recorded yet gets a frame that says so, and the page is written again with every card in order.
"""
import sys
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE.parent / "readme"))
from fontTools.ttLib import TTFont  # noqa: E402
from keys import BOLD, EDGE, GREY, INK, REGULAR, YELLOW, text  # noqa: E402
from plugins import PAPER, PLUGINS, width  # noqa: E402

DISPLAY = TTFont(HERE.parent / "readme" / "BricolageGrotesque-ExtraBold.ttf")
WIDTH, LEFT, HEIGHT = 1680, 56, 176
NAME, LINE, KEY, CAP, GAP = 56, 27, 28, 64, 12
SOFT = "#c9c8c2"  # the line: quieter than the name, still easy to read

LINES = {
    "Favourites": "A heart on the photos you love, the same one your iPhone shows.",
    "Turn and crop": "Turn a photo or crop it. Your iPhone sees the change, and Ctrl Z takes it back.",
    "Info": "When a photo was taken, with which camera and settings, and where.",
    "Send to phone": "Sends photos to a phone or computer nearby that has LocalSend open.",
    "Screenshots": "Screenshots folded into one card a day, or out of the grid altogether.",
    "Search": "Describe a photo in your own words, and it comes first.",
    "People": "Faces grouped by person. Name them once, and your other computers know them too.",
    "Live Photos": "A Live Photo moves, and plays as a loop, a bounce or a long exposure.",
    "Map": "Your photos on a map, drawn in your theme's colours.",
    "On this day": "What you photographed on this date in earlier years.",
    "Series": "Shots taken one after another become one card, and a short GIF to send.",
    "Contact sheet": "The photos you ticked, laid out as one picture to send.",
    "Year calendar": "Every day of every year as a square, brighter the more photos it has.",
    "Photo frame": "Your photos one after another, slowly moving, for a laptop left open.",
    "Background": "The photo you are looking at becomes the desktop background.",
    "Theme": "A whole Omarchy theme made from one photo's colours. Ctrl Z goes back.",
    "Rainbow wall": "Your most colourful photos, laid out by colour.",
    "Mosaic": "Any photo built from hundreds of your other photos.",
}

INTRO = """# Pinhole's plugins

Every plugin is one key away. They go from the ones I use every day to the ones that exist because I could
imagine them. Each clip below is the real application, recorded on a library of a thousand of my photos where
the only face is mine.

<img src="../readme/plugins.svg" alt="{alt}" width="100%">
"""

OUTRO = """
## More about them

- Every key and setting of each plugin: [What Pinhole does](../features.md)
- Turn plugins on and off in Pinhole's side panel: press <kbd>Tab</kbd>.
- Want one that is not here? [Build it with your AI agent](../../README.md#build-a-plugin-with-your-ai-agent).
"""


def slug(name):
    return name.lower().replace(" ", "-")


def caps(keys):
    """'R K' is two keys, 'Ctrl F' and '⇧ S' one each."""
    out, words = [], keys.split()
    while words:
        word = words.pop(0)
        out.append(f"{word} {words.pop(0)}" if word in ("Ctrl", "⇧") and words else word)
    return out


def card(name, keys, line):
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {HEIGHT}" role="img">',
           f"<title>{name}: {' '.join(caps(keys))}. {line}</title>",
           f'<rect width="{WIDTH}" height="{HEIGHT}" rx="28" fill="{INK}"/>',
           text(LEFT, 82, name, NAME, DISPLAY, PAPER),
           text(LEFT, 136, line, LINE, REGULAR, SOFT)]
    right = WIDTH - LEFT
    for cap in reversed(caps(keys)):
        w = max(CAP, width(cap, KEY, BOLD) + 32)
        x = right - w
        out += [f'<rect x="{x:.1f}" y="30" width="{w:.1f}" height="{CAP}" rx="12" fill="{YELLOW}"/>',
                text(x + (w - width(cap, KEY, BOLD)) / 2, 30 + CAP / 2 + KEY * 0.36, cap, KEY, BOLD, INK)]
        right = x - GAP
    out.append("</svg>")
    return "\n".join(out) + "\n"


def coming():
    h = 220
    return "\n".join([
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {h}" role="img">',
        "<title>A clip of this plugin is still to be recorded</title>",
        f'<rect x="2" y="2" width="{WIDTH - 4}" height="{h - 4}" rx="26" fill="none" stroke="{EDGE}" stroke-width="3"/>',
        text(WIDTH / 2 + width("A clip of this one is still to be recorded", 26, REGULAR) / 2, h / 2 + 9,
             "A clip of this one is still to be recorded", 26, REGULAR, GREY, "end"),
        "</svg>"]) + "\n"


def main():
    (HERE / "coming.svg").write_text(coming())
    alt = "Plugins, from useful every day to imagined it, so I built it: " + ", ".join(
        f"{n} {k}" for n, k in PLUGINS)
    page = [INTRO.format(alt=alt)]
    for name, keys in PLUGINS:
        s = slug(name)
        line = LINES[name]
        (HERE / f"{s}.svg").write_text(card(name, keys, line))
        clip = HERE / f"{s}.webp"
        shown = (f'<img src="{s}.webp" alt="{name} at work in Pinhole" width="100%">' if clip.exists()
                 else '<img src="coming.svg" alt="A clip of this plugin is still to be recorded" width="100%">')
        page.append(f'<img src="{s}.svg" alt="{name}, {" ".join(caps(keys))}: {line}" width="100%">\n{shown}\n')
    page.append(OUTRO)
    (HERE / "README.md").write_text("\n".join(page))


if __name__ == "__main__":
    main()
