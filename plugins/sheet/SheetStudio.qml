import QtQuick
import Pinhole.Plugin
import "Layout.js" as Layout

// The photos ticked as one sheet, full window: the sheet as it will be,
// laid out here from the photos' own pictures, with the ways to change it
// beneath, every one a click or a key. Enter makes it: the photos fetched
// at the size their cells need, put together by magick, copied to the
// clipboard and kept in Pictures/Pinhole.
Item {
    id: studio
    required property var plugin
    readonly property var pinhole: plugin.pinhole

    property bool shown: false
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    // The photos, oldest first, by id (positions move when the library is
    // read again): {id, date, video, thumbnail}. At most Layout.MOST; how
    // many more were ticked.
    property var photos: []
    property int over: 0
    // Their shapes, width over height, as the sheet is planned from: set
    // as it opens, and again only for a photo whose shape was not known.
    property var aspects: []
    // Sharper pictures for the preview, by id, as they come.
    property var sharp: ({})
    // Files fetched while it is open, by id and kind.
    property var files: ({})
    // The library it was opened on, by its key.
    property string libraryKey: ""
    // Bumped as it opens again or closes, so what comes for before is let
    // be; and the same for making it, bumped as that stops (Esc while it is
    // made), when the previews still come.
    property int generation: 0
    property int makeGeneration: 0
    // What is on its way (fetches, programs), to cancel: for the preview,
    // and for making it.
    property var pending: []
    property var makePending: []
    // The thumbnails' decoded size, about a cell's, for a thousand to fit.
    property int thumbSize: 512

    // How it is made, kept for next time.
    property var recipe: Object.assign({}, Layout.DEFAULT_RECIPE, pinhole.setting("recipe", {}))
    function change(part) {
        recipe = Object.assign({}, recipe, part)
        pinhole.setSetting("recipe", recipe)
    }
    function cycle(name, list) {
        const part = {}
        part[name] = list[(list.indexOf(recipe[name]) + 1) % list.length]
        change(part)
    }

    // What the plan is made from: the background changes nothing in it, so
    // B plans nothing again (a string is only changed when it differs).
    readonly property string shape: JSON.stringify({ layout: recipe.layout, gap: recipe.gap, size: recipe.size, dates: recipe.dates })
    readonly property var sheet: Layout.plan(JSON.parse(shape), aspects)
    readonly property var colours: recipe.background === "white" ? { background: "#ffffff", caption: "#707070" }
        : recipe.background === "black" ? { background: "#000000", caption: "#a0a0a0" }
        : { background: String(theme.background), caption: String(theme.darkForeground) }
    // Under each photo: the day it was taken, or the time when all are of
    // one day.
    readonly property bool oneDay: photos.length > 0 && day(photos[0].date) === day(photos[photos.length - 1].date)
    readonly property var dates: photos.map(p => oneDay ? Qt.formatTime(new Date(p.date), "HH:mm")
                                                        : new Date(p.date).toLocaleDateString(Qt.locale(), "d MMM yyyy"))
    function day(ms) { return Qt.formatDate(new Date(ms), "yyyy-MM-dd") }
    // Each date's size, in the file's pixels: half its band, or less where
    // its cell is too narrow for the longest; none where that would be too
    // small to read. The preview and the file alike.
    readonly property var captionPoints: {
        if (!recipe.dates || sheet.cells.length === 0)
            return []
        const perPoint = Math.max(0.01, dateWidth.advanceWidth / dateWidth.font.pixelSize)
        const most = Math.floor(sheet.cells[0].caption.h * 0.5)
        return sheet.cells.map(c => {
            const fits = Math.min(most, Math.floor(c.w * 0.92 / perPoint))
            return fits >= Math.min(6, most) ? fits : 0
        })
    }
    TextMetrics {
        id: dateWidth
        font.family: theme.font
        font.pixelSize: 100
        text: studio.dates.reduce((a, b) => b.length > a.length ? b : a, "")
    }

    // Being made: "fetching" the photos (how many are here), then
    // "making"; the last one made, or why not, or a word on what happened.
    property string making: ""
    property int fetched: 0
    property var made: null
    property string error: ""
    property string note: ""

    readonly property var names: ({
        grid: qsTr("Grid"), collage: qsTr("Collage"),
        none: qsTr("No gap"), thin: qsTr("Thin gap"), wide: qsTr("Wide gap"),
        theme: qsTr("Theme"), white: qsTr("White"), black: qsTr("Black")
    })

    function open(indexes) {
        if (making !== "")
            return
        stop()
        let list = Array.from(indexes).map(i => {
            const item = pinhole.library.item(i)
            return { id: item.id, date: item.date, video: item.video, thumbnail: String(item.thumbnail), thumbPath: item.thumbPath,
                     aspect: item.width > 0 && item.height > 0 ? item.width / item.height : 0 }
        }).filter(p => p.id !== undefined).sort((a, b) => a.date - b.date)
        over = Math.max(0, list.length - Layout.MOST)
        list = list.slice(0, Layout.MOST)
        // A photo of no known shape is taken as 4:3 until its medium says.
        aspects = list.map(p => p.aspect > 0 ? p.aspect : 4 / 3)
        photos = list.map(p => ({ id: p.id, date: p.date, video: p.video, thumbnail: p.thumbnail, thumbPath: p.thumbPath }))
        libraryKey = pinhole.library.key
        sharp = {}
        files = {}
        made = null
        error = ""
        note = ""
        thumbSize = Math.max(64, Math.min(512, Math.ceil(2 * Math.sqrt(width * height / Math.max(1, list.length)) / 64) * 64))
        shown = true
        const gen = generation
        // The sharper pictures for the preview, as each comes; for a few
        // dozen, past which a cell is small enough for the thumbnail.
        list.slice(0, 48).forEach(p => fetchFile(p.id, "medium").then(path => {
            if (gen !== generation)
                return
            const more = Object.assign({}, sharp)
            more[p.id] = "image://file/" + encodeURIComponent(path)
            sharp = more
        }, () => {}).catch(pinhole.warn))
        // The shape of those that had none, from their mediums, all at once.
        const unknown = list.filter(p => !(p.aspect > 0))
        if (unknown.length > 0)
            measure(unknown, gen)
    }
    function measure(unknown, gen) {
        const paths = {}
        let left = unknown.length
        unknown.forEach(p => fetchFile(p.id, "medium").then(path => paths[p.id] = path, () => {}).then(() => {
            if (--left > 0 || gen !== generation)
                return
            const ids = unknown.map(p => p.id).filter(id => paths[id])
            if (ids.length === 0)
                return
            keep(pinhole.run("magick", ["identify", "-ping", "-format", "%w %h %[orientation]\n"].concat(ids.map(id => Layout.magickFile(paths[id]))),
                             { timeout: 60000 })).then(result => {
                if (gen !== generation)
                    return
                const lines = result.stdout.trim().split("\n")
                const shapes = {}
                ids.forEach((id, n) => {
                    const [w, h, orientation] = (lines[n] || "").split(" ")
                    // Turned a quarter, as LeftTop to LeftBottom are.
                    const turned = /^(LeftTop|RightTop|RightBottom|LeftBottom)$/.test(orientation)
                    if (Number(w) > 0 && Number(h) > 0)
                        shapes[id] = turned ? Number(h) / Number(w) : Number(w) / Number(h)
                })
                aspects = photos.map((p, n) => shapes[p.id] || aspects[n])
            }, () => {}).catch(pinhole.warn)
        }).catch(pinhole.warn))
    }

    // Esc, K or ✕: while one is made, stops it, and says so; else closes.
    function close() {
        if (making !== "") {
            stopMaking()
            note = qsTr("Cancelled")
            return
        }
        stop()
        shown = false
    }
    // Whatever is on its way, let go.
    function stop() {
        ++generation
        const was = pending
        pending = []
        was.forEach(p => p.cancel())
        stopMaking()
    }
    // Making it, let go; the previews still come.
    function stopMaking() {
        ++makeGeneration
        const was = makePending
        makePending = []
        was.forEach(p => p.cancel())
        making = ""
    }
    // A promise kept, to cancel, until it is answered: the preview's, or
    // with `forMaking` the making's.
    function keep(promise, forMaking) {
        const list = () => forMaking ? makePending : pending
        list().push(promise)
        const done = () => { const at = list().indexOf(promise); if (at >= 0) list().splice(at, 1) }
        promise.then(done, done)
        return promise
    }
    // A photo's file, once: a promise of its path.
    function fetchFile(id, kind, forMaking) {
        const key = id + ":" + kind
        if (files[key] !== undefined)
            return Promise.resolve(files[key])
        return keep(pinhole.fetch(id, kind), forMaking).then(path => {
            files[key] = path
            return path
        })
    }

    // The library read again, as when photos come or go, or another
    // source's shown: the photos are kept by id, those gone left out.
    Connections {
        target: studio.pinhole.library
        function onChanged() { studio.libraryChanged() }
    }
    function libraryChanged() {
        if (!shown)
            return
        if (pinhole.library.key !== libraryKey) {
            stop()
            shown = false
            pinhole.notify(qsTr("Another library is shown, so the contact sheet was closed"))
            return
        }
        const kept = photos.map((p, n) => n).filter(n => pinhole.library.indexOf(photos[n].id) >= 0)
        if (kept.length === photos.length)
            return
        const gone = photos.length - kept.length
        if (kept.length < 2) {
            stop()
            shown = false
            pinhole.notify(qsTr("The photos ticked are no longer in the library"))
            return
        }
        // Being made, it stops, and says so: ⏎ makes it of those left.
        const wasMaking = making !== ""
        if (wasMaking)
            stopMaking()
        aspects = kept.map(n => aspects[n])
        photos = kept.map(n => photos[n])
        note = wasMaking ? qsTr("%n photo(s) no longer in the library, so the sheet was not made", "", gone)
                         : qsTr("%n photo(s) no longer in the library, left out", "", gone)
    }

    // The sheet: each photo fetched as large as its cell needs (the
    // medium, about 2048 px, unless the cell is near that), a few at a
    // time, then magick.
    function make() {
        if (making !== "" || photos.length === 0)
            return
        const gen = makeGeneration
        const sheet = studio.sheet
        const shot = photos.slice()
        const paths = new Array(shot.length)
        const points = captionPoints
        making = "fetching"
        fetched = 0
        made = null
        error = ""
        note = ""
        let next = 0, left = shot.length, blurry = 0
        const take = (n, path) => {
            paths[n] = path
            ++fetched
            if (--left === 0)
                render(sheet, shot, paths, points, blurry)
            else
                start()
        }
        // A function, so that take() above may start the next.
        function start() {
            if (gen !== makeGeneration || next >= shot.length)
                return
            const n = next++
            const p = shot[n]
            const cell = sheet.cells[n]
            const kind = p.video || Math.max(cell.w, cell.h) <= 1280 ? "medium" : "full"
            // Without the full photo, the medium; without that, the
            // thumbnail, better than none, and said. Stopped, neither.
            fetchFile(p.id, kind, true).then(path => path, error => {
                if (kind === "medium" || error.code === "cancelled" || gen !== makeGeneration)
                    throw error
                return fetchFile(p.id, "medium", true)
            }).then(path => {
                if (gen === makeGeneration)
                    take(n, path)
            }, error => {
                if (gen !== makeGeneration || error.code === "cancelled")
                    return
                ++blurry
                take(n, p.thumbPath || "")
            }).catch(pinhole.warn)
        }
        for (let k = 0; k < 6; ++k)
            start()
    }
    function render(sheet, shot, paths, points, blurry) {
        // Mostly thumbnails is no sheet to send.
        if (paths.some(path => path === "") || blurry * 2 > shot.length) {
            fail(qsTr("%1 of %2 photos could not be fetched").arg(blurry).arg(shot.length))
            return
        }
        making = "making"
        const gen = makeGeneration
        const path = pinhole.picturePath(day(shot[0].date) + " " + (recipe.layout === "grid" ? qsTr("Contact sheet") : qsTr("Collage")),
                                         "jpg")
        // Made beside, then moved in whole.
        const temp = pinhole.cacheDir + "/sheet.jpg"
        const args = Layout.magickArguments(sheet, paths, colours, recipe.dates ? dates : [], points, plugin.fontFile,
                                            pinhole.dir + "/sRGB.icc", temp)
        keep(pinhole.run("mkdir", ["-p", pinhole.cacheDir]), true).then(() => {
            if (gen !== makeGeneration)
                return null
            // A minute, and more for many.
            return keep(pinhole.run("magick", args, { timeout: 60000 + 500 * shot.length }), true)
        }).then(result => {
            if (result === null || gen !== makeGeneration)
                return null
            return keep(pinhole.run("mv", ["-f", temp, path]), true)
        }).then(result => {
            if (result === null || gen !== makeGeneration)
                return
            making = ""
            // Said copied once the clipboard has it (a big sheet's PNG takes
            // a moment), not before, when a paste would be what was there.
            const done = { path: path, name: path.split("/").pop(), size: pinhole.sizeText(pinhole.fileSize(path)),
                           pixels: sheet.w + " × " + sheet.h, blurry: blurry, copy: "copying" }
            made = done
            const copied = how => {
                if (made === done)
                    made = Object.assign({}, done, { copy: how })
            }
            // Another copy asked for meanwhile has the clipboard: this one
            // only kept.
            pinhole.copyFile(path).then(() => copied("copied"), error => {
                copied(error.code === "cancelled" ? "kept" : "failed")
                if (error.code !== "cancelled")
                    pinhole.warn(error)
            })
        }, error => {
            if (gen !== makeGeneration || error.code === "cancelled")
                return
            fail(error.code === "timeout" ? qsTr("magick took too long")
                 : (error.message || "").trim().split("\n")[0] || qsTr("magick failed"))
        }).catch(pinhole.warn)
    }
    function fail(why) {
        making = ""
        error = why
    }

    // Its keys, while it is on top (Command.qml).
    Command {
        name: "sheet.close"
        keys: ["Escape", "K"]
        keyLayer: "plugin"
        when: studio.shown
        mouse: qsTr("The ✕ button")
        touchpad: qsTr("The ✕ button")
        onTriggered: studio.close()
    }
    Repeater {
        model: Layout.LAYOUTS
        delegate: Command {
            required property string modelData
            required property int index
            name: "sheet.layout" + (index + 1)
            text: qsTr("Grid, collage")
            section: index === 0 ? "Contact sheet" : ""
            keys: [String(index + 1)]
            keysText: "1 2"
            keyLayer: "plugin"
            when: studio.shown && studio.making === ""
            mouse: qsTr("Its button")
            touchpad: qsTr("Its button")
            pointerHelp: false
            onTriggered: studio.change({ layout: modelData })
        }
    }
    Command {
        name: "sheet.gap"
        text: qsTr("Gap")
        section: "Contact sheet"
        keys: ["G"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.cycle("gap", Layout.GAPS)
    }
    Command {
        name: "sheet.background"
        text: qsTr("Background")
        section: "Contact sheet"
        keys: ["B"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.cycle("background", Layout.BACKGROUNDS)
    }
    Command {
        name: "sheet.size"
        text: qsTr("Size")
        section: "Contact sheet"
        keys: ["Z"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.cycle("size", Layout.SIZES)
    }
    Command {
        name: "sheet.dates"
        text: qsTr("Dates under the photos")
        section: "Contact sheet"
        keys: ["D"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.change({ dates: !studio.recipe.dates })
    }
    Command {
        name: "sheet.make"
        text: qsTr("Make it, copied")
        section: "Contact sheet"
        keys: ["Return", "Enter"]
        keysText: "⏎"
        keyLayer: "plugin"
        when: studio.shown
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.make()
    }
    Command {
        id: showCommand
        name: "sheet.show"
        text: qsTr("Show the file made")
        section: "Contact sheet"
        keys: ["S"]
        keyLayer: "plugin"
        when: studio.shown && studio.made !== null
        onTriggered: studio.pinhole.showFile(studio.made.path)
    }

    // Nothing over it reaches the photos beneath.
    Panel {}

    // When, and how many.
    Chip {
        id: top
        x: 16
        y: 16
        readonly property var first: studio.photos.length > 0 ? new Date(studio.photos[0].date) : null
        readonly property var last: studio.photos.length > 0 ? new Date(studio.photos[studio.photos.length - 1].date) : null
        text: !first || !last ? ""
            : (studio.oneDay ? first.toLocaleDateString(Qt.locale(), "d MMM yyyy")
               : first.toLocaleDateString(Qt.locale(), first.getFullYear() === last.getFullYear() ? "d MMM" : "d MMM yyyy")
                 + " – " + last.toLocaleDateString(Qt.locale(), "d MMM yyyy"))
              + " <font color=\"" + theme.darkForeground + "\">· "
              + (studio.over > 0 ? qsTr("the first %1 of %2 ticked").arg(Number(studio.photos.length).toLocaleString(Qt.locale(), "f", 0))
                                                                  .arg(Number(studio.photos.length + studio.over).toLocaleString(Qt.locale(), "f", 0))
                 : qsTr("%n photo(s)", "", studio.photos.length)) + "</font>"
        textColor: theme.brightForeground
    }
    ShellButton {
        id: closeButton
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        text: "✕"
        hint: "Esc"
        onTapped: studio.close()
    }

    // What was made, or why not: on the clipboard, and where it is kept.
    // Beside the way out where there is room, else under the date.
    Row {
        id: outcome
        readonly property bool beside: top.x + top.width + 12 + width + 6 + closeButton.width + 16 <= studio.width
        x: beside ? closeButton.x - 6 - width : 16
        y: beside ? 16 : top.y + top.height + 8
        spacing: 6
        visible: studio.made !== null || studio.error !== "" || studio.note !== ""
        Chip {
            text: studio.error !== "" ? qsTr("Could not make it: %1").arg(studio.error)
                : studio.made ? (studio.made.copy === "copied" ? "✓ " + qsTr("Copied, and kept in Pictures/Pinhole · %1 · %2")
                                                                       : qsTr("Kept in Pictures/Pinhole · %1 · %2"))
                                    .arg(studio.made.pixels).arg(studio.made.size)
                                + (studio.made.copy === "copying" ? " · " + qsTr("copying…") : "")
                                + (studio.made.blurry > 0 ? " · " + qsTr("%n from thumbnails, blurrier", "", studio.made.blurry) : "")
                : studio.note
            textColor: studio.error !== "" ? theme.red : studio.made && studio.made.blurry > 0 ? theme.yellow : theme.foreground
        }
        ShellButton {
            visible: studio.made !== null
            floating: true
            text: qsTr("Show")
            action: showCommand
        }
    }

    // The sheet as it will be, as big as fits.
    Item {
        id: area
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: outcome.visible && !outcome.beside ? outcome.bottom : top.bottom
        anchors.bottom: card.top
        anchors.margins: 16
        anchors.bottomMargin: 20
        readonly property real scale: Math.min(width / studio.sheet.w, height / studio.sheet.h)

        Rectangle {
            id: paper
            anchors.centerIn: parent
            width: Math.round(studio.sheet.w * area.scale)
            height: Math.round(studio.sheet.h * area.scale)
            color: studio.colours.background
            // A hairline, so a sheet the colour of the window has an edge.
            border.width: studio.recipe.background === "theme" ? 1 : 0
            border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.18)
            Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
            Behavior on height { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

            // Many photos move at once without easing: each change a
            // frame, not an animation of a thousand.
            readonly property bool easing: studio.photos.length <= 150
            Repeater {
                model: studio.photos.length
                delegate: Item {
                    id: cell
                    required property int index
                    readonly property var photo: studio.photos[index]
                    readonly property var at: studio.sheet.cells[index] || { x: 0, y: 0, w: 0, h: 0, caption: { h: 0 } }
                    readonly property real s: paper.width / studio.sheet.w
                    x: at.x * s
                    y: at.y * s
                    width: at.w * s
                    height: at.h * s
                    Behavior on x { enabled: paper.easing; NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    Behavior on y { enabled: paper.easing; NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    Behavior on width { enabled: paper.easing; NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    Behavior on height { enabled: paper.easing; NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    Item {
                        anchors.fill: parent
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: cell.photo ? cell.photo.thumbnail : ""
                            sourceSize: Qt.size(studio.thumbSize, studio.thumbSize)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                        Image {
                            anchors.fill: parent
                            source: cell.photo ? studio.sharp[cell.photo.id] || "" : ""
                            sourceSize.width: 1024
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            opacity: status === Image.Ready ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 160 } }
                        }
                    }
                    // Its date beneath it, as magick will write it.
                    Text {
                        readonly property int points: studio.captionPoints[cell.index] || 0
                        visible: studio.recipe.dates && points > 0
                        width: parent.width
                        y: parent.height + (cell.at.caption.h - points * 1.2) / 2 * cell.s
                        horizontalAlignment: Text.AlignHCenter
                        font.family: theme.font
                        font.pixelSize: Math.max(1, points * cell.s)
                        color: studio.colours.caption
                        text: visible ? studio.dates[cell.index] || "" : ""
                    }
                }
            }
        }
    }

    // How it is laid out, and the file: as an Omarchy menu card, the
    // choices lit in the accent, as Series' studio has it.
    Rectangle {
        id: card
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: rows.width + 2
        height: rows.height + 2
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1

        Column {
            id: rows
            x: 1
            y: 1
            visible: studio.making === ""
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                Repeater {
                    model: Layout.LAYOUTS
                    delegate: ShellButton {
                        required property string modelData
                        required property int index
                        flat: true
                        text: studio.names[modelData]
                        hint: String(index + 1)
                        active: studio.recipe.layout === modelData
                        onTapped: studio.change({ layout: modelData })
                    }
                }
                ShellButton {
                    flat: true
                    text: qsTr("Dates")
                    hint: "D"
                    active: studio.recipe.dates
                    onTapped: studio.change({ dates: !studio.recipe.dates })
                }
            }
            Rectangle {
                width: parent.width
                height: 1
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.18)
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                ShellButton {
                    flat: true
                    text: studio.names[studio.recipe.gap]
                    hint: "G"
                    onTapped: studio.cycle("gap", Layout.GAPS)
                }
                ShellButton {
                    flat: true
                    text: studio.names[studio.recipe.background]
                    hint: "B"
                    onTapped: studio.cycle("background", Layout.BACKGROUNDS)
                }
                // Its longest side.
                ShellButton {
                    flat: true
                    text: qsTr("%1 px").arg(studio.recipe.size)
                    hint: "Z"
                    onTapped: studio.cycle("size", Layout.SIZES)
                }
                ShellButton {
                    active: true
                    text: qsTr("Make")
                    hint: "⏎"
                    onTapped: studio.make()
                }
            }
        }
        // Being made: how far.
        Column {
            anchors.centerIn: parent
            visible: studio.making !== ""
            spacing: 8
            readonly property real progress: studio.making === "making" ? 1
                : studio.photos.length > 0 ? studio.fetched / studio.photos.length : 0
            UiText {
                anchors.horizontalCenter: parent.horizontalCenter
                text: studio.making === "making" ? qsTr("Putting it together…")
                    : qsTr("Fetching the photos… %1 of %2").arg(studio.fetched).arg(studio.photos.length)
            }
            UiText {
                anchors.horizontalCenter: parent.horizontalCenter
                color: theme.darkForeground
                text: qsTr("Esc stops it")
            }
            Rectangle {
                width: rows.width - 4 * theme.fontSize
                height: Math.max(4, Math.round(theme.fontSize * 0.4))
                radius: height / 2
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
                Rectangle {
                    id: bar
                    width: parent.width * parent.parent.progress
                    height: parent.height
                    radius: height / 2
                    color: theme.accent
                    Behavior on width { NumberAnimation { duration: 160 } }
                    // Putting it together: breathing, as nothing says how far.
                    SequentialAnimation on opacity {
                        running: studio.making === "making"
                        loops: Animation.Infinite
                        onStopped: bar.opacity = 1
                        NumberAnimation { to: 0.35; duration: 600; easing.type: Easing.InOutSine }
                        NumberAnimation { to: 1; duration: 600; easing.type: Easing.InOutSine }
                    }
                }
            }
        }
    }
}
