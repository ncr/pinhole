.pragma library

// Colours as the eye sees them: OKLab (Björn Ottosson's), in its polar
// form, lightness L (0 black to 1 white), chroma C (0 grey, a photo's
// average rarely past 0.15) and hue h in degrees.

// sRGB's 0..255 to linear light, looked up: the library's colours are
// read 50,000 at a time.
const LINEAR = (() => {
    const table = new Float64Array(256)
    for (let i = 0; i < 256; ++i) {
        const c = i / 255
        table[i] = c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4)
    }
    return table
})()

// 0xrrggbb to [L, a, b]; into `out` from `at` when given, for many at
// once without an array each.
function lab(rgb, out, at) {
    const r = LINEAR[rgb >> 16 & 255], g = LINEAR[rgb >> 8 & 255], b = LINEAR[rgb & 255]
    const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    const L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720040 * s
    const A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s
    const B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s
    if (!out)
        return [L, A, B]
    out[at] = L
    out[at + 1] = A
    out[at + 2] = B
    return out
}

function chroma(a, b) { return Math.sqrt(a * a + b * b) }
function hue(a, b) {
    const h = Math.atan2(b, a) * 180 / Math.PI
    return h < 0 ? h + 360 : h
}

// The rainbow wall only shows photos at least this colourful.
const wallChroma = 0.03

// The library's colours, for the wall: each photo's id, its colour and when
// it was taken, for those that have a colour. `colours` is each position's
// "#rrggbb" or "", `dates` its date in ms, `ids` its id (the library's
// columns, library.field). Ids, not positions: positions move when the
// library is read again, and what was found stays what it was. `pos` is
// where each was as it was read, to ask whether it is hidden from the grid
// (withHidden); `shown` which are not, null for all.
function index(colours, dates, ids) {
    let n = 0
    for (let i = 0; i < colours.length; ++i)
        if (colours[i] !== "")
            ++n
    const data = { n: n, id: new Array(n), pos: new Int32Array(n), L: new Float32Array(n), a: new Float32Array(n),
                   b: new Float32Array(n), C: new Float32Array(n), h: new Float32Array(n), date: new Float64Array(n),
                   shown: null, count: 0, colourful: 0, wheel: null }
    let k = 0
    const p = new Float64Array(3)
    for (let i = 0; i < colours.length; ++i) {
        if (colours[i] === "")
            continue
        lab(parseInt(colours[i].slice(1), 16), p, 0)
        data.id[k] = ids[i]
        data.pos[k] = i
        data.L[k] = p[0]
        data.a[k] = p[1]
        data.b[k] = p[2]
        data.C[k] = chroma(p[1], p[2])
        data.h[k] = hue(p[1], p[2])
        data.date[k] = dates[i]
        ++k
    }
    tally(data)
    return data
}
function shown(data, k) { return data.shown === null || data.shown[k] === 1 }
// How many are shown, and how many of those are colourful enough for the
// wall.
function tally(data) {
    data.count = 0
    data.colourful = 0
    for (let k = 0; k < data.n; ++k)
        if (shown(data, k)) {
            ++data.count
            if (data.C[k] >= wallChroma)
                ++data.colourful
        }
}

// The index with the photos hidden from the grid left out of the wall,
// `isHidden(position)` saying which (null: none is): a new one, its colours
// shared, when that changes which are, else the same one.
function withHidden(data, isHidden) {
    let mask = null
    if (isHidden)
        for (let k = 0; k < data.n; ++k)
            if (isHidden(data.pos[k])) {
                if (mask === null)
                    mask = new Uint8Array(data.n).fill(1)
                mask[k] = 0
            }
    const before = data.shown
    if (before === mask || (before !== null && mask !== null && before.every((v, k) => v === mask[k])))
        return data
    const next = Object.assign({}, data)
    next.shown = mask
    next.wheel = null
    tally(next)
    return next
}

// Chroma in steps, for counting sorts: 512 of them up to past the most a
// screen shows.
const chromaSteps = 512
const chromaTop = 0.33
function chromaStep(C) { return Math.min(chromaSteps - 1, Math.floor(C / chromaTop * chromaSteps)) }

// The colourful photos shown by whole degree of hue, each degree's most
// colourful first: `order` the photos, `from[d]` where degree d's start
// (360 for the end), and `count[d]` how many. Made once for an index, by
// counting (QML's JavaScript sorts only by comparing, and too slowly for
// 50,000).
function wheel(data) {
    if (data.wheel)
        return data.wheel
    const place = new Int32Array(360 * chromaSteps + 1)
    const key = k => (Math.floor(data.h[k]) % 360) * chromaSteps + (chromaSteps - 1 - chromaStep(data.C[k]))
    let n = 0
    for (let k = 0; k < data.n; ++k)
        if (data.C[k] >= wallChroma && shown(data, k)) {
            ++place[key(k) + 1]
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
        if (data.C[k] >= wallChroma && shown(data, k))
            order[place[key(k)]++] = k
    data.wheel = { order: order, from: from, count: count, n: n }
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
