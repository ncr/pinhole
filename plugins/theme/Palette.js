.pragma library

// The palette: a photo's colours (magick's k-means, as a histogram) made
// the roles of Omarchy's colors.toml. The work is done in OKLCH, where a
// lightness is the lightness seen whatever the hue, so a ground can be
// darkened or lightened and keep its colour; and every text colour is held
// to a contrast with the ground it is read on (WCAG's), so a beige photo
// still gives text to read.

// The magick arguments that give the photo's colours: a small copy, in
// twelve clusters, as "count: (r,g,b) #RRGGBB …" lines.
function magickArgs(path) {
    return [path + "[0]", "-auto-orient", "-resize", "200x200>", "-colorspace", "sRGB", "-kmeans", "12",
            "-depth", "8", "-format", "%c", "histogram:info:-"]
}

// The histogram's colours, the most of the photo first: [{ weight, rgb }].
function parseHistogram(text) {
    const list = []
    let total = 0
    for (const line of text.split("\n")) {
        const m = line.match(/^\s*(\d+):.*#([0-9A-Fa-f]{6})/)
        if (!m)
            continue
        const count = parseInt(m[1])
        total += count
        list.push({ count: count, rgb: hexToRgb("#" + m[2]) })
    }
    return list.map(c => ({ weight: c.count / total, rgb: c.rgb, lch: rgbToOklch(c.rgb) }))
               .sort((a, b) => b.weight - a.weight)
}

// sRGB, OKLab and OKLCH (Björn Ottosson's).
function hexToRgb(hex) {
    return [1, 3, 5].map(i => parseInt(hex.substr(i, 2), 16) / 255)
}
function rgbToHex(rgb) {
    return "#" + rgb.map(v => Math.round(clamp(v, 0, 1) * 255).toString(16).padStart(2, "0")).join("")
}
function clamp(v, lo, hi) { return Math.min(hi, Math.max(lo, v)) }
function toLinear(v) { return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4) }
function fromLinear(v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * Math.pow(v, 1 / 2.4) - 0.055 }

function rgbToOklch(rgb) {
    const [r, g, b] = rgb.map(toLinear)
    const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    const L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s
    const A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s
    const B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s
    const h = Math.atan2(B, A) * 180 / Math.PI
    return { l: L, c: Math.hypot(A, B), h: h < 0 ? h + 360 : h }
}
// Linear sRGB, which may be out of gamut.
function oklchToLinear(lch) {
    const A = lch.c * Math.cos(lch.h * Math.PI / 180), B = lch.c * Math.sin(lch.h * Math.PI / 180)
    const l = Math.pow(lch.l + 0.3963377774 * A + 0.2158037573 * B, 3)
    const m = Math.pow(lch.l - 0.1055613458 * A - 0.0638541728 * B, 3)
    const s = Math.pow(lch.l - 0.0894841775 * A - 1.2914855480 * B, 3)
    return [4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s]
}
function inGamut(lin) { return lin.every(v => v >= -0.0001 && v <= 1.0001) }
// A colour as near as sRGB has it: the chroma lowered until it fits.
function oklchToRgb(lch) {
    let lin = oklchToLinear(lch)
    if (!inGamut(lin)) {
        let lo = 0, hi = lch.c
        for (let i = 0; i < 24; ++i) {
            const mid = (lo + hi) / 2
            if (inGamut(oklchToLinear({ l: lch.l, c: mid, h: lch.h })))
                lo = mid
            else
                hi = mid
        }
        lin = oklchToLinear({ l: lch.l, c: lo, h: lch.h })
    }
    return lin.map(v => fromLinear(clamp(v, 0, 1)))
}
function hex(lch) { return rgbToHex(oklchToRgb(lch)) }
function lch(l, c, h) { return { l: clamp(l, 0, 1), c: Math.max(0, c), h: ((h % 360) + 360) % 360 } }

// WCAG's relative luminance and contrast ratio.
function luminance(rgb) {
    const [r, g, b] = rgb.map(toLinear)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b
}
function contrast(a, b) {
    const la = luminance(hexToRgb(a)), lb = luminance(hexToRgb(b))
    return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
}
// `colour` made lighter (on a dark ground) or darker (on a light one) until
// it reads at `ratio` on every one of `grounds`: its hue and chroma kept.
function readable(colour, grounds, ratio, dark) {
    let c = colour
    for (let i = 0; i < 100 && grounds.some(g => contrast(hex(c), g) < ratio); ++i)
        c = lch(c.l + (dark ? 0.01 : -0.01), c.c, c.h)
    return c
}

function hueDistance(a, b) {
    const d = Math.abs(a - b) % 360
    return d > 180 ? 360 - d : d
}
// From hue `a` toward `b` by `t`, the short way round.
function hueToward(a, b, t) {
    let d = b - a
    if (d > 180) d -= 360
    if (d < -180) d += 360
    return (a + d * t + 360) % 360
}

// The hue most of `colours` are near (within 30°), each counted by how much
// of the photo it is and how colourful: { h, c }, or null for none.
function hueGroup(colours) {
    const near = (a, b) => hueDistance(a.lch.h, b.lch.h) <= 30
    const mass = c => c.weight * c.lch.c
    let best = null, bestMass = 0
    for (const centre of colours) {
        const m = colours.filter(c => near(c, centre)).reduce((sum, c) => sum + mass(c), 0)
        if (m > bestMass) {
            best = centre
            bestMass = m
        }
    }
    if (!best)
        return null
    const group = colours.filter(c => near(c, best))
    let x = 0, y = 0, c = 0, w = 0
    for (const g of group) {
        x += mass(g) * Math.cos(g.lch.h * Math.PI / 180)
        y += mass(g) * Math.sin(g.lch.h * Math.PI / 180)
        c += g.weight * g.lch.c
        w += g.weight
    }
    const h = Math.atan2(y, x) * 180 / Math.PI
    return { h: h < 0 ? h + 360 : h, c: c / w }
}

// The named colours: their hue in OKLCH, and how far a photo may pull it
// and still have it be that colour.
var NAMED = {
    red: { h: 25, pull: 14 },
    orange: { h: 55, pull: 12 },
    yellow: { h: 95, pull: 12 },
    green: { h: 145, pull: 20 },
    cyan: { h: 200, pull: 16 },
    blue: { h: 260, pull: 18 },
    magenta: { h: 330, pull: 18 }
}

// The palette for a photo's colours. `strong` keeps the photo's colour as
// it is; otherwise it is muted. `mode` is "dark", "light", or "auto": how
// light the photo is, over all of it, says which.
function make(colours, strong, mode) {
    const lightness = colours.reduce((sum, c) => sum + c.weight * c.lch.l, 0)
    const dark = mode === "dark" || (mode !== "light" && lightness < 0.66)
    const vivid = strong ? 1 : 0.6

    // The ground: the photo's dominant colour, darkened or lightened, from
    // its own shadows for a dark theme (a sunset's plum, not its orange)
    // and its own highlights for a light one. K-means cuts a gradient into
    // many clusters, so the colour is the hue most of them are near.
    const side = colours.filter(c => dark ? c.lch.l < 0.55 : c.lch.l > 0.6)
    const tinted = (side.length > 0 ? side : colours).filter(c => c.lch.c > 0.015)
    const dominant = hueGroup(tinted) || { h: 0, c: 0 }
    const groundHue = dominant.h
    const groundChroma = Math.min(dominant.c * (dark ? 0.6 : 0.4), dark ? (strong ? 0.06 : 0.025) : (strong ? 0.04 : 0.018))
    const g = (l, c = groundChroma) => hex(lch(l, c, groundHue))
    const background = g(dark ? 0.18 : 0.965)
    const ground = {
        background: background,
        dark_background: g(dark ? 0.155 : 0.935),
        darker_background: g(dark ? 0.13 : 0.905),
        lighter_background: g(dark ? 0.235 : 0.915, groundChroma * 1.3)
    }
    const grounds = [ground.background, ground.dark_background, ground.darker_background, ground.lighter_background]

    // The accent: the most colourful of the photo, if enough of it is
    // there to be its; a little of a strong colour beats much of a weak one.
    const candidates = colours.filter(c => c.weight >= 0.015 && c.lch.c > 0.035)
    const score = c => c.lch.c * Math.pow(c.weight, 0.25)
    const accentSource = candidates.length > 0 ? candidates.reduce((a, b) => score(b) > score(a) ? b : a) : null
    // A photo with little colour gives an accent with as little, and a
    // grey or black-and-white one a grey accent: a theme without colour.
    const accentHue = accentSource ? accentSource.lch.h : groundHue
    const sourceChroma = accentSource ? accentSource.lch.c : 0
    const accentChroma = !accentSource ? Math.min(dominant.c, 0.02)
                       : sourceChroma < 0.06 ? sourceChroma * 2 * vivid
                       : Math.max(sourceChroma, 0.13) * vivid + (strong ? 0.02 : 0)
    const accentLch = readable(lch(dark ? (accentSource ? 0.74 : 0.82) : (accentSource ? 0.52 : 0.42), Math.min(accentChroma, 0.2), accentHue),
                               [background], 4.5, dark)
    const accent = hex(accentLch)

    // Text: from the ground's hue, a little of it, read at 7:1 on the ground
    // and 4.5:1 on every surface (AAA and AA).
    const textChroma = Math.min(groundChroma * 0.6, 0.025)
    const t = (l, ratio, on) => hex(readable(lch(l, textChroma, groundHue), on, ratio, dark))
    const foreground = t(dark ? 0.9 : 0.3, 7, [background])
    const text = {
        foreground: contrast(foreground, ground.lighter_background) >= 4.5 ? foreground : t(dark ? 0.9 : 0.3, 4.5, grounds),
        dark_foreground: t(0.6, 4.5, [background]),
        light_foreground: t(dark ? 0.8 : 0.42, 6, [background]),
        bright_foreground: t(dark ? 0.98 : 0.2, 9, [background])
    }

    // The selection: the ground with the accent in it, the text still
    // read on it.
    let selectionLch = lch(dark ? 0.3 : 0.86, Math.min(accentLch.c * 0.45, 0.08), accentHue)
    for (let i = 0; i < 40 && contrast(text.foreground, hex(selectionLch)) < 4.5; ++i)
        selectionLch = lch(selectionLch.l + (dark ? -0.01 : 0.01), selectionLch.c, selectionLch.h)
    const selection = hex(selectionLch)
    const muted = hex(readable(lch(dark ? 0.45 : 0.72, groundChroma * 1.2 + 0.02, groundHue), [background], 2.6, dark))

    // The named colours keep what they mean, each pulled toward the photo's
    // nearest hue of its own, as far as its pull lets it.
    const photoHues = colours.filter(c => c.weight >= 0.01 && c.lch.c > 0.03)
    const named = {}
    for (const name in NAMED) {
        const base = NAMED[name]
        const near = photoHues.filter(c => hueDistance(c.lch.h, base.h) <= base.pull * 2.5)
                              .sort((a, b) => hueDistance(a.lch.h, base.h) - hueDistance(b.lch.h, base.h))[0]
        let h = base.h
        if (near) {
            const d = hueDistance(near.lch.h, base.h)
            h = hueToward(base.h, near.lch.h, Math.min(1, base.pull / Math.max(d, 1)) * (strong ? 0.9 : 0.6))
        }
        // The photo's own chroma there, when it has that colour; the
        // terminal still needs every colour to be told apart.
        const c = Math.max(near ? near.lch.c : 0, name === "yellow" ? 0.13 : 0.11) * vivid + (strong ? 0.03 : 0.02)
        const l = dark ? (name === "yellow" ? 0.84 : name === "blue" ? 0.7 : 0.74) : (name === "yellow" ? 0.6 : 0.52)
        named[name] = readable(lch(l, Math.min(c, 0.2), h), [background], 4.5, dark)
    }
    named.brown = readable(lch(dark ? 0.62 : 0.42, 0.08 * vivid + 0.01, hueToward(60, named.orange.h, 0.5)), [background], 3.5, dark)

    // The border: the accent running into the photo's next colour, one a
    // hue apart from it.
    const second = candidates.filter(c => hueDistance(c.lch.h, accentHue) > 40)
                             .reduce((a, b) => !a || score(b) > score(a) ? b : a, null)
    const borderEnd = second ? hex(readable(lch(accentLch.l, Math.max(second.lch.c, 0.1) * vivid, second.lch.h), [background], 3, dark))
                    : accentSource ? hex(named.magenta)
                    : hex(lch(dark ? accentLch.l - 0.25 : accentLch.l + 0.25, accentLch.c, accentLch.h))
    const strip = h => h.substr(1)

    const p = {
        mode: dark ? "dark" : "light",
        accent: accent,
        selection: selection,
        muted: muted
    }
    Object.assign(p, ground, text)
    p.hyprland_active_border = "rgba(" + strip(accent) + "ee) rgba(" + strip(borderEnd) + "ee) 45deg"
    p.hyprland_inactive_border = "rgba(" + strip(muted) + "66)"
    for (const name of ["red", "orange", "yellow", "green", "cyan", "blue", "magenta", "brown"])
        p[name] = hex(named[name])
    // Bright: away from the ground, lighter on a dark one.
    for (const name of ["red", "yellow", "green", "cyan", "blue", "magenta"]) {
        const c = named[name]
        p["bright_" + name] = hex(lch(c.l + (dark ? 0.1 : -0.08), c.c * 0.85, c.h))
    }
    return p
}

// The order and the comments of colors.toml, as Omarchy's own are.
function toml(p, title) {
    const line = (key, value) => key.padEnd(25) + "= \"" + value + "\"\n"
    const group = (comment, keys) => "\n# " + comment + "\n" + keys.map(k => line(k, p[k])).join("")
    return "# " + title + ", made by Pinhole from a photo.\n\n"
        + line("mode", p.mode)
        + group("Accent, selection and the de-emphasised ramp", ["accent", "selection", "muted"])
        + group("Ground, from the deepest surface up to the elevated one", ["background", "dark_background", "darker_background", "lighter_background"])
        + group("Text, from de-emphasised up to the brightest", ["foreground", "dark_foreground", "light_foreground", "bright_foreground"])
        + group("Window borders", ["hyprland_active_border", "hyprland_inactive_border"])
        + group("Named colours, pulled toward the photo's", ["red", "orange", "yellow", "green", "cyan", "blue", "magenta", "brown"])
        + group("Bright variants", ["bright_red", "bright_yellow", "bright_green", "bright_cyan", "bright_blue", "bright_magenta"])
}

// Yaru's icons in the accent's colour, as Omarchy's themes name them.
function icons(p) {
    const c = rgbToOklch(hexToRgb(p.accent))
    if (c.c < 0.05)
        return "Yaru"
    const names = [[40, "Yaru-red"], [75, "Yaru-wartybrown"], [110, "Yaru-yellow"], [135, "Yaru-olive"],
                   [170, "Yaru-sage"], [215, "Yaru-prussiangreen"], [285, "Yaru-blue"], [320, "Yaru-purple"],
                   [355, "Yaru-magenta"], [360, "Yaru-red"]]
    return names.find(n => c.h < n[0])[1]
}

// The colours the preview shows, under the photo.
function swatches(p) {
    return [p.background, p.lighter_background, p.selection, p.muted, p.foreground, p.accent,
            p.red, p.orange, p.yellow, p.green, p.cyan, p.blue, p.magenta]
}
