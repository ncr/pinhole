.pragma library

// Colours as the eye sees them: OKLab (Björn Ottosson's), in which the
// distance between two colours is close to how different they look, and
// its polar form, lightness L (0 black to 1 white), chroma C (0 grey, a
// photo's average rarely past 0.15) and hue h in degrees.

function linear(c) {
    c /= 255
    return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4)
}
function gamma(c) {
    const v = c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055
    return Math.round(255 * Math.max(0, Math.min(1, v)))
}

// 0xrrggbb to [L, a, b].
function lab(rgb) {
    const r = linear(rgb >> 16 & 255), g = linear(rgb >> 8 & 255), b = linear(rgb & 255)
    const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    return [0.2104542553 * l + 0.7936177850 * m - 0.0040720040 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s]
}

// [L, a, b] to linear sRGB, each 0..1 when it is a colour a screen shows.
function linearRgb(L, a, b) {
    const l = Math.pow(L + 0.3963377774 * a + 0.2158037573 * b, 3)
    const m = Math.pow(L - 0.1055613458 * a - 0.0638541728 * b, 3)
    const s = Math.pow(L - 0.0894841775 * a - 1.2914855480 * b, 3)
    return [4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s]
}
function inGamut(L, a, b) {
    const c = linearRgb(L, a, b)
    return c[0] >= -1e-4 && c[0] <= 1.0001 && c[1] >= -1e-4 && c[1] <= 1.0001 && c[2] >= -1e-4 && c[2] <= 1.0001
}

// [L, a, b] to [r, g, b] 0..255, clipped.
function rgb(L, a, b) {
    const c = linearRgb(L, a, b)
    return [gamma(c[0]), gamma(c[1]), gamma(c[2])]
}
function hex(L, a, b) {
    const c = rgb(L, a, b)
    return "#" + ((1 << 24) | (c[0] << 16) | (c[1] << 8) | c[2]).toString(16).slice(1)
}
function fromPolar(L, C, h) {
    const r = h * Math.PI / 180
    return [L, C * Math.cos(r), C * Math.sin(r)]
}
function chroma(a, b) { return Math.sqrt(a * a + b * b) }
function hue(a, b) {
    const h = Math.atan2(b, a) * 180 / Math.PI
    return h < 0 ? h + 360 : h
}

// The most chroma a screen shows at this lightness and hue.
function maxChroma(L, h) {
    let lo = 0, hi = 0.4
    for (let k = 0; k < 14; ++k) {
        const mid = (lo + hi) / 2
        const p = fromPolar(L, mid, h)
        if (inGamut(p[0], p[1], p[2]))
            lo = mid
        else
            hi = mid
    }
    return lo
}

// A colour as it shows at its hue and chroma, its lightness moved to
// where a screen can show that chroma: the colour a hue "is".
function hexOf(L, C, h) {
    const p = fromPolar(L, Math.min(C, maxChroma(L, h)), h)
    return hex(p[0], p[1], p[2])
}

// How far apart two colours look. Lightness counts for half: an average
// colour's lightness says more about how the photo was lit than what is
// in it, and finding by colour is mostly finding by hue.
const lightnessWeight = 0.5
function distance2(L1, a1, b1, L2, a2, b2) {
    const dL = (L1 - L2) * lightnessWeight, da = a1 - a2, db = b1 - b2
    return dL * dL + da * da + db * db
}

// The name of a hue, by OKLab's angles for red (29), yellow (110), green
// (142), cyan (195), blue (264) and magenta (328); grey below this chroma.
const greyBelow = 0.02
const HUES = [[15, "pink"], [42, "red"], [70, "orange"], [125, "yellow"], [175, "green"], [230, "cyan"],
              [280, "blue"], [315, "violet"], [345, "magenta"], [360, "pink"]]
function hueName(C, h) {
    if (C < greyBelow)
        return "grey"
    for (const [end, name] of HUES)
        if (h < end)
            return name
    return "pink"
}

// The library's colours, for finding: a photo's position, its colour and
// when it was taken, for those that have a colour. `colours` is each
// position's "#rrggbb" or "", `dates` its date in ms.
function index(colours, dates) {
    const n = colours.filter(c => c !== "").length
    const data = { n: n, at: new Int32Array(n), L: new Float32Array(n), a: new Float32Array(n), b: new Float32Array(n),
                   C: new Float32Array(n), h: new Float32Array(n), date: new Float64Array(n) }
    let k = 0
    for (let i = 0; i < colours.length; ++i) {
        if (colours[i] === "")
            continue
        const p = lab(parseInt(colours[i].slice(1), 16))
        data.at[k] = i
        data.L[k] = p[0]
        data.a[k] = p[1]
        data.b[k] = p[2]
        data.C[k] = chroma(p[1], p[2])
        data.h[k] = hue(p[1], p[2])
        data.date[k] = dates ? dates[i] : 0
        ++k
    }
    return data
}

// The `count` photos nearest [L, a, b], nearest first, as positions; not
// `skip`.
function nearest(data, L, a, b, count, skip) {
    const best = [], far = []
    for (let k = 0; k < data.n; ++k) {
        if (data.at[k] === skip)
            continue
        const d = distance2(data.L[k], data.a[k], data.b[k], L, a, b)
        if (best.length === count && d >= far[count - 1])
            continue
        let j = best.length === count ? count - 1 : best.length
        while (j > 0 && far[j - 1] > d) {
            best[j] = best[j - 1]
            far[j] = far[j - 1]
            --j
        }
        best[j] = data.at[k]
        far[j] = d
    }
    return best
}

// How many photos have a colour clear enough to find by: their average
// more than grey.
function colourful(data) {
    let n = 0
    for (let k = 0; k < data.n; ++k)
        if (data.C[k] >= greyBelow)
            ++n
    return n
}

// The rainbow wall: `columns` × `rows` photos, each column a slice of
// the hues from `start` round, its photos the most colourful of that hue,
// light at the top and dark at the foot. A library's hues are anything
// but even (skin, wood and sunsets: mostly orange), so the slices are as
// wide two parts by the circle and one by how many photos have each hue: the
// wall goes round the whole rainbow, and has more room for the hues a
// library has more of. A burst of shots of one thing takes one tile: of
// photos taken within minutes of each other, only the most colourful is
// on the wall. `seed`, past 0, picks among more of each slice's
// colourful photos, for another wall. Positions, column by column; -1
// where a slice runs out.
function wall(data, columns, rows, start, seed) {
    const turn = h => (h - start + 360) % 360
    const bins = new Float64Array(360)
    let colourfulCount = 0
    for (let k = 0; k < data.n; ++k)
        if (data.C[k] >= 0.03) {
            bins[Math.floor(turn(data.h[k])) % 360] += 1
            ++colourfulCount
        }
    // Where each slice starts, by the mixed share of the circle.
    const edges = [0]
    let share = 0, j = 1
    for (let d = 0; d < 360 && j < columns; ++d) {
        share += 0.65 / 360 + 0.35 * (colourfulCount > 0 ? bins[d] / colourfulCount : 1 / 360)
        while (j < columns && share >= j / columns) {
            edges.push(d + 1)
            ++j
        }
    }
    while (edges.length < columns)
        edges.push(360)
    edges.push(360)
    const slices = []
    for (let c = 0; c < columns; ++c)
        slices.push([])
    let c = 0
    const order = Array.from({ length: data.n }, (_, k) => k).sort((x, y) => turn(data.h[x]) - turn(data.h[y]))
    for (const k of order) {
        const h = turn(data.h[k])
        while (c < columns - 1 && h >= edges[c + 1])
            ++c
        slices[c].push(k)
    }
    let random = seed
    const next = () => {
        random = (Math.imul(random, 1103515245) + 12345) & 0x7fffffff
        return random / 2147483648
    }
    // Taken by five minutes: a photo whose five minutes, or those either
    // side, have one on the wall already is left off.
    const bucket = k => Math.floor(data.date[k] / 300000)
    const taken = new Set()
    const free = k => data.date[k] === 0 || !(taken.has(bucket(k)) || taken.has(bucket(k) - 1) || taken.has(bucket(k) + 1))
    const out = []
    for (const slice of slices) {
        slice.sort((x, y) => data.C[y] - data.C[x])
        // The most colourful, or for another wall any of six times as
        // many, in a shuffled order.
        let pool = slice.slice(0, seed > 0 ? rows * 6 : slice.length)
        if (seed > 0)
            for (let i = pool.length - 1; i > 0; --i) {
                const r = Math.floor(next() * (i + 1))
                const t = pool[i]
                pool[i] = pool[r]
                pool[r] = t
            }
        const pick = []
        for (let i = 0; i < pool.length && pick.length < rows; ++i)
            if (free(pool[i])) {
                pick.push(pool[i])
                taken.add(bucket(pool[i]))
            }
        pick.sort((x, y) => data.L[y] - data.L[x])
        for (let r = 0; r < rows; ++r)
            out.push(r < pick.length ? data.at[pick[r]] : -1)
    }
    return out
}
