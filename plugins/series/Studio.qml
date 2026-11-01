import QtQuick
import Pinhole.Plugin
import "Engine.js" as Engine

// One series as its clip, full window: playing at once, with the ways to
// change it beneath, every one a click or a key, and nothing like a
// timeline. ← → go through the series; Enter makes a GIF, copied to the
// clipboard and kept in Pictures/Pinhole.
Item {
    id: studio
    objectName: "seriesStudio"
    required property var plugin
    readonly property var pinhole: plugin.pinhole

    property bool shown: false
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    // Which series, and whether it was opened from the grid (closing then
    // shows the grid at it) or from a photo (closing goes back to it).
    property int k: -1
    property bool fromGrid: true
    readonly property var positions: k >= 0 && k < plugin.series.length ? plugin.series[k] : []

    // Every shot of the series: {index, thumbnail, sharp, aspect, shift}.
    property var all: []
    // Shots left out, by series (their first photo's id): for as long as
    // the window runs.
    property var leftOut: ({})
    readonly property string key: positions.length > 0 ? pinhole.library.item(positions[0]).id : ""
    readonly property var skipped: leftOut[key] || []
    readonly property var shots: all.filter(s => skipped.indexOf(s.index) < 0)

    // How the clip is made; all but the crop is kept for next time.
    property var recipe: Object.assign({}, Engine.DEFAULT_RECIPE, pinhole.settings.recipe, { pan: { x: 0, y: 0 } })
    function change(part) {
        recipe = Object.assign({}, recipe, part)
        const kept = Object.assign({}, recipe)
        delete kept.pan
        pinhole.settings.recipe = kept
    }
    function cycle(name, list) {
        const at = list.indexOf(recipe[name])
        const part = {}
        part[name] = list[(at + 1) % list.length]
        change(part)
    }

    property real time: 0
    property bool playing: true
    readonly property real duration: preview.timeline.duration

    // A file being made, how far, and the last one made.
    property string making: ""
    property real progress: 0
    property var made: null
    property string error: ""

    readonly property var names: ({
        flip: qsTr("Flip"), bounce: qsTr("Bounce"), fade: qsTr("Fade"), drift: qsTr("Drift"), pile: qsTr("Pile"),
        slow: qsTr("Slow"), normal: qsTr("Normal"), fast: qsTr("Fast"),
        natural: qsTr("Natural"), vivid: qsTr("Vivid"), warm: qsTr("Warm"), mono: qsTr("Mono"), film: qsTr("Film"),
        original: qsTr("Original"), square: qsTr("Square"), tall: "4:5", story: qsTr("Story")
    })

    function openSeries(k, fromGrid) {
        if (k < 0 || k >= plugin.series.length)
            return
        studio.fromGrid = fromGrid
        show(k)
        shown = true
    }
    function show(k) {
        studio.k = k
        made = null
        error = ""
        time = 0
        playing = true
        change({ pan: { x: 0, y: 0 } })
        const positions = plugin.series[k]
        all = positions.map(i => {
            const item = pinhole.library.item(i)
            return { index: i, id: item.id, thumbnail: item.thumbnail, sharp: "",
                     aspect: item.width > 0 && item.height > 0 ? item.width / item.height : 4 / 3, shift: { x: 0, y: 0 } }
        })
        // The sharper pictures, as each comes; a thumbnail stays where one
        // does not.
        for (const i of positions) {
            pinhole.fetch(i, "medium").then(path => {
                if (studio.k === k)
                    studio.all = studio.all.map(s => s.index === i ? Object.assign({}, s, { sharp: "file://" + path }) : s)
            }, () => {}).catch(pinhole.warn)
        }
        // How each is moved against the first, for Steady: the last
        // series' no longer wanted.
        if (steadying)
            steadying.cancel()
        steadying = pinhole.run(plugin.finder, ["steady", pinhole.library.dir].concat(all.map(s => s.id)))
        steadying.then(result => {
            const lines = result.stdout.trim().split("\n")
            studio.all = studio.all.map((s, n) => {
                const parts = (lines[n] || "0 0").split(" ")
                return Object.assign({}, s, { shift: { x: Number(parts[0]) || 0, y: Number(parts[1]) || 0 } })
            })
        }, error => {
            if (error.code !== "cancelled")
                throw error
        }).catch(pinhole.warn)
    }
    property var steadying: null
    function step(delta) {
        const next = k + delta
        if (next >= 0 && next < plugin.series.length)
            show(next)
    }
    function close() {
        // Escape while a file is made stops making it.
        if (making !== "") {
            if (recording)
                recording.cancel()
            return
        }
        shown = false
        if (fromGrid && positions.length > 0)
            pinhole.reveal(positions[0])
    }
    // A shot in or out; two at least stay in.
    function toggle(index) {
        const now = skipped.slice()
        const at = now.indexOf(index)
        if (at >= 0)
            now.splice(at, 1)
        else if (all.length - now.length > 2)
            now.push(index)
        else
            return
        const map = Object.assign({}, leftOut)
        map[key] = now
        leftOut = map
    }

    // The file: named for when the series was taken and how it moves, in
    // Pictures/Pinhole, then on the clipboard.
    function make(format) {
        if (making !== "" || shots.length < 2)
            return
        const plan = Engine.plan(recipe, shots, format)
        const date = new Date(pinhole.library.item(positions[0]).date)
        const base = pinhole.picturesDir() + "/Pinhole/" + Qt.formatDateTime(date, "yyyy-MM-dd HH.mm") + " " + names[recipe.motion]
        const extension = format === "gif" ? ".gif" : ".mp4"
        let path = base + extension
        for (let n = 2; pinhole.exists(path); ++n)
            path = base + " " + n + extension
        recorder.width = plan.w
        recorder.height = plan.h
        recorder.time = 0
        making = format
        progress = 0
        made = null
        error = ""
        // Once the pictures at the file's size are here.
        recordWhenReady.plan = plan
        recordWhenReady.path = path
        recordWhenReady.restart()
    }
    // The file on its way (pinhole.record), to cancel.
    property var recording: null
    Timer {
        id: recordWhenReady
        property var plan
        property string path
        interval: 50
        repeat: true
        onTriggered: {
            if (!recorder.ready)
                return
            stop()
            const fps = plan.fps
            studio.recording = studio.pinhole.record(recorder, {
                path: path, format: studio.making, width: plan.w, height: plan.h, fps: fps, frames: plan.frames,
                seek: frame => recorder.time = frame / fps,
                progress: fraction => studio.progress = fraction
            })
            studio.recording.then(file => {
                studio.making = ""
                studio.recording = null
                // Said copied once the clipboard has it (a GIF's first
                // frame is made a PNG first), not before.
                const done = { path: file, name: file.split("/").pop(), size: studio.pinhole.sizeText(studio.pinhole.fileSize(file)),
                               copy: "copying" }
                studio.made = done
                const copied = how => {
                    if (studio.made === done)
                        studio.made = Object.assign({}, done, { copy: how })
                }
                // Another copy asked for meanwhile has the clipboard: this
                // one only kept.
                studio.pinhole.copyFile(file).then(() => copied("copied"), error => {
                    copied(error.code === "cancelled" ? "kept" : "failed")
                    if (error.code !== "cancelled")
                        studio.pinhole.warn(error)
                })
            }, error => {
                studio.making = ""
                studio.recording = null
                if (error.code !== "cancelled")
                    studio.error = error.message
            }).catch(studio.pinhole.warn)
        }
    }

    // The video made as the desktop background, by the Background plugin's
    // own script, beside this plugin's folder: kept as it keeps one, in
    // Pictures/Pinhole/.backgrounds, where no folder source shows it and no
    // cache clean takes it, and the one before remembered with its theme.
    // Ctrl+Z puts that one back, as the Background plugin's does.
    // Offered only where that script is: not when Series is a copy among
    // one's own plugins, or Background is not installed.
    property bool settingBackground: false
    readonly property string backgroundScript: pinhole.exists(pinhole.dir + "/../background/background")
                                               ? pinhole.dir + "/../background/background" : ""
    function makeBackground() {
        if (!made || settingBackground || backgroundScript === "")
            return
        let kept = "", before = null
        settingBackground = true
        pinhole.run(backgroundScript, ["keep", made.path, pinhole.picturesDir() + "/Pinhole/.backgrounds",
                                       made.name.replace(/\.[^.]*$/, ""), "mp4", "own"])
            .then(result => {
                kept = result.stdout.trim()
                return pinhole.run(backgroundScript, ["current"]).then(result => {
                    const lines = result.stdout.split("\n")
                    return { theme: lines[0].trim(), path: (lines[1] || "").trim() }
                }, () => ({ theme: "", path: "" }))
            })
            .then(now => {
                before = now
                return pinhole.run(backgroundScript, ["set", kept, ""])
            })
            .then(() => {
                pinhole.notify(qsTr("Your moving desktop background now"))
                if (before.path !== "" && before.path !== kept)
                    pinhole.did(qsTr("setting the background"), () => setBackground(before),
                                () => setBackground({ path: kept, theme: before.theme }))
            })
            .catch(backgroundFailed)
            .then(() => settingBackground = false)
    }
    // Undone or done again: that one, if it is still there.
    function setBackground(background) {
        pinhole.run(backgroundScript, ["set", background.path, background.theme]).catch(error => {
            if (error.exitCode === 3)
                pinhole.notify(qsTr("The background before went with the theme %1").arg(background.theme))
            else if (error.exitCode === 4)
                pinhole.notify(qsTr("The background before is gone: %1").arg(background.path.split("/").pop()))
            else
                backgroundFailed(error)
        })
    }
    function backgroundFailed(error) {
        if (error.code === "cancelled")
            return
        const why = String(error.message || error.code || error).trim().split("\n").pop()
        pinhole.notify(qsTr("Could not set the background: %1").arg(why))
    }

    FrameAnimation {
        running: studio.shown && studio.playing && studio.making === ""
        onTriggered: studio.time = (studio.time + frameTime) % Math.max(0.05, studio.duration)
    }

    // Its keys, while it is on top (Command.qml).
    Command {
        name: "studio.previous"
        text: qsTr("Previous, next series")
        section: "Series"
        keys: ["Left"]
        keysText: "← →"
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("The ‹ › buttons")
        touchpad: qsTr("The ‹ › buttons")
        pointerHelp: false
        onTriggered: studio.step(-1)
    }
    Command {
        name: "studio.next"
        keys: ["Right"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("The ‹ › buttons")
        touchpad: qsTr("The ‹ › buttons")
        onTriggered: studio.step(1)
    }
    Command {
        name: "studio.close"
        keys: ["Escape", "G"]
        keyLayer: "plugin"
        when: studio.shown
        mouse: qsTr("The ✕ button")
        touchpad: qsTr("The ✕ button")
        onTriggered: studio.close()
    }
    Command {
        id: pauseCommand
        name: "studio.pause"
        text: qsTr("Pause, play")
        section: "Series"
        keys: ["Space"]
        keyLayer: "plugin"
        when: studio.shown
        onTriggered: studio.playing = !studio.playing
    }
    Command {
        name: "studio.pace"
        text: qsTr("Pace")
        section: "Series"
        keys: ["P"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.cycle("pace", Engine.PACES)
    }
    Command {
        name: "studio.look"
        text: qsTr("Look")
        section: "Series"
        keys: ["L"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.cycle("look", Engine.LOOKS)
    }
    Command {
        name: "studio.shape"
        text: qsTr("Shape")
        section: "Series"
        keys: ["O"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.cycle("shape", Engine.SHAPES)
    }
    Command {
        name: "studio.steady"
        text: qsTr("Steady")
        section: "Series"
        keys: ["T"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.change({ steady: !studio.recipe.steady })
    }
    Command {
        name: "studio.gif"
        text: qsTr("Make a GIF")
        section: "Series"
        keys: ["Return", "Enter"]
        keysText: "⏎"
        keyLayer: "plugin"
        when: studio.shown
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.make("gif")
    }
    Command {
        name: "studio.video"
        text: qsTr("Make a video")
        section: "Series"
        keys: ["Shift+Return", "Shift+Enter"]
        keysText: "⇧ ⏎"
        keyLayer: "plugin"
        when: studio.shown
        mouse: qsTr("Its button")
        touchpad: qsTr("Its button")
        pointerHelp: false
        onTriggered: studio.make("mp4")
    }
    Repeater {
        model: Engine.MOTIONS
        delegate: Command {
            required property string modelData
            required property int index
            name: "studio.motion" + (index + 1)
            text: qsTr("Motion")
            section: index === 0 ? "Series" : ""
            keys: [String(index + 1)]
            keysText: "1–" + Engine.MOTIONS.length
            keyLayer: "plugin"
            when: studio.shown && studio.making === ""
            mouse: qsTr("Its button")
            touchpad: qsTr("Its button")
            pointerHelp: false
            onTriggered: { studio.change({ motion: modelData }); studio.time = 0 }
        }
    }

    // The crop moved with ⇧ and the arrows, a quarter of its room a press,
    // and 0 puts it back in the middle.
    function nudgeCrop(dx, dy) {
        const room = preview.room
        const pan = studio.recipe.pan
        studio.change({ pan: {
            x: room.x > 1 ? Math.max(-1, Math.min(1, pan.x + dx * 0.25)) : 0,
            y: room.y > 1 ? Math.max(-1, Math.min(1, pan.y + dy * 0.25)) : 0
        } })
    }
    Repeater {
        model: [["Shift+Left", -1, 0], ["Shift+Right", 1, 0], ["Shift+Up", 0, -1], ["Shift+Down", 0, 1]]
        delegate: Command {
            required property var modelData
            required property int index
            name: "studio.crop" + index
            text: qsTr("Move the crop")
            section: index === 0 ? "Series" : ""
            keys: [modelData[0]]
            keysText: "⇧ ← → ↑ ↓"
            keyLayer: "plugin"
            when: studio.shown && studio.making === ""
            autoRepeat: true
            mouse: qsTr("Drag the picture")
            touchpad: qsTr("Three fingers on it")
            onTriggered: studio.nudgeCrop(modelData[1], modelData[2])
        }
    }
    Command {
        name: "studio.centre"
        keys: ["0"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        mouse: qsTr("Double-click the picture")
        touchpad: qsTr("Double-tap the picture")
        onTriggered: studio.change({ pan: { x: 0, y: 0 } })
    }
    // A shot chosen with [ and ], and X leaves it out or takes it back.
    property int shotCursor: 0
    Command {
        name: "studio.shotBack"
        text: qsTr("Choose a shot")
        section: "Series"
        keys: ["["]
        keysText: "[ ]"
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        autoRepeat: true
        mouse: qsTr("Point at the strip")
        touchpad: qsTr("Point at the strip")
        onTriggered: studio.shotCursor = Math.max(0, studio.shotCursor - 1)
        pointerHelp: false
    }
    Command {
        name: "studio.shotOn"
        keys: ["]"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === ""
        autoRepeat: true
        mouse: qsTr("Point at the strip")
        touchpad: qsTr("Point at the strip")
        onTriggered: studio.shotCursor = Math.min(studio.all.length - 1, studio.shotCursor + 1)
    }
    Command {
        name: "studio.leaveOut"
        text: qsTr("Leave the shot out, or back in")
        section: "Series"
        keys: ["X"]
        keyLayer: "plugin"
        when: studio.shown && studio.making === "" && studio.shotCursor < studio.all.length
        mouse: qsTr("Click it in the strip")
        touchpad: qsTr("Tap it in the strip")
        onTriggered: studio.toggle(studio.all[studio.shotCursor].index)
    }
    Command {
        id: showCommand
        name: "studio.show"
        text: qsTr("Show the file made")
        section: "Series"
        keys: ["S"]
        keyLayer: "plugin"
        when: studio.shown && studio.made !== null
        onTriggered: studio.pinhole.showFile(studio.made.path)
    }
    Command {
        id: backgroundCommand
        name: "studio.background"
        text: qsTr("The video as the background")
        section: "Series"
        keys: ["B"]
        keyLayer: "plugin"
        when: studio.shown && studio.made !== null && studio.made.path.endsWith(".mp4") && !studio.settingBackground
              && studio.backgroundScript !== ""
        onTriggered: studio.makeBackground()
    }

    // The file's frames are drawn here, at its size, out of sight under
    // the studio (a picture is taken of it, not of the screen).
    Clip {
        id: recorder
        visible: studio.making !== ""
        shots: studio.shots
        recipe: studio.recipe
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

    // The series: when, how many, which of how many.
    Row {
        id: top
        x: 16
        y: 16
        spacing: 6
        Chip {
            readonly property var item: studio.positions.length > 0 ? studio.pinhole.library.item(studio.positions[0]) : null
            readonly property var date: item ? new Date(item.date) : null
            text: date ? date.toLocaleDateString(Qt.locale(), Locale.LongFormat) + " <font color=\"" + theme.darkForeground + "\">· "
                         + date.toLocaleTimeString(Qt.locale(), Locale.ShortFormat) + "</font>" : ""
            textColor: theme.brightForeground
        }
        ShellButton {
            text: "‹"
            hint: "←"
            usable: studio.k > 0 && studio.making === ""
            onTapped: studio.step(-1)
        }
        Chip {
            text: qsTr("%1 of %2").arg(Number(studio.k + 1).toLocaleString(Qt.locale(), "f", 0))
                                  .arg(Number(studio.plugin.series.length).toLocaleString(Qt.locale(), "f", 0))
            textColor: theme.darkForeground
        }
        ShellButton {
            text: "›"
            hint: "→"
            usable: studio.k < studio.plugin.series.length - 1 && studio.making === ""
            onTapped: studio.step(1)
        }
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

    // The clip, as big as fits; drag it to move the crop.
    Item {
        id: area
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: top.bottom
        anchors.bottom: strip.top
        anchors.margins: 16
        readonly property real ratio: Engine.ratio(studio.recipe, studio.shots)
        Clip {
            id: preview
            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height * area.ratio)
            height: width / area.ratio
            shots: studio.shots
            recipe: studio.recipe
            time: studio.time
            readonly property var room: geometry.room
            HoverHandler {
                cursorShape: preview.room.x > 1 || preview.room.y > 1 ? (drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor) : Qt.ArrowCursor
            }
            DragHandler {
                id: drag
                target: null
                property var from: ({ x: 0, y: 0 })
                onActiveChanged: if (active) from = studio.recipe.pan
                onTranslationChanged: {
                    const room = preview.room
                    studio.change({ pan: {
                        x: room.x > 1 ? Math.max(-1, Math.min(1, from.x + translation.x / room.x)) : 0,
                        y: room.y > 1 ? Math.max(-1, Math.min(1, from.y + translation.y / room.y)) : 0
                    } })
                }
            }
            TapHandler { onDoubleTapped: studio.change({ pan: { x: 0, y: 0 } }) }
        }
        // Paused: said on the picture's corner, quietly.
        Chip {
            visible: !studio.playing
            x: preview.x + 10
            y: preview.y + 10
            text: "❚❚"
        }
    }

    // Every shot; a click leaves one out or takes it back. The one showing
    // is marked.
    Row {
        id: strip
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: card.top
        anchors.bottomMargin: 14
        spacing: 4
        readonly property real cell: 44
        readonly property int showing: {
            const m = preview.moment
            const shot = studio.shots[m.a]
            return shot ? shot.index : -1
        }
        Repeater {
            model: studio.all
            delegate: Item {
                required property var modelData
                readonly property bool out: studio.skipped.indexOf(modelData.index) >= 0
                width: Math.round(strip.cell * Math.min(1.5, modelData.aspect))
                height: strip.cell + 6
                Image {
                    width: parent.width
                    height: strip.cell
                    source: modelData.thumbnail
                    sourceSize.height: strip.cell * 2
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    opacity: parent.out ? 0.25 : 1
                    Behavior on opacity { NumberAnimation { duration: 120 } }
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 2
                    radius: 1
                    color: theme.accent
                    visible: strip.showing === modelData.index
                }
                // The one [ and ] are on, while the keyboard is in use.
                Rectangle {
                    width: parent.width
                    height: strip.cell
                    color: "transparent"
                    border.width: 2
                    border.color: theme.brightForeground
                    visible: keymap.input === "keyboard" && studio.all[studio.shotCursor]?.index === modelData.index
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: studio.toggle(modelData.index) }
            }
        }
    }

    // What was made, or why not: on the clipboard, and where it is kept.
    // At the top, beside the way out, clear of the picture.
    Row {
        anchors.right: closeButton.left
        anchors.rightMargin: 6
        anchors.top: parent.top
        anchors.topMargin: 16
        spacing: 6
        visible: studio.made !== null || studio.error !== ""
        Chip {
            text: studio.error !== "" ? qsTr("Could not make it: %1").arg(studio.error)
                : studio.made ? (studio.made.copy === "copied" ? "✓ " + qsTr("Copied, and kept in Pictures/Pinhole: %1 · %2")
                                                               : qsTr("Kept in Pictures/Pinhole: %1 · %2"))
                                    .arg(studio.made.name).arg(studio.made.size)
                                + (studio.made.copy === "copying" ? " · " + qsTr("copying…") : "")
                : ""
            textColor: studio.error !== "" ? theme.red : theme.foreground
        }
        ShellButton {
            visible: studio.made !== null
            floating: true
            text: qsTr("Show")
            action: showCommand
        }
        ShellButton {
            visible: studio.made !== null && studio.made.path.endsWith(".mp4") && studio.backgroundScript !== ""
            floating: true
            text: qsTr("Background")
            busy: studio.settingBackground
            action: backgroundCommand
        }
    }

    // How it moves, at what pace, in what look and shape, and the file:
    // as an Omarchy menu card, the choices lit in the accent.
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
                    model: Engine.MOTIONS
                    delegate: ShellButton {
                        required property string modelData
                        required property int index
                        flat: true
                        text: studio.names[modelData]
                        hint: String(index + 1)
                        active: studio.recipe.motion === modelData
                        onTapped: { studio.change({ motion: modelData }); studio.time = 0 }
                    }
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
                    text: studio.names[studio.recipe.pace]
                    hint: "P"
                    onTapped: studio.cycle("pace", Engine.PACES)
                }
                ShellButton {
                    flat: true
                    text: studio.names[studio.recipe.look]
                    hint: "L"
                    onTapped: studio.cycle("look", Engine.LOOKS)
                }
                ShellButton {
                    flat: true
                    text: studio.names[studio.recipe.shape]
                    hint: "O"
                    onTapped: studio.cycle("shape", Engine.SHAPES)
                }
                ShellButton {
                    flat: true
                    text: qsTr("Steady")
                    hint: "T"
                    active: studio.recipe.steady && studio.recipe.motion !== "pile"
                    usable: studio.recipe.motion !== "pile"
                    onTapped: studio.change({ steady: !studio.recipe.steady })
                }
                ShellButton {
                    flat: true
                    text: studio.playing ? qsTr("Pause") : qsTr("Play")
                    action: pauseCommand
                }
                ShellButton {
                    flat: true
                    text: qsTr("Video")
                    hint: "⇧ ⏎"
                    onTapped: studio.make("mp4")
                }
                ShellButton {
                    active: true
                    text: qsTr("Make GIF")
                    hint: "⏎"
                    onTapped: studio.make("gif")
                }
            }
        }
        // Being made: how far.
        Column {
            anchors.centerIn: parent
            visible: studio.making !== ""
            spacing: 8
            UiText {
                anchors.horizontalCenter: parent.horizontalCenter
                text: (studio.making === "gif" ? qsTr("Making the GIF…") : qsTr("Making the video…"))
                      + "  " + Math.round(studio.progress * 100) + "%"
            }
            Rectangle {
                width: rows.width - 4 * theme.fontSize
                height: Math.max(4, Math.round(theme.fontSize * 0.4))
                radius: height / 2
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
                Rectangle {
                    width: parent.width * studio.progress
                    height: parent.height
                    radius: height / 2
                    color: theme.accent
                }
            }
        }
    }
}
