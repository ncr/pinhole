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

    // The photos, oldest first: {index, aspect, thumbnail, sharp, date, video}.
    property var photos: []
    // Bumped as it opens again, so what comes for the last ones is let be.
    property int generation: 0
    // Files fetched while it is open, by position and kind.
    property var files: ({})

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

    readonly property var sheet: Layout.plan(recipe, photos.map(p => p.aspect))
    readonly property var colours: recipe.background === "white" ? { background: "#ffffff", caption: "#707070" }
        : recipe.background === "black" ? { background: "#000000", caption: "#a0a0a0" }
        : { background: String(theme.background), caption: String(theme.darkForeground) }
    // Under each photo: the day it was taken, or the time when all are of
    // one day.
    readonly property bool oneDay: photos.length > 0 && day(photos[0].date) === day(photos[photos.length - 1].date)
    readonly property var dates: photos.map(p => oneDay ? Qt.formatTime(new Date(p.date), "HH:mm")
                                                        : new Date(p.date).toLocaleDateString(Qt.locale(), "d MMM yyyy"))
    function day(ms) { return Qt.formatDate(new Date(ms), "yyyy-MM-dd") }

    // Being made: "fetching" the photos (how many are here), then
    // "making"; the last one made, or why not.
    property string making: ""
    property int fetched: 0
    property var made: null
    property string error: ""

    readonly property var names: ({
        grid: qsTr("Grid"), collage: qsTr("Collage"),
        none: qsTr("No gap"), thin: qsTr("Thin gap"), wide: qsTr("Wide gap"),
        theme: qsTr("Theme"), white: qsTr("White"), black: qsTr("Black")
    })

    function open(indexes) {
        if (making !== "")
            return
        ++generation
        photos = Array.from(indexes).map(i => {
            const item = pinhole.library.item(i)
            return { index: i, aspect: item.width > 0 && item.height > 0 ? item.width / item.height : 4 / 3,
                     thumbnail: String(item.thumbnail), sharp: "", date: item.date, video: item.video }
        }).sort((a, b) => a.date - b.date)
        files = {}
        made = null
        error = ""
        shown = true
        // The sharper pictures for the preview, as each comes; for a few
        // dozen, past which a cell is small enough for the thumbnail.
        const gen = generation
        for (const p of photos.slice(0, 48)) {
            fetchFile(p, "medium", path => {
                if (gen !== generation || path === "")
                    return
                studio.photos = studio.photos.map(q => q.index === p.index ? Object.assign({}, q, { sharp: "file://" + path }) : q)
            })
        }
    }
    function close() {
        if (making === "making")
            return
        // Fetching is let go; what comes is not waited for.
        ++generation
        making = ""
        shown = false
    }
    // A photo's file, once: done(path), "" if it could not be had.
    function fetchFile(p, kind, done) {
        const key = p.index + ":" + kind
        if (files[key] !== undefined) {
            done(files[key])
            return
        }
        pinhole.fetch(p.index, kind, (path, error) => {
            if (path !== "")
                files[key] = path
            done(path)
        })
    }
    function localPath(url) {
        return url.startsWith("file://") ? decodeURIComponent(url.slice(7)) : ""
    }

    // The sheet: each photo fetched as large as its cell needs (the
    // medium, about 2048 px, unless the cell is near that), then magick.
    function make() {
        if (making !== "" || photos.length === 0)
            return
        const gen = generation
        const sheet = studio.sheet
        const shot = photos.slice()
        const paths = new Array(shot.length)
        making = "fetching"
        fetched = 0
        made = null
        error = ""
        let left = shot.length
        shot.forEach((p, n) => {
            const cell = sheet.cells[n]
            const kind = p.video || Math.max(cell.w, cell.h) <= 1280 ? "medium" : "full"
            const take = path => {
                if (gen !== generation)
                    return
                paths[n] = path
                ++fetched
                if (--left === 0)
                    render(sheet, shot, paths)
            }
            // Without the full photo, the medium; without that, the
            // thumbnail, better than none.
            fetchFile(p, kind, path => {
                if (path !== "" || kind === "medium")
                    take(path !== "" ? path : localPath(p.thumbnail))
                else
                    fetchFile(p, "medium", medium => take(medium !== "" ? medium : localPath(p.thumbnail)))
            })
        })
    }
    function render(sheet, shot, paths) {
        if (paths.some(path => path === "")) {
            fail(qsTr("a photo could not be fetched"))
            return
        }
        making = "making"
        const dir = pinhole.picturesDir() + "/Pinhole"
        const base = dir + "/" + day(shot[0].date) + " " + (recipe.layout === "grid" ? qsTr("Contact sheet") : qsTr("Collage"))
        let path = base + ".jpg"
        for (let n = 2; pinhole.exists(path); ++n)
            path = base + " " + n + ".jpg"
        // Made beside, then moved in whole.
        const temp = pinhole.cacheDir + "/sheet.jpg"
        const args = Layout.magickArguments(sheet, paths, colours, recipe.dates ? dates : [], plugin.fontFile, temp)
        pinhole.run("mkdir", ["-p", dir, pinhole.cacheDir], code => {
            if (code !== 0) {
                fail(qsTr("Pictures/Pinhole could not be made"))
                return
            }
            pinhole.run("magick", args, (code, out, err) => {
                if (code !== 0) {
                    fail(err.trim().split("\n")[0] || qsTr("magick failed"))
                    return
                }
                pinhole.run("mv", ["-f", temp, path], (code, out, err) => {
                    making = ""
                    if (code !== 0) {
                        error = err.trim()
                        return
                    }
                    pinhole.copyFile(path)
                    made = { path: path, name: path.split("/").pop(), size: pinhole.sizeText(pinhole.fileSize(path)),
                             pixels: sheet.w + " × " + sheet.h }
                })
            })
        })
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

    Rectangle {
        anchors.fill: parent
        color: theme.background
    }
    // Clicks stay here, off the photos beneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
    }

    // When, and how many.
    Chip {
        id: top
        x: 16
        y: 16
        readonly property var first: studio.photos.length > 0 ? new Date(studio.photos[0].date) : null
        readonly property var last: studio.photos.length > 0 ? new Date(studio.photos[studio.photos.length - 1].date) : null
        text: (!first ? ""
               : studio.oneDay ? first.toLocaleDateString(Qt.locale(), "d MMM yyyy")
               : first.toLocaleDateString(Qt.locale(), first.getFullYear() === last.getFullYear() ? "d MMM" : "d MMM yyyy")
                 + " – " + last.toLocaleDateString(Qt.locale(), "d MMM yyyy"))
              + " <font color=\"" + theme.darkForeground + "\">· " + qsTr("%n photo(s)", "", studio.photos.length) + "</font>"
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
        visible: studio.made !== null || studio.error !== ""
        Chip {
            text: studio.error !== "" ? qsTr("Could not make it: %1").arg(studio.error)
                : studio.made ? "✓ " + qsTr("Copied, and kept in Pictures/Pinhole · %1 · %2").arg(studio.made.pixels).arg(studio.made.size) : ""
            textColor: studio.error !== "" ? theme.red : theme.foreground
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
                    Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    Behavior on height { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    clip: true
                    Image {
                        anchors.fill: parent
                        source: cell.photo ? cell.photo.thumbnail : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }
                    Image {
                        anchors.fill: parent
                        source: cell.photo ? cell.photo.sharp : ""
                        sourceSize.width: 1024
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        opacity: status === Image.Ready ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 160 } }
                    }
                    // Its date beneath it, as magick will write it.
                    Text {
                        parent: paper
                        visible: studio.recipe.dates
                        x: cell.x
                        width: cell.width
                        y: cell.y + cell.height + (cell.at.caption.h * cell.s - font.pixelSize * 1.2) / 2
                        horizontalAlignment: Text.AlignHCenter
                        font.family: theme.font
                        font.pixelSize: Math.max(1, studio.sheet.cells.length > 0 ? studio.sheet.cells[0].caption.h * 0.5 * cell.s : 1)
                        color: studio.colours.caption
                        text: studio.dates[cell.index] || ""
                        elide: Text.ElideRight
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
