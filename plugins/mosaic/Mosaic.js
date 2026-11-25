.pragma library

// A photo made of photos: which of the library's photos stand for which
// part of a picture, by colour.
//
// A photo's colour is the average of its thumbnail, which the index keeps
// (library.field("avgColor")). The mosaic is made of a thousand of them, the
// tiles (`tiles`), chosen to cover the colours the library has; a picture
// is cut into cells, and each cell gets the tile closest to its colour
// that no cell beside it has (`plan`). Colours are compared as the eye sees
// them: OKLab (Björn Ottosson's).

// sRGB's 0..255 to linear light, looked up.
const LINEAR = (() => {
    const table = new Float64Array(256)
    for (let i = 0; i < 256; ++i) {
        const c = i / 255
        table[i] = c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4)
    }
    return table
})()

// r, g, b (0..255) to OKLab, into out[at..at+2].
function lab(r8, g8, b8, out, at) {
    const r = LINEAR[r8], g = LINEAR[g8], b = LINEAR[b8]
    const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    out[at] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720040 * s
    out[at + 1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s
    out[at + 2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s
}

// Lightness counts for more than hue: a picture is read by its lights and
// darks first, and a tile's hue is moved to the picture's as it is drawn
// (the view's tint).
const lightnessWeight = 1.5

// The photos a mosaic is made of: `count` of the library's, spread over
// its colours. `colours` is each position's "#rrggbb" or "", `dates` its
// date in ms, `ids` its id (library.field); `isHidden(position)` says which
// the grid hides, null for none. The colours are gathered into small boxes
// of the colour space; every box gives a photo, so a library's one purple
// evening is there, and the boxes with more photos give more, by the root
// of how many, so the browns and greys a picture is mostly made of have a
// choice of tiles. Of photos taken within minutes of each other, one. `seed`
// picks which of each box: another seed, other photos.
//
// { n, ids, rgb, lab }: the photos' ids, and their colours, three to each.
function tiles(colours, dates, ids, isHidden, count, seed) {
    let random = (seed * 2654435761 + 12345) & 0x7fffffff
    const next = () => {
        random = (Math.imul(random, 1103515245) + 12345) & 0x7fffffff
        return random / 2147483648
    }
    const boxes = new Map()
    const p = new Float64Array(3)
    let all = 0
    for (let i = 0; i < colours.length; ++i) {
        const c = colours[i]
        if (c === "" || (isHidden && isHidden(i)))
            continue
        const v = parseInt(c.slice(1), 16)
        lab(v >> 16 & 255, v >> 8 & 255, v & 255, p, 0)
        const key = (Math.floor(p[0] * 9) * 64 + Math.floor((p[1] + 0.4) / 0.04)) * 64 + Math.floor((p[2] + 0.4) / 0.04)
        const box = boxes.get(key)
        if (box)
            box.push(i)
        else
            boxes.set(key, [i])
        ++all
    }
    const list = Array.from(boxes.values())
    // In a shuffled order: with more boxes than tiles, which are left out
    // is by chance.
    for (let i = list.length - 1; i > 0; --i) {
        const r = Math.floor(next() * (i + 1))
        const t = list[i]
        list[i] = list[r]
        list[r] = t
    }
    const want = Math.min(count, all)
    let roots = 0
    for (const box of list)
        roots += Math.sqrt(box.length)
    const spare = Math.max(0, want - list.length)
    // Taken by five minutes, as a burst is one moment.
    const taken = new Set()
    const chosen = []
    const pick = box => {
        while (box.length > 0) {
            const r = Math.floor(next() * box.length)
            const i = box[r]
            box[r] = box[box.length - 1]
            box.pop()
            const moment = dates[i] > 0 ? Math.floor(dates[i] / 300000) : -1 - i
            if (taken.has(moment))
                continue
            taken.add(moment)
            chosen.push(i)
            return true
        }
        return false
    }
    for (const box of list) {
        const share = 1 + Math.round(spare * Math.sqrt(box.length) / roots)
        for (let k = 0; k < share && chosen.length < want; ++k)
            if (!pick(box))
                break
    }
    // What rounding and bursts left over, a photo a box in turn.
    for (let more = true; more && chosen.length < want;) {
        more = false
        for (const box of list)
            if (chosen.length < want && pick(box))
                more = true
    }
    const n = chosen.length
    const out = { n: n, ids: new Array(n), rgb: new Uint8Array(3 * n), lab: new Float32Array(3 * n) }
    for (let k = 0; k < n; ++k) {
        const i = chosen[k]
        const v = parseInt(colours[i].slice(1), 16)
        out.ids[k] = ids[i]
        out.rgb[3 * k] = v >> 16 & 255
        out.rgb[3 * k + 1] = v >> 8 & 255
        out.rgb[3 * k + 2] = v & 255
        lab(v >> 16 & 255, v >> 8 & 255, v & 255, p, 0)
        out.lab[3 * k] = p[0]
        out.lab[3 * k + 1] = p[1]
        out.lab[3 * k + 2] = p[2]
    }
    return out
}

// For a set of tiles, the closest of them to any colour, found once for
// each of 32 × 32 × 32 colours as it is first asked for, and kept: a
// picture has a few thousand of them, and the next size of the same
// picture the same ones.
const nearest = 64
function matcher(set) {
    return { set: set, known: new Uint8Array(32768), close: new Uint16Array(32768 * nearest) }
}
function learn(m, colour) {
    const set = m.set, base = colour * nearest
    const p = new Float64Array(3)
    lab((colour >> 10 & 31) * 8 + 4, (colour >> 5 & 31) * 8 + 4, (colour & 31) * 8 + 4, p, 0)
    const L = p[0], a = p[1], b = p[2]
    const far = new Float64Array(nearest)
    const most = Math.min(nearest, set.n)
    let have = 0
    for (let k = 0; k < set.n; ++k) {
        const dL = (set.lab[3 * k] - L) * lightnessWeight, da = set.lab[3 * k + 1] - a, db = set.lab[3 * k + 2] - b
        const d = dL * dL + da * da + db * db
        if (have === most && d >= far[most - 1])
            continue
        let j = have === most ? most - 1 : have++
        while (j > 0 && far[j - 1] > d) {
            far[j] = far[j - 1]
            m.close[base + j] = m.close[base + j - 1]
            --j
        }
        far[j] = d
        m.close[base + j] = k
    }
    m.known[colour] = 1
}

// A picture's colours cell by cell: `pixels` its r, g, b, `width` by
// `height`, averaged over each of `columns` by `rows` cells.
function cells(pixels, width, height, columns, rows) {
    const out = new Uint8Array(columns * rows * 3)
    for (let y = 0; y < rows; ++y) {
        const y0 = Math.floor(y * height / rows), y1 = Math.max(y0 + 1, Math.floor((y + 1) * height / rows))
        for (let x = 0; x < columns; ++x) {
            const x0 = Math.floor(x * width / columns), x1 = Math.max(x0 + 1, Math.floor((x + 1) * width / columns))
            let r = 0, g = 0, b = 0
            for (let v = y0; v < y1; ++v) {
                let o = (v * width + x0) * 3
                for (let u = x0; u < x1; ++u) {
                    r += pixels[o]
                    g += pixels[o + 1]
                    b += pixels[o + 2]
                    o += 3
                }
            }
            const n = (x1 - x0) * (y1 - y0), o = (y * columns + x) * 3
            out[o] = r / n
            out[o + 1] = g / n
            out[o + 2] = b / n
        }
    }
    return out
}

const BASE64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
function base64(bytes) {
    const parts = []
    let line = ""
    const n = bytes.length
    for (let i = 0; i < n; i += 3) {
        const a = bytes[i], b = i + 1 < n ? bytes[i + 1] : 0, c = i + 2 < n ? bytes[i + 2] : 0
        line += BASE64[a >> 2] + BASE64[(a & 3) << 4 | b >> 4] + (i + 1 < n ? BASE64[(b & 15) << 2 | c >> 6] : "=")
                + (i + 2 < n ? BASE64[c & 63] : "=")
        if (line.length >= 8192) {
            parts.push(line)
            line = ""
        }
    }
    parts.push(line)
    return parts.join("")
}

// The mosaic of a picture: for each of its cells (`colours`, from cells())
// the tile closest to its colour that none of the cells around it has (four
// either side in its row and in the four rows above), so a sky is many photos
// and not one over and over.
//
// { which, used, image }: each cell's tile, how many different tiles there
// are, and the plan as the shader reads it (mosaic.frag), a picture as a
// data: URL, `columns` wide and twice `rows` high: its upper half each
// cell's tile, the low byte in red and the high in green, its lower half
// what to add to that tile's colour to have the picture's, halved, around
// grey.
function plan(m, colours, columns, rows) {
    const set = m.set, count = columns * rows
    const which = new Uint16Array(count)
    const seen = new Uint8Array(set.n)
    const beside = new Int32Array(set.n)
    const reach = 4
    let used = 0
    // A BMP: 54 bytes of header, rows from the last up, each padded to
    // four bytes, blue first.
    const stride = (columns * 3 + 3) & ~3, high = rows * 2
    const bytes = new Uint8Array(54 + stride * high)
    const size = bytes.length
    bytes[0] = 66
    bytes[1] = 77
    bytes[2] = size & 255
    bytes[3] = size >> 8 & 255
    bytes[4] = size >> 16 & 255
    bytes[5] = size >> 24 & 255
    bytes[10] = 54
    bytes[14] = 40
    bytes[18] = columns & 255
    bytes[19] = columns >> 8
    bytes[22] = high & 255
    bytes[23] = high >> 8
    bytes[26] = 1
    bytes[28] = 24
    for (let y = 0; y < rows; ++y) {
        const number = 54 + stride * (high - 1 - y), nudge = 54 + stride * (rows - 1 - y)
        for (let x = 0; x < columns; ++x) {
            const at = y * columns + x, o = at * 3
            const r = colours[o], g = colours[o + 1], b = colours[o + 2]
            const colour = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3
            if (!m.known[colour])
                learn(m, colour)
            const base = colour * nearest
            const most = Math.min(nearest, set.n)
            // The tiles the cells around have, marked with this cell.
            for (let dy = -reach; dy <= 0; ++dy) {
                if (y + dy < 0)
                    continue
                const row = (y + dy) * columns
                const last = dy === 0 ? x - 1 : Math.min(columns - 1, x + reach)
                for (let u = Math.max(0, x - reach); u <= last; ++u)
                    beside[which[row + u]] = at + 1
            }
            // One of the three closest that are free, by the cell: the
            // closest alone would lay a sky out in stripes.
            let skip = (Math.imul(x, 73856093) ^ Math.imul(y, 19349663)) >>> 0
            skip = skip % 3
            let tile = -1
            for (let j = 0; j < most; ++j) {
                const t = m.close[base + j]
                if (beside[t] === at + 1)
                    continue
                tile = t
                if (skip-- === 0)
                    break
            }
            if (tile < 0)
                tile = m.close[base + skip % most]
            which[at] = tile
            if (!seen[tile]) {
                seen[tile] = 1
                ++used
            }
            bytes[number + x * 3 + 2] = tile & 255
            bytes[number + x * 3 + 1] = tile >> 8
            const dr = (r - set.rgb[3 * tile]) / 2 + 128, dg = (g - set.rgb[3 * tile + 1]) / 2 + 128,
                  db = (b - set.rgb[3 * tile + 2]) / 2 + 128
            bytes[nudge + x * 3 + 2] = dr < 0 ? 0 : dr > 255 ? 255 : dr
            bytes[nudge + x * 3 + 1] = dg < 0 ? 0 : dg > 255 ? 255 : dg
            bytes[nudge + x * 3] = db < 0 ? 0 : db > 255 ? 255 : db
        }
    }
    return { which: which, used: used, image: "data:image/bmp;base64," + base64(bytes) }
}
