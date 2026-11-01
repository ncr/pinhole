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

// The library's colours, for finding: each photo's id, its colour and when
// it was taken, for those that have a colour. `colours` is each position's
// "#rrggbb" or "", `dates` its date in ms, `ids` its id. Ids, not
// positions: positions move when the library is read again, and what was
// found stays what it was.
function index(colours, dates, ids) {
    let n = 0
    for (let i = 0; i < colours.length; ++i)
        if (colours[i] !== "")
            ++n
    const data = { n: n, id: new Array(n), L: new Float32Array(n), a: new Float32Array(n), b: new Float32Array(n),
                   C: new Float32Array(n), h: new Float32Array(n), date: new Float64Array(n), colourful: 0,
                   wheel: null }
    let k = 0
    for (let i = 0; i < colours.length; ++i) {
        if (colours[i] === "")
            continue
        const p = lab(parseInt(colours[i].slice(1), 16))
        data.id[k] = ids[i]
        data.L[k] = p[0]
        data.a[k] = p[1]
        data.b[k] = p[2]
        data.C[k] = chroma(p[1], p[2])
        data.h[k] = hue(p[1], p[2])
        data.date[k] = dates[i]
        if (data.C[k] >= greyBelow)
            ++data.colourful
        ++k
    }
    return data
}

// The `count` photos nearest [L, a, b], nearest first, as ids; not the
// photo `skip` (an id).
function nearest(data, L, a, b, count, skip) {
    const best = [], far = []
    for (let k = 0; k < data.n; ++k) {
        const d = distance2(data.L[k], data.a[k], data.b[k], L, a, b)
        if (best.length === count && d >= far[count - 1])
            continue
        if (data.id[k] === skip)
            continue
        let j = best.length === count ? count - 1 : best.length
        while (j > 0 && far[j - 1] > d) {
            best[j] = best[j - 1]
            far[j] = far[j - 1]
            --j
        }
        best[j] = k
        far[j] = d
    }
    return best.map(k => data.id[k])
}

// The rainbow wall only shows photos at least this colourful.
const wallChroma = 0.03

// Chroma in steps, for counting sorts: 512 of them up to past the most a
// screen shows.
const chromaSteps = 512
const chromaTop = 0.33
function chromaStep(C) { return Math.min(chromaSteps - 1, Math.floor(C / chromaTop * chromaSteps)) }

// The colourful photos by whole degree of hue, each degree's most
// colourful first: `order` the photos, `from[d]` where degree d's start
// (360 for the end), and `count[d]` how many; `steps[s]` how many there
// are at each step of chroma. Made once for an index, by counting (QML's
// JavaScript sorts only by comparing, and too slowly for 50,000).
function wheel(data) {
    if (data.wheel)
        return data.wheel
    const place = new Int32Array(360 * chromaSteps + 1)
    const steps = new Int32Array(chromaSteps)
    const key = k => (Math.floor(data.h[k]) % 360) * chromaSteps + (chromaSteps - 1 - chromaStep(data.C[k]))
    let n = 0
    for (let k = 0; k < data.n; ++k)
        if (data.C[k] >= wallChroma) {
            ++place[key(k) + 1]
            ++steps[chromaStep(data.C[k])]
            ++n
        }
    for (let i = 1; i < place.length; ++i)
        place[i] += place[i - 1]
    const order = new Int32Array(n), from = new Int32Array(361), count = new Int32Array(360)
    for (let d = 0; d <= 360; ++d)
        from[d] = place[d * chromaSteps]
    for (let d = 0; d < 360; ++d)
        count[d] = from[d + 1] - from[d]
    for (let k = 0; k < data.n; ++k)
        if (data.C[k] >= wallChroma)
            order[place[key(k)]++] = k
    data.wheel = { order: order, from: from, count: count, steps: steps, n: n }
    return data.wheel
}

// The rainbow wall: `columns` × `rows` photos, each column a slice of
// the hues from `start` round, its photos the most colourful of that hue,
// light at the top and dark at the foot. A library's hues are anything
// but even (skin, wood and sunsets: mostly orange), so the slices are as
// wide two parts by the circle and one by how many photos have each hue:
// the wall goes round the whole rainbow, and has more room for the hues a
// library has more of. A burst of shots of one thing takes one tile: of
// photos taken within minutes of each other, only the most colourful is
// on the wall. `seed`, past 0, picks among more of each slice's colourful
// photos, for another wall. Ids, column by column; "" where a slice runs
// out.
function wall(data, columns, rows, start, seed) {
    const w = wheel(data)
    const first = Math.round(start) % 360
    // Where each slice starts, in degrees from `start`, by the mixed share
    // of the circle.
    const edges = [0]
    let share = 0, j = 1
    for (let d = 0; d < 360 && j < columns; ++d) {
        share += 0.65 / 360 + 0.35 * (w.n > 0 ? w.count[(first + d) % 360] / w.n : 1 / 360)
        while (j < columns && share >= j / columns) {
            edges.push(d + 1)
            ++j
        }
    }
    while (edges.length < columns)
        edges.push(360)
    edges.push(360)
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
    for (let c = 0; c < columns; ++c) {
        // The slice's photos, most colourful first: its degrees' lists
        // merged, as far as they are wanted.
        const heads = []
        for (let d = edges[c]; d < edges[c + 1]; ++d) {
            const deg = (first + d) % 360
            if (w.count[deg] > 0)
                heads.push([w.from[deg], w.from[deg + 1]])
        }
        const take = () => {
            let best = -1
            for (let i = 0; i < heads.length; ++i)
                if (heads[i][0] < heads[i][1] && (best < 0 || data.C[w.order[heads[i][0]]] > data.C[w.order[heads[best][0]]]))
                    best = i
            return best < 0 ? -1 : w.order[heads[best][0]++]
        }
        const pick = []
        const choose = k => {
            if (free(k)) {
                pick.push(k)
                taken.add(bucket(k))
            }
        }
        // Another wall: any of the three times as many most colourful, in a
        // shuffled order, and then on down the slice if those run out.
        if (seed > 0) {
            const pool = []
            for (let k = take(); k >= 0 && pool.length < rows * 3; k = pool.length < rows * 3 ? take() : -1)
                pool.push(k)
            for (let i = pool.length - 1; i > 0; --i) {
                const r = Math.floor(next() * (i + 1))
                const t = pool[i]
                pool[i] = pool[r]
                pool[r] = t
            }
            for (let i = 0; i < pool.length && pick.length < rows; ++i)
                choose(pool[i])
        }
        for (let k = pick.length < rows ? take() : -1; k >= 0; k = pick.length < rows ? take() : -1)
            choose(k)
        pick.sort((x, y) => data.L[y] - data.L[x])
        for (let r = 0; r < rows; ++r)
            out.push(r < pick.length ? data.id[pick[r]] : "")
    }
    return out
}

// A photo to show what "same colour" does: one of many of much the same
// colour, clearly coloured but not the most (the most colourful are
// screens and stage lights), and of a hue from green to blue: what a
// photo takes from its subject (grass, sea, sky) rather than from its
// light, as the warm hues of lamplight, wood and skin are. Its id, or "".
function example(data) {
    const w = wheel(data)
    if (w.n === 0)
        return ""
    // How many photos fall in each small cell of the colour space; a
    // photo's neighbours are those in its cell and the cells around it.
    const size = 0.03
    const at = (L, a, b) => (Math.round(L * lightnessWeight / size) * 1000 + Math.round(a / size) + 500) * 1000 + Math.round(b / size) + 500
    const cells = new Map()
    for (let k = 0; k < data.n; ++k) {
        const c = at(data.L[k], data.a[k], data.b[k])
        cells.set(c, (cells.get(c) || 0) + 1)
    }
    const near = k => {
        const c = at(data.L[k], data.a[k], data.b[k])
        let n = 0
        for (const dL of [-1000000, 0, 1000000])
            for (const da of [-1000, 0, 1000])
                for (const db of [-1, 0, 1])
                    n += cells.get(c + dL + da + db) || 0
        return n
    }
    // Between the 90th and 95th centiles of chroma among the colourful.
    const centile = share => {
        let seen = 0
        for (let i = 0; i < chromaSteps; ++i) {
            seen += w.steps[i]
            if (seen > w.n * share)
                return (i + 0.5) * chromaTop / chromaSteps
        }
        return chromaTop
    }
    const low = centile(0.9), high = centile(0.95)
    let best = -1, most = 0
    for (let k = 0; k < data.n; ++k) {
        if (data.C[k] < low || data.C[k] > high || data.L[k] < 0.4 || data.L[k] > 0.85 || data.h[k] < 125 || data.h[k] >= 280)
            continue
        const n = near(k)
        if (n > most) {
            most = n
            best = k
        }
    }
    return best < 0 ? "" : data.id[best]
}
