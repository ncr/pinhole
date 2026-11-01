.pragma library

// What sqlite3 is asked, of the source's library.sqlite (docs/sources.md),
// to find the screenshots: their ids, one to a line.

// The library's tables: whether it is iCloud's (icloud_assets).
var tablesQuery = "SELECT name FROM sqlite_master WHERE type = 'table';"

// Names screenshots are given: Omarchy's and grim's, hyprshot's, a Mac's
// (in English and in Polish), and a few more languages'. GLOB patterns, on
// the name in lower case.
var names = [
    "screenshot*", "screen shot*", "zrzut ekranu*", "*hyprshot*", "*_grim.png", "bildschirmfoto*",
    "capture d?écran*", "captura de pantalla*", "schermata*", "skärmavbild*", "schermafbeelding*"
]

// iCloud's subtype of an iPhone's or an iPad's screenshot.
var icloudScreenshot = 3

// A phone's screen, either way up, as the short side over the long one of
// the thumbnail (which every source has, its rounding included): an iPhone
// since the X (19.5:9), and one before it or a 1080p screen (16:9). Other
// shapes (3:2, 4:3, 16:10) are as often a picture as a screen, and a scan
// at 16:9 measures 0.5604.
var shapes = [[0.455, 0.468], [0.561, 0.566]]

function query(tables) {
    var icloud = tables.indexOf("icloud_assets") >= 0
    var named = names.map(function (n) { return "lower(a.filename) GLOB '" + n.replace(/'/g, "''") + "'" }).join(" OR ")
    var ratio = "MIN(a.thumb_w, a.thumb_h) * 1.0 / MAX(a.thumb_w, a.thumb_h)"
    var shaped = shapes.map(function (s) { return ratio + " BETWEEN " + s[0] + " AND " + s[1] }).join(" OR ")
    return "SELECT a.id FROM assets a"
        + (icloud ? " LEFT JOIN icloud_assets i ON i.asset_id = a.id" : "")
        + " WHERE NOT a.is_video AND ("
        + (icloud ? "i.subtype = " + icloudScreenshot + " OR " : "")
        + named
        // A PNG with no place, shaped like a phone's screen.
        + " OR (lower(a.filename) GLOB '*.png' AND NOT a.has_location AND a.thumb_w > 0 AND a.thumb_h > 0"
        + " AND (" + shaped + ")));"
}
