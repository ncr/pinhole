.pragma library

// The clip engine: where every shot of a series is at any moment of its
// clip. Clip.qml draws what this says, for the preview and for the file
// made, so the file is the picture seen.

var MOTIONS = ["flip", "bounce", "fade", "drift", "pile"]
var PACES = ["slow", "normal", "fast"]
var LOOKS = ["natural", "vivid", "warm", "mono", "film"]
var SHAPES = ["original", "square", "tall", "story"]

var DEFAULT_RECIPE = { motion: "bounce", pace: "normal", look: "natural", shape: "original", steady: true, pan: { x: 0, y: 0 } }

// Width over height; 0 keeps the photos' own.
var RATIOS = { original: 0, square: 1, tall: 4 / 5, story: 9 / 16 }

// How each look is drawn (QtQuick.Effects' MultiEffect): -1 to 1 each.
var EFFECTS = {
    natural: null,
    vivid: { saturation: 0.32, contrast: 0.08, brightness: 0, colorization: 0, color: "#000000" },
    warm: { saturation: 0.18, contrast: 0.02, brightness: 0.03, colorization: 0.16, color: "#ff9440" },
    mono: { saturation: -1, contrast: 0.16, brightness: 0.02, colorization: 0, color: "#000000" },
    film: { saturation: -0.22, contrast: -0.1, brightness: 0.05, colorization: 0.14, color: "#d8a466" },
}

// Seconds per shot, per pace (and for fades, seconds of the fade). Each
// shot's time is a multiple of 0.05 s and each loop a multiple of 0.2 s, so
// the frame rates used (15, 20, 30) keep time exactly and a loop joins
// without a hiccup.
const TIMING = {
    flip: { slow: [0.40], normal: [0.20], fast: [0.10] },
    bounce: { slow: [0.25], normal: [0.15], fast: [0.10] },
    fade: { slow: [1.10, 0.70], normal: [0.60, 0.40], fast: [0.30, 0.30] },
    drift: { slow: [1.60, 0.80], normal: [1.00, 0.60], fast: [0.60, 0.40] },
    pile: { slow: [0.80], normal: [0.60], fast: [0.40] },
}

function smooth(u) { return u * u * (3 - 2 * u) }
function clamp(v, a, b) { return Math.min(b, Math.max(a, v)) }
function wrap(t, d) { return ((t % d) + d) % d }

// The same numbers every time for a seed.
function random(seed) {
    let s = (seed >>> 0) || 1
    return function () {
        s ^= s << 13; s ^= s >>> 17; s ^= s << 5
        return ((s >>> 0) % 100000) / 100000
    }
}

// A frame taken at f / fps lands exactly on a boundary (0.6 / 0.2 =
// 2.9999…); the nudge keeps it on the right side.
const EPS = 1e-6

// A clip stays short whatever the number of shots: a long series plays
// faster rather than longer. Steps stay multiples of 0.2 s.
const LONGEST = 10
function fit(step, n, extra, least) {
    return n * step + extra <= LONGEST ? step : Math.max(least, Math.floor((LONGEST - extra) / n / 0.2) * 0.2)
}

// The clip's length and what shows at each moment of it: shot `a`, and
// for a fade shot `b` over it by `mix`; for a pile, the prints landed.
function timeline(recipe, n) {
    const timing = TIMING[recipe.motion][recipe.pace]
    const a = timing[0], b = timing[1] || 0
    if (n < 1)
        return { duration: 1, at: t => ({ a: 0, b: 0, mix: 0, landed: [], fadeOut: 0 }) }
    switch (recipe.motion) {
    case "flip": {
        const d = n * a
        return { duration: d, at: t => ({ a: Math.min(n - 1, Math.floor(wrap(t + EPS, d) / a)), mix: 0 }) }
    }
    case "bounce": {
        if (n < 2)
            return { duration: a, at: t => ({ a: 0, mix: 0 }) }
        // Forwards, then back; the two ends linger, as a boomerang does.
        const steps = []
        for (let i = 0; i < n; i++) steps.push(i)
        for (let i = n - 2; i > 0; i--) steps.push(i)
        const lengths = steps.map(s => (s === 0 || s === n - 1 ? a * 2 : a))
        const d = lengths.reduce((x, y) => x + y, 0)
        return {
            duration: d,
            at(t) {
                let u = wrap(t + EPS, d)
                for (let i = 0; i < steps.length; i++) {
                    if (u < lengths[i] - 1e-9)
                        return { a: steps[i], mix: 0 }
                    u -= lengths[i]
                }
                return { a: steps[0], mix: 0 }
            }
        }
    }
    case "fade":
    case "drift": {
        const full = a + b
        const step = fit(full, n, 0, 0.4)
        const fade = b * step / full
        const hold = step - fade
        const d = n * step
        return {
            duration: d,
            at(t) {
                const u0 = wrap(t + EPS, d)
                const i = Math.min(n - 1, Math.floor(u0 / step))
                const u = u0 - i * step
                const j = (i + 1) % n
                const mix = n > 1 && u > hold ? smooth((u - hold) / fade) : 0
                // How far each shot is through its own time on screen, 0..1.
                const span = step + fade
                return { a: i, b: j, mix: mix, pa: (u + fade) / span, pb: Math.max(0, u - hold) / span }
            }
        }
    }
    case "pile": {
        const land = 0.4, hold = 1.6, out = 0.6
        const every = fit(a, n, hold + out, 0.2)
        const d = n * every + hold + out
        return {
            duration: d,
            at(t) {
                const u = wrap(t, d)
                const landed = []
                for (let i = 0; i < n; i++) {
                    const k = (u - i * every) / land
                    if (k <= 0)
                        break
                    landed.push({ i: i, k: Math.min(1, k) })
                }
                const fadeOut = u > d - out ? smooth((u - (d - out)) / out) : 0
                return { landed: landed, fadeOut: fadeOut, a: landed.length ? landed[landed.length - 1].i : 0 }
            }
        }
    }
    }
}

// Width over height of the clip: the shape's, or the first shot's own.
function ratio(recipe, shots) {
    return RATIOS[recipe.shape] || (shots.length ? shots[0].aspect : 4 / 3)
}

// A shot of aspect `aspect` just covering a canvas cw × ch.
function cover(aspect, cw, ch) {
    return aspect > cw / ch ? { w: ch * aspect, h: ch } : { w: cw, h: cw / aspect }
}

// How much larger than covering the shots are drawn, so each still covers
// the canvas when moved by its steadying and the crop by (px, py); and how
// far the crop may move, each way, in canvas pixels.
function geometry(shots, recipe, cw, ch) {
    const steady = recipe.steady && recipe.motion !== "pile"
    const need = (px, py) => {
        let f = 1
        for (const s of shots) {
            const c = cover(s.aspect, cw, ch)
            const sx = steady ? Math.abs(s.shift.x) : 0, sy = steady ? Math.abs(s.shift.y) : 0
            f = Math.max(f, (cw + 2 * Math.abs(px)) / (c.w * (1 - 2 * sx)), (ch + 2 * Math.abs(py)) / (c.h * (1 - 2 * sy)))
        }
        return f
    }
    if (shots.length === 0 || cw <= 0 || ch <= 0)
        return { f: 1, px: 0, py: 0, room: { x: 0, y: 0 }, steady: steady }
    const c = cover(shots[0].aspect, cw, ch)
    const f0 = need(0, 0)
    const room = { x: Math.max(0, (c.w * f0 - cw) / 2), y: Math.max(0, (c.h * f0 - ch) / 2) }
    const pan = recipe.pan || { x: 0, y: 0 }
    const px = clamp(pan.x, -1, 1) * room.x, py = clamp(pan.y, -1, 1) * room.y
    return { f: need(px, py), px: px, py: py, room: room, steady: steady }
}

// Drift: each shot zooms in or out a little and moves a little, its own way.
function driftOf(i, p) {
    const r = random(i * 7919 + 13)
    const zoomIn = i % 2 === 0
    const angle = r() * Math.PI * 2
    const e = smooth(clamp(p, 0, 1))
    const z0 = zoomIn ? 1.04 : 1.12, z1 = zoomIn ? 1.12 : 1.04
    return { z: z0 + (z1 - z0) * e, mx: Math.cos(angle) * 0.015 * (e - 0.5) * 2, my: Math.sin(angle) * 0.015 * (e - 0.5) * 2 }
}

// Pile: where each print comes to rest, and from how far round it falls.
function printOf(i) {
    const r = random(i * 104729 + 7)
    return { rot: (r() - 0.5) * 14, x: (r() - 0.5) * 0.08, y: (r() - 0.5) * 0.08, from: (r() - 0.5) * 0.6 }
}

// Where shot i is at moment m: its picture's centre, size, turn and
// opacity, and its stacking; null when it does not show. A print is the
// photo with a white border around it.
function place(i, m, recipe, g, shots, cw, ch) {
    const shot = shots[i]
    if (!shot)
        return null
    if (recipe.motion === "pile") {
        const landed = (m.landed || []).find(l => l.i === i)
        if (!landed)
            return null
        const p = printOf(i)
        const e = 1 - Math.pow(1 - landed.k, 3)
        const fitted = Math.min(cw * 0.74 / shot.aspect, ch * 0.74)
        const h = fitted, w = fitted * shot.aspect
        return {
            x: cw / 2 + p.x * cw, y: ch / 2 + p.y * ch, w: w, h: h,
            scale: 1.28 - 0.28 * e, rotation: p.rot + p.from * 20 * (1 - e),
            opacity: Math.min(1, landed.k * 2.2) * (1 - m.fadeOut), z: i, print: true,
            border: Math.max(2, Math.round(Math.min(w, h) * 0.045)),
        }
    }
    let opacity, z = 1, extra = null
    if (i === m.a)
        opacity = 1
    else if (i === m.b && m.mix > 0)
        opacity = m.mix, z = 2
    else
        return null
    if (recipe.motion === "drift")
        extra = driftOf(i, i === m.a ? m.pa : m.pb)
    const c = cover(shot.aspect, cw, ch)
    const scale = g.f * (extra ? extra.z : 1)
    const w = c.w * scale, h = c.h * scale
    const sx = g.steady ? shot.shift.x : 0, sy = g.steady ? shot.shift.y : 0
    return {
        x: cw / 2 + g.px + (extra ? extra.mx * cw : 0) - sx * w,
        y: ch / 2 + g.py + (extra ? extra.my * ch : 0) - sy * h,
        w: w, h: h, scale: 1, rotation: 0, opacity: opacity, z: z, print: false, border: 0,
    }
}

// The file's size and frame rate. A GIF keeps every changed pixel, so it
// stays small (about 520 × 390); a video is 1080 on its short side, as
// phones expect.
function plan(recipe, shots, format) {
    const r = ratio(recipe, shots)
    let w, h
    if (format === "gif") {
        w = Math.sqrt(520 * 390 * r)
        h = w / r
    } else if (r >= 1) {
        h = 1080, w = 1080 * r
    } else {
        w = 1080, h = 1080 / r
    }
    w = Math.round(w), h = Math.round(h)
    w -= w % 2, h -= h % 2
    // Flip, Bounce and Pile change on 0.05 s steps, which 20 fps holds
    // exactly; fades are smooth enough at 15 in a GIF.
    const steps = recipe.motion === "flip" || recipe.motion === "bounce" || recipe.motion === "pile"
    const fps = steps ? 20 : format === "gif" ? 15 : 30
    const duration = timeline(recipe, shots.length).duration
    // A video plays a few times round, so it is worth pressing play for.
    const loops = format === "gif" ? 1 : Math.max(1, Math.min(8, Math.ceil(6 / duration)))
    return { w: w, h: h, fps: fps, frames: Math.max(1, Math.round(duration * loops * fps)) }
}
