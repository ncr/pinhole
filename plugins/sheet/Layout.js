.pragma library

// Where each photo goes on the sheet, in the file's pixels. The studio
// draws the preview from this, scaled down, and magick the file, so the
// file is the picture seen.

var LAYOUTS = ["grid", "collage"]
var GAPS = ["none", "thin", "wide"]
var BACKGROUNDS = ["theme", "white", "black"]
var SIZES = [2048, 4096]

var DEFAULT_RECIPE = { layout: "collage", gap: "thin", background: "theme", size: 2048, dates: false }

// A gap, and the margin round the sheet, as a share of its longest side.
var GAP_SHARE = { none: 0, thin: 0.006, wide: 0.018 }

// The shape a sheet is made near: square, for a chat on a phone or a
// computer alike.
var TARGET = 1

// The sheet: { w, h, cells: [{ x, y, w, h, caption: { x, y, w, h } }] },
// one cell to a photo in the order given, its longest side `recipe.size`
// (near enough: the gaps keep their pixels as it is fitted).
function plan(recipe, aspects) {
    var longest = recipe.size
    var gap = Math.round(longest * GAP_SHARE[recipe.gap])
    var n = aspects.length
    if (n === 0)
        return { w: longest, h: longest, cells: [] }
    // A band under each photo for its date, from the size a photo has.
    var caption = recipe.dates ? Math.max(16, Math.round(0.07 * longest / Math.sqrt(n))) : 0
    var lay = recipe.layout === "grid" ? grid : collage
    var width = longest
    var sheet = lay(aspects, width, gap, caption, null)
    // Taller than wide: narrower, until the height is the longest side.
    for (var k = 0; k < 4 && sheet.h > longest + 0.5; ++k) {
        width = width * longest / sheet.h
        sheet = lay(aspects, width, gap, caption, sheet.choice)
    }
    return rounded(sheet, caption)
}

// Even cells of the photos' middle shape, each photo cropped to fill its
// own; as many columns as make the sheet nearest square, the last row
// centred.
function grid(aspects, width, gap, caption, choice) {
    var n = aspects.length
    var sorted = aspects.slice().sort(function (a, b) { return a - b })
    var cell = sorted[Math.floor(n / 2)]
    var columns = choice
    if (columns === null) {
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
        }
    }
    var rows = Math.ceil(n / columns)
    var w = (width - (columns + 1) * gap) / columns
    var hh = w / cell
    var cells = []
    for (var i = 0; i < n; ++i) {
        var row = Math.floor(i / columns)
        var inRow = row === rows - 1 ? n - row * columns : columns
        var left = (width - inRow * w - (inRow - 1) * gap) / 2
        cells.push({ x: left + (i % columns) * (w + gap), y: gap + row * (hh + caption + gap), w: w, h: hh })
    }
    return { w: width, h: (rows + 1) * gap + rows * (hh + caption), cells: cells, choice: columns }
}

// Whole photos in rows, in order, each row as tall as fills the width: the
// photos split into as many rows as make the sheet nearest square, as
// evenly as their shapes allow.
function collage(aspects, width, gap, caption, choice) {
    var n = aspects.length
    var rowsOf = choice
    if (rowsOf === null) {
        var best = Infinity
        for (var r = 1; r <= n; ++r) {
            var rows = split(aspects, r)
            var h = height(rows, width, gap, caption)
            var score = Math.abs(Math.log(width / h / TARGET))
            if (score < best) {
                best = score
                rowsOf = rows
            }
            // Past square, more rows only make it taller.
            if (width / h < TARGET * 0.7)
                break
        }
    }
    var cells = []
    var y = gap
    for (var k = 0; k < rowsOf.length; ++k) {
        var row = rowsOf[k]
        var sum = 0
        for (var j = row[0]; j < row[1]; ++j)
            sum += aspects[j]
        var hh = (width - (row[1] - row[0] + 1) * gap) / sum
        var x = gap
        for (j = row[0]; j < row[1]; ++j) {
            cells.push({ x: x, y: y, w: hh * aspects[j], h: hh })
            x += hh * aspects[j] + gap
        }
        y += hh + caption + gap
    }
    return { w: width, h: y, cells: cells, choice: rowsOf }
}

function height(rows, width, gap, caption) {
    var y = gap
    for (var k = 0; k < rows.length; ++k)
        y += (width - (rows[k][1] - rows[k][0] + 1) * gap) / rows[k][2] + caption + gap
    return y
}

// The photos, in order, in `r` rows whose shapes add up as evenly as can
// be: [[from, to, sum of aspects], ...].
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

// Whole pixels: each edge rounded, so neighbours keep their gap exactly.
function rounded(sheet, caption) {
    var cells = sheet.cells.map(function (c) {
        var x = Math.round(c.x), y = Math.round(c.y)
        var w = Math.round(c.x + c.w) - x, h = Math.round(c.y + c.h) - y
        return { x: x, y: y, w: w, h: h, caption: { x: x, y: y + h, w: w, h: caption } }
    })
    return { w: Math.round(sheet.w), h: Math.round(sheet.h), cells: cells }
}

// What magick is given to make the file: the photos (`files`, in the
// cells' order) on the background, each cut to its cell, and the dates.
function magickArguments(sheet, files, colours, dates, font, out) {
    var args = ["-size", sheet.w + "x" + sheet.h, "xc:" + colours.background, "-background", colours.background]
    for (var i = 0; i < sheet.cells.length; ++i) {
        var c = sheet.cells[i]
        var size = c.w + "x" + c.h
        // Decoding a JPEG at no more than twice the cell is much faster.
        args.push("(", "-define", "jpeg:size=" + 2 * c.w + "x" + 2 * c.h, files[i] + "[0]",
                  "-auto-orient", "-resize", size + "^", "-gravity", "center", "-extent", size, ")",
                  "-gravity", "NorthWest", "-geometry", "+" + c.x + "+" + c.y, "-composite")
    }
    if (dates.length > 0) {
        if (font !== "")
            args.push("-font", font)
        var points = Math.round(sheet.cells[0].caption.h * 0.5)
        args.push("-fill", colours.caption, "-pointsize", String(points), "-gravity", "North")
        for (i = 0; i < sheet.cells.length; ++i) {
            var box = sheet.cells[i].caption
            var dx = Math.round(box.x + box.w / 2 - sheet.w / 2)
            var dy = Math.round(box.y + (box.h - points * 1.2) / 2)
            args.push("-annotate", (dx < 0 ? "" : "+") + dx + "+" + dy, dates[i].replace(/\\/g, "\\\\").replace(/%/g, "%%"))
        }
    }
    args.push("-quality", "90", out)
    return args
}
