.pragma library

// What sqlite3 is asked, of the source's library.sqlite (docs/sources.md),
// to find the screenshots: their ids, one to a line.

// The library's tables: whether it is iCloud's (icloud_assets), and has
// the sizes of the originals (renditions; pinhole-fake's has none).
var tablesQuery = "SELECT name FROM sqlite_master WHERE type = 'table';"

// Screens, either way up: iPhones' and iPads' (what their screenshots
// measure, in pixels), then monitors' and laptops'.
var screens = [
    [320, 480], [640, 960], [640, 1136], [750, 1334], [1080, 1920], [1242, 2208], [1125, 2436], [828, 1792],
    [1242, 2688], [1080, 2340], [1170, 2532], [1284, 2778], [1179, 2556], [1290, 2796], [1206, 2622],
    [1320, 2868], [1260, 2736],
    [768, 1024], [1536, 2048], [1668, 2224], [1668, 2388], [2048, 2732], [1620, 2160], [1640, 2360], [1488, 2266],
    [1366, 768], [1440, 900], [1680, 1050], [1920, 1080], [1920, 1200], [2560, 1080], [2560, 1440], [2560, 1600],
    [2880, 1800], [2880, 1920], [3024, 1964], [3440, 1440], [3456, 2234], [3840, 2160], [5120, 2880],
    [2560, 1664], [2940, 1912], [2256, 1504], [2880, 1620]
]

// Names screenshots are given: Omarchy's and grim's, hyprshot's, a Mac's
// (in English and in Polish), and a few more languages'. GLOB patterns, on
// the name in lower case.
var names = [
    "screenshot*", "screen shot*", "zrzut ekranu*", "*hyprshot*", "*_grim.png", "bildschirmfoto*",
    "capture d?écran*", "captura de pantalla*", "schermata*", "skärmavbild*", "schermafbeelding*"
]

// iCloud's subtype of an iPhone's or an iPad's screenshot.
var icloudScreenshot = 3

function query(tables) {
    var icloud = tables.indexOf("icloud_assets") >= 0
    var sized = tables.indexOf("renditions") >= 0
    var sizes = screens.map(function (s) { return "(" + s[0] + "," + s[1] + ")" }).join(",")
    var named = names.map(function (n) { return "lower(a.filename) GLOB '" + n.replace(/'/g, "''") + "'" }).join(" OR ")
    return "WITH sizes(w, h) AS (VALUES " + sizes + ")"
        + " SELECT a.id FROM assets a"
        + (sized ? " LEFT JOIN renditions r ON r.asset_id = a.id AND r.kind = 'original'" : "")
        + (icloud ? " LEFT JOIN icloud_assets i ON i.asset_id = a.id" : "")
        + " WHERE NOT a.is_video AND ("
        + (icloud ? "i.subtype = " + icloudScreenshot + " OR " : "")
        + named
        // A PNG with no place, as big as a screen.
        + (sized ? " OR (lower(a.filename) GLOB '*.png' AND NOT a.has_location"
           + " AND EXISTS (SELECT 1 FROM sizes WHERE (w = r.width AND h = r.height) OR (h = r.width AND w = r.height)))" : "")
        + ");"
}
