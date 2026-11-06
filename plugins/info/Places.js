.pragma library

const maxZoom = 19
const wrap = x => x - Math.floor(x)
const delta = (x, center) => wrap(x - center + 0.5) - 0.5
const scale = zoom => 256 * Math.pow(2, zoom)
const clamp = (value, lo, hi) => Math.max(lo, Math.min(hi, value))

function point(id, latitude, longitude, date) {
    if (typeof latitude !== "number" || typeof longitude !== "number"
            || !(Math.abs(latitude) <= 90 && Math.abs(longitude) <= 180))
        return null
    const rad = clamp(latitude, -85.05112878, 85.05112878) * Math.PI / 180
    return { id: id, x: wrap((longitude + 180) / 360),
        y: clamp((1 - Math.log(Math.tan(rad) + 1 / Math.cos(rad)) / Math.PI) / 2, 0, 1),
        date: date || 0 }
}

// Cut the longitude circle at the largest gap, so Fiji and Tonga fit
// together even though they are on opposite sides of the date line.
function fit(points, width, height) {
    if (!points.length)
        return { x: 0.5, y: 0.5, zoom: 1 }
    let top = 1, bottom = 0
    const xs = points.map(p => { top = Math.min(top, p.y); bottom = Math.max(bottom, p.y); return p.x })
    xs.sort((a, b) => a - b)
    let start = xs[0], span = xs[xs.length - 1] - start
    for (let i = 1; i < xs.length; ++i) {
        const around = 1 - (xs[i] - xs[i - 1])
        if (around < span) {
            start = xs[i]
            span = around
        }
    }
    const zoom = clamp(Math.log2(Math.min(Math.max(32, width - 132) / (256 * span),
        Math.max(32, height - 132) / (256 * (bottom - top)))), 0, 14)
    return { x: wrap(start + span / 2), y: (top + bottom) / 2, zoom: zoom }
}

// Screen distance gives a consistent spacing between markers. Check the
// neighbouring cells too, so a cell boundary doesn't split nearby photos.
// Fixed seeds keep the result deterministic and bound each group's radius.
//
// The cells are the world's, not the view's: at one zoom the groups are the
// same wherever the view is, so panning only shows more or fewer of them and
// no marker jumps from one group to another under the fingers.
function clusters(points, zoom) {
    const size = scale(zoom), radius = 74
    const bins = Object.create(null)
    const groups = []
    for (const p of points) {
        const x = p.x * size, y = p.y * size
        const bx = Math.floor(x / radius), by = Math.floor(y / radius)
        let near = null, distance = radius * radius
        for (let j = by - 1; j <= by + 1; ++j) {
            for (let i = bx - 1; i <= bx + 1; ++i) {
                const cell = bins[i + ":" + j]
                if (!cell) continue
                for (const g of cell) {
                    const d = (g.sx - x) ** 2 + (g.sy - y) ** 2
                    if (d < distance) {
                        near = g
                        distance = d
                    }
                }
            }
        }
        if (near) {
            near.points.push(p)
            if (p.date > near.cover.date)
                near.cover = p
        } else {
            const group = { key: p.id, sx: x, sy: y, x: p.x, y: p.y, cover: p, points: [p] }
            const key = bx + ":" + by
            if (!bins[key])
                bins[key] = []
            bins[key].push(group)
            groups.push(group)
        }
    }
    return groups
}

// Those of `groups` around the view (a marker's width past its edges).
function around(groups, cx, cy, zoom, width, height) {
    const size = scale(zoom), margin = 74
    return groups.filter(g => Math.abs(delta(g.x, cx) * size) <= width / 2 + margin
                              && Math.abs((g.y - cy) * size) <= height / 2 + margin)
}

// Reuse the delegates while panning through the same set of places.
function sameGroups(before, after) {
    return before.length === after.length && before.every((g, i) => {
        const other = after[i]
        return g.x === other.x && g.y === other.y && g.cover.id === other.cover.id
            && g.points.length === other.points.length
            && g.points.every((p, j) => p.id === other.points[j].id
                && p.x === other.points[j].x && p.y === other.points[j].y && p.date === other.points[j].date)
    })
}
