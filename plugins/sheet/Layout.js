.pragma library

// Where each photo goes on the sheet, in the file's pixels. The studio
// draws the preview from this, scaled down, and magick the file, so the
// file is the picture seen. It is planned again on every change of layout,
// gap, size or dates, so it stays well under a frame at a thousand photos:
// nothing here is worse than a few passes over them.

var LAYOUTS = ["grid", "collage"]
var GAPS = ["none", "thin", "wide"]
var BACKGROUNDS = ["theme", "white", "black"]
var SIZES = [2048, 4096]

var DEFAULT_RECIPE = { layout: "collage", gap: "thin", background: "theme", size: 2048, dates: false }

// The most photos on one sheet: at 4096 px each is then about 120 px.
var MOST = 1000

// A gap, and the margin round the sheet, as a share of its longest side,
// for a sheet of a few photos; with many, smaller, as the photos are.
var GAP_SHARE = { none: 0, thin: 0.006, wide: 0.018 }

// The shape a sheet is made near: square, for a chat on a phone or a
// computer alike.
var TARGET = 1

// The sheet: { w, h, cells: [{ x, y, w, h, caption: { x, y, w, h } }] },
// one cell to a photo in the order given, its longest side `shape.size`.
// `shape` is the recipe's layout, gap, size and dates.
function plan(shape, aspects) {
    var longest = shape.size
    var n = aspects.length
    if (n === 0)
        return { w: longest, h: longest, cells: [] }
    var many = Math.min(1, 6 / Math.sqrt(n))
    var gap = Math.round(longest * GAP_SHARE[shape.gap] * many)
    // A band under each photo for its date, from the size a photo has.
    var caption = shape.dates ? Math.max(12, Math.round(0.07 * longest / Math.sqrt(n))) : 0
    var lay = shape.layout === "grid" ? grid : collage
    var sheet = lay(aspects, longest, gap, caption, null)
    // Taller than wide: the width that makes the height the longest side.
    // The gaps and the dates keep their pixels, so it is looked for: the
    // height only grows with the width, the photos laid out as they are.
    if (sheet.h > longest) {
        var lo = 1, hi = longest
        for (var k = 0; k < 24; ++k) {
            var mid = (lo + hi) / 2
            if (tall(shape.layout, aspects.length, mid, gap, caption, sheet.choice) > longest)
                hi = mid
            else
                lo = mid
        }
        sheet = lay(aspects, lo, gap, caption, sheet.choice)
    }
    return rounded(sheet, caption)
}

// The height at a width, the photos laid out as `choice` has them.
function tall(layout, n, width, gap, caption, choice) {
    if (layout === "collage")
        return height(choice, width, gap, caption)
    var rows = Math.ceil(n / choice.columns)
    return (rows + 1) * gap + rows * (Math.max(0, (width - (choice.columns + 1) * gap) / choice.columns) / choice.cell + caption)
}

// Even cells of the photos' middle shape, each photo cropped to fill its
// own; as many columns as make the sheet nearest square, the last row
// centred.
function grid(aspects, width, gap, caption, choice) {
    var n = aspects.length
    var cell = choice ? choice.cell : middle(aspects)
    var columns = choice ? choice.columns : 1
    if (!choice) {
        var best = Infinity
        for (var c = 1; c <= n; ++c) {
            var r = Math.ceil(n / c)
            var cw = (width - (c + 1) * gap) / c
            var h = (r + 1) * gap + r * (cw / cell + caption)
            var score = Math.abs(Math.log(width / h / TARGET)) + 0.6 * (c * r - n) / c
            if (score < best) {
                best = score
                columns = c
            }
            // Wider than square from here on, and only wider.
            if (width / h > TARGET * 4)
                break
        }
    }
    var rows = Math.ceil(n / columns)
    var w = Math.max(0, (width - (columns + 1) * gap) / columns)
    var hh = w / cell
    var cells = new Array(n)
    for (var i = 0; i < n; ++i) {
        var row = Math.floor(i / columns)
        var inRow = row === rows - 1 ? n - row * columns : columns
        var left = (width - inRow * w - (inRow - 1) * gap) / 2
        cells[i] = { x: left + (i % columns) * (w + gap), y: gap + row * (hh + caption + gap), w: w, h: hh }
    }
    return { w: width, h: (rows + 1) * gap + rows * (hh + caption), cells: cells, choice: { cell: cell, columns: columns } }
}

function middle(aspects) {
    var sorted = aspects.slice().sort(function (a, b) { return a - b })
    return sorted[Math.floor(sorted.length / 2)]
}

// Whole photos in rows, in order, each row as tall as fills the width: as
// many rows as make the sheet nearest square, the photos split between
// them as evenly as their shapes allow. Square without gaps is about
// √(their widths over their heights, added up) rows, so only the few
// counts round that are tried.
function collage(aspects, width, gap, caption, choice) {
    var n = aspects.length
    var rowsOf = choice
    if (!rowsOf) {
        var sum = 0
        for (var i = 0; i < n; ++i)
            sum += aspects[i]
        var guess = Math.round(Math.sqrt(sum / TARGET))
        var best = Infinity
        for (var r = Math.max(1, guess - 3); r <= Math.min(n, guess + 3); ++r) {
            var rows = n <= 120 ? split(aspects, r) : greedy(aspects, r)
            var score = Math.abs(Math.log(width / height(rows, width, gap, caption) / TARGET))
            if (score < best) {
                best = score
                rowsOf = rows
            }
        }
    }
    var cells = new Array(n)
    var y = gap
    for (var k = 0; k < rowsOf.length; ++k) {
        var row = rowsOf[k]
        var hh = Math.max(0, (width - (row[1] - row[0] + 1) * gap) / row[2])
        var x = gap
        for (var j = row[0]; j < row[1]; ++j) {
            cells[j] = { x: x, y: y, w: hh * aspects[j], h: hh }
            x += hh * aspects[j] + gap
        }
        y += hh + caption + gap
    }
    return { w: width, h: y, cells: cells, choice: rowsOf }
}

function height(rows, width, gap, caption) {
    var y = gap
    for (var k = 0; k < rows.length; ++k)
        y += Math.max(0, (width - (rows[k][1] - rows[k][0] + 1) * gap) / rows[k][2]) + caption + gap
    return y
}

// The photos, in order, in `r` rows whose shapes add up as evenly as can
// be: [[from, to, sum of aspects], ...]. Every split is weighed, so only
// for a few dozen photos.
function split(aspects, r) {
    var n = aspects.length
    var prefix = [0]
    for (var i = 0; i < n; ++i)
        prefix.push(prefix[i] + aspects[i])
    var ideal = prefix[n] / r
    // cost[k][i]: the first i photos in k rows; from[k][i]: where the last
    // of those rows starts.
    var cost = [], from = []
    for (var k = 0; k <= r; ++k) {
        cost.push(new Array(n + 1).fill(Infinity))
        from.push(new Array(n + 1).fill(0))
    }
    cost[0][0] = 0
    for (k = 1; k <= r; ++k) {
        for (i = k; i <= n - (r - k); ++i) {
            for (var j = k - 1; j < i; ++j) {
                var d = prefix[i] - prefix[j] - ideal
                var c = cost[k - 1][j] + d * d
                if (c < cost[k][i]) {
                    cost[k][i] = c
                    from[k][i] = j
                }
            }
        }
    }
    var rows = []
    var end = n
    for (k = r; k >= 1; --k) {
        var start = from[k][end]
        rows.unshift([start, end, prefix[end] - prefix[start]])
        end = start
    }
    return rows
}

// The same in one pass, for many: each row filled to its share of what is
// left, ending before or after the photo that crosses it, whichever is
// nearer. Rows of many photos even out what one more or less does.
function greedy(aspects, r) {
    var n = aspects.length
    var left = 0
    for (var i = 0; i < n; ++i)
        left += aspects[i]
    var rows = []
    var start = 0
    for (var k = r; k >= 1 && start < n; --k) {
        if (k === 1) {
            var last = 0
            for (var j = start; j < n; ++j)
                last += aspects[j]
            rows.push([start, n, last])
            break
        }
        var share = left / k
        var sum = 0, end = start
        // At least one photo, and one left for each row still to come.
        while (end < n - (k - 1)) {
            var next = sum + aspects[end]
            if (end > start && Math.abs(next - share) > Math.abs(sum - share))
                break
            sum = next
            ++end
        }
        rows.push([start, end, sum])
        left -= sum
        start = end
    }
    return rows
}

// Whole pixels: each edge rounded, so neighbours keep their gap exactly.
function rounded(sheet, caption) {
    var cells = new Array(sheet.cells.length)
    for (var i = 0; i < cells.length; ++i) {
        var c = sheet.cells[i]
        var x = Math.round(c.x), y = Math.round(c.y)
        var w = Math.max(1, Math.round(c.x + c.w) - x), h = Math.max(1, Math.round(c.y + c.h) - y)
        cells[i] = { x: x, y: y, w: w, h: h, caption: { x: x, y: y + h, w: w, h: caption } }
    }
    return { w: Math.round(sheet.w), h: Math.round(sheet.h), cells: cells }
}

// A file as magick is given it, its first image: every % doubled, as
// ImageMagick reads "%d" or "%o" in a name as a frame's number ("50%off.jpg"
// is "500ff.jpg" to it, and a folder's photo may be called anything).
// `[1]` and `*` in the name of a file that is there are read as they are.
function magickFile(path) {
    return path.replace(/%/g, "%%") + "[0]"
}

// What magick is given to make the file: the photos (`files`, in the
// cells' order) on the background, each cut to its cell, and the dates,
// each in its size in `points` (none where 0). Each photo is made sRGB from
// its own profile (an iPhone's are Display P3, which shown as sRGB is
// duller), or without one by ImageMagick's own sums (a CMYK JPEG's colours
// went in as they were, magenta as green), and the file says it is sRGB:
// `profile` is sRGB.icc, beside this file, Little CMS's own sRGB
// (cmsCreate_sRGBProfile) saved as ICC v4, 620 bytes and free of any
// copyright. As v4 its curves are a formula, not tables: a
// v2 one's took Little CMS 85 ms a photo to turn round, this one's 3.
function magickArguments(sheet, files, colours, dates, points, font, profile, out) {
    var args = ["-size", sheet.w + "x" + sheet.h, "xc:" + colours.background, "-background", colours.background]
    for (var i = 0; i < sheet.cells.length; ++i) {
        var c = sheet.cells[i]
        var size = c.w + "x" + c.h
        // Decoding a JPEG at no more than twice the cell is much faster.
        args.push("(", "-define", "jpeg:size=" + 2 * c.w + "x" + 2 * c.h, magickFile(files[i]),
                  "-auto-orient", "-resize", size + "^", "-profile", profile, "-colorspace", "sRGB", "-gravity", "center", "-extent", size, ")",
                  "-gravity", "NorthWest", "-geometry", "+" + c.x + "+" + c.y, "-composite")
    }
    if (dates.length > 0) {
        if (font !== "")
            args.push("-font", font)
        args.push("-fill", colours.caption, "-gravity", "North")
        var size = 0
        for (i = 0; i < sheet.cells.length; ++i) {
            var pt = points[i] || 0
            if (pt === 0)
                continue
            if (pt !== size)
                args.push("-pointsize", String(size = pt))
            var box = sheet.cells[i].caption
            var dx = Math.round(box.x + box.w / 2 - sheet.w / 2)
            var dy = Math.round(box.y + (box.h - pt * 1.2) / 2)
            args.push("-annotate", (dx < 0 ? "" : "+") + dx + "+" + dy, dates[i].replace(/\\/g, "\\\\").replace(/%/g, "%%"))
        }
    }
    args.push("-profile", profile, "-quality", "90", out.replace(/%/g, "%%"))
    return args
}
