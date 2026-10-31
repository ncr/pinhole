import QtQuick
import Pinhole.Plugin

// The photos to send and where to: the LocalSend devices nearby as they
// answer, each on its number, the files fetched meanwhile. Picking one asks
// its owner, and the files go once they say yes, the bar filling as they
// do. A send goes on with the panel closed; the button turns while it does,
// and opens it again.
Item {
    id: panel
    required property var plugin
    readonly property var pinhole: plugin.pinhole

    property bool shown: false
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    // The photos, as they were when it opened: {id, filename, video, live}.
    property var chosen: []
    // Their files, once here, in order: {path, name}; a Live Photo's video
    // after its photo.
    property var files: []
    property bool preparing: false
    property int missing: 0
    // Fetches of an earlier opening are let go.
    property int generation: 0

    // The devices that answered, nearest address last said: by
    // fingerprint, in the order shown.
    property var devices: []
    // The run listening for them, while it does.
    property var looking: null

    // The send: "" none, "waiting" (for the files), "asking", "sending",
    // "sent", "declined", "busy", "pin", "failed", "cancelled".
    property string phase: ""
    property var target: null
    property real sentBytes: 0
    property real totalBytes: 0
    property int sentFiles: 0
    property string failure: ""
    property var sending: null
    readonly property bool running: phase === "waiting" || phase === "asking" || phase === "sending"

    readonly property int videos: chosen.filter(item => item.live && !item.video).length

    function open(indexes) {
        if (!running) {
            const list = []
            for (let k = 0; k < indexes.length; ++k) {
                const item = pinhole.library.item(indexes[k])
                if (item && item.id)
                    list.push({ id: item.id, filename: item.filename, video: item.video, live: item.live })
            }
            if (list.length === 0)
                return
            chosen = list
            phase = ""
            prepare()
        }
        shown = true
        look()
    }
    function close() {
        shown = false
        stopLooking()
    }

    // The files: originals as taken; HEIC as JPEG, the photo as it looks
    // now (an edit, if it has one), as Export does. Videos as they are.
    function prepare() {
        const job = ++generation
        const wanted = []
        for (const item of chosen) {
            wanted.push({ item: item, kind: item.video || !plugin.compatible ? "original" : "full" })
            if (item.live && !item.video)
                wanted.push({ item: item, kind: "live" })
        }
        const got = new Array(wanted.length)
        let left = wanted.length
        files = []
        missing = 0
        preparing = true
        const done = (k, file) => {
            if (job !== generation)
                return
            got[k] = file
            if (!file)
                ++missing
            if (--left > 0)
                return
            files = got.filter(f => f)
            preparing = false
            if (phase === "waiting")
                start()
        }
        wanted.forEach((w, k) => {
            pinhole.fetch(w.item.id, w.kind).then(path => {
                if (w.kind !== "live" && plugin.compatible && /\.hei[cf]$/i.test(path))
                    return plugin.jpeg(path)
                return path
            }).then(path => done(k, { path: path, name: nameOf(w.item.filename, path) }), error => {
                if (job === generation)
                    pinhole.warn(error)
                done(k, null)
            }).catch(pinhole.warn)
        })
    }
    // The camera's name with the file's own extension (an edit comes as a
    // JPEG, a Live Photo's video as a MOV), in the camera's case.
    function nameOf(filename, path) {
        const ext = path.replace(/^.*\./, "")
        if (!filename)
            return path.replace(/^.*\//, "")
        const dot = filename.lastIndexOf(".")
        const base = dot > 0 ? filename.slice(0, dot) : filename
        const was = dot > 0 ? filename.slice(dot + 1) : ""
        if (was.toLowerCase() === ext.toLowerCase())
            return filename
        return base + "." + (was !== "" && was === was.toUpperCase() ? ext.toUpperCase() : ext)
    }

    // The devices nearby: asked where they were last, announced to, and
    // listened for, while the panel shows.
    function look() {
        if (looking)
            return
        devices = []
        const known = plugin.known().map(d => JSON.stringify(d))
        const run = pinhole.run(plugin.sender, ["devices"].concat(known), { output: line => found(line) })
        looking = run
        run.then(() => {}, error => {
            if (error.code !== "cancelled")
                pinhole.warn(error)
        }).catch(pinhole.warn).then(() => {
            if (looking === run)
                looking = null
        })
    }
    function stopLooking() {
        if (looking)
            looking.cancel()
    }
    function found(line) {
        let device
        try {
            device = JSON.parse(line)
        } catch (e) {
            return
        }
        if (!device.fingerprint)
            return
        const list = devices.slice()
        const at = list.findIndex(d => d.fingerprint === device.fingerprint)
        if (at >= 0) {
            list[at] = device
        } else {
            // Where it was in the list sent to last, else after the rest.
            const order = plugin.known().map(d => d.fingerprint)
            const rank = d => { const r = order.indexOf(d.fingerprint); return r < 0 ? order.length : r }
            list.push(device)
            list.sort((a, b) => rank(a) - rank(b))
        }
        devices = list
    }

    function sendTo(device) {
        if (running || !device)
            return
        target = device
        sentBytes = 0
        totalBytes = 0
        sentFiles = 0
        failure = ""
        plugin.remember(device)
        phase = "waiting"
        if (!preparing)
            start()
    }
    function start() {
        if (files.length === 0) {
            phase = "failed"
            failure = qsTr("The photos could not be fetched.")
            return
        }
        phase = "asking"
        const args = ["send", JSON.stringify(target)]
        for (const file of files)
            args.push(file.path, file.name)
        // What it says is heard as it goes; ended any way but by saying so,
        // it failed.
        const run = pinhole.run(plugin.sender, args, { output: line => heard(line) })
        sending = run
        run.then(() => {}, error => {
            if (phase === "asking" || phase === "sending") {
                phase = "failed"
                failure = error.message || qsTr("It stopped.")
            }
        }).catch(pinhole.warn).then(() => {
            if (sending === run)
                sending = null
        })
    }
    function heard(line) {
        const words = line.split(" ")
        switch (words[0]) {
        case "accepted":
            phase = "sending"
            totalBytes = Number(words[2])
            break
        case "progress":
            sentBytes = Number(words[1])
            break
        case "sent":
            sentFiles = Number(words[1])
            sentBytes = totalBytes
            phase = "sent"
            if (shown)
                closing.restart()
            break
        case "declined":
        case "busy":
        case "pin":
        case "cancelled":
            phase = words[0]
            break
        case "failed":
            phase = "failed"
            failure = words.slice(1).join(" ")
            break
        }
    }
    function stopSending() {
        if (phase === "waiting")
            phase = "cancelled"
        else if (sending)
            sending.cancel()
    }
    // A moment to read that they went.
    Timer { id: closing; interval: 1600; onTriggered: if (panel.phase === "sent") panel.close() }

    // LocalSend's own window, with the files: for a device that does not
    // show here, or one with a PIN.
    function openLocalSend() {
        if (preparing || files.length === 0)
            return
        const folder = pinhole.cacheDir + "/localsend/" + Date.now()
        const args = ["stage", folder]
        for (const file of files)
            args.push(file.path, file.name)
        pinhole.run(plugin.sender, args).then(result => {
            // Handed to LocalSend's window, or to the one already running.
            pinhole.run("localsend", result.stdout.split("\n").filter(l => l !== "")).catch(pinhole.warn)
            panel.close()
        }).catch(pinhole.warn)
    }

    readonly property string what: chosen.every(item => item.video) ? qsTr("Send %n video(s)", "", chosen.length)
                                                                   : qsTr("Send %n photo(s)", "", chosen.length)
    readonly property string also: videos > 0 ? qsTr("With the video of %n Live Photo(s)", "", videos) : ""
    readonly property string status: {
        const name = target ? target.alias : ""
        switch (phase) {
        case "waiting": return qsTr("Fetching the photos for %1…").arg(name)
        case "asking": return qsTr("Waiting for %1 to accept…").arg(name)
        case "sending": return qsTr("Sending to %1: %2 of %3").arg(name).arg(pinhole.sizeText(sentBytes)).arg(pinhole.sizeText(totalBytes))
        case "sent": return qsTr("Sent %n file(s) to %1", "", sentFiles).arg(name)
        case "declined": return qsTr("%1 declined.").arg(name)
        case "busy": return qsTr("%1 is busy with another transfer.").arg(name)
        case "pin": return qsTr("%1 asks for a PIN: send with LocalSend (L).").arg(name)
        case "cancelled": return qsTr("Stopped.")
        case "failed": return qsTr("Could not send: %1").arg(failure)
        }
        if (preparing)
            return chosen.some(item => !item.video) && plugin.compatible ? qsTr("Fetching and converting…") : qsTr("Fetching the originals…")
        if (missing > 0)
            return qsTr("%n file(s) could not be fetched.", "", missing)
        return ""
    }

    // Behind the card: the window dimmed, a tap there closes.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.45)
        MouseArea {
            anchors.fill: parent
            onClicked: panel.close()
        }
    }

    Rectangle {
        id: card
        readonly property int pad: Math.round(theme.fontSize * 1.4)
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * pad, Math.round(theme.fontSize * 30))
        height: column.implicitHeight + 2 * pad
        radius: Math.min(8, theme.radius)
        color: theme.background
        border.width: 1
        border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.25)
        // Taps on the card stay on it.
        MouseArea { anchors.fill: parent }

        Column {
            id: column
            x: card.pad
            y: card.pad
            width: card.width - 2 * card.pad
            spacing: Math.round(theme.fontSize * 0.8)

            Item {
                width: parent.width
                height: closeButton.height
                UiText {
                    y: centredIn(parent.height)
                    width: parent.width - closeButton.width - theme.fontSize
                    elide: Text.ElideRight
                    font.pixelSize: Math.round(theme.fontSize * 1.15)
                    color: theme.brightForeground
                    text: panel.what
                }
                ShellButton {
                    id: closeButton
                    anchors.right: parent.right
                    text: "✕"
                    action: closeCommand
                    onTapped: if (!closeCommand.when) panel.close()
                }
            }

            UiText {
                visible: panel.also !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.darkForeground
                text: panel.also
            }

            // The devices, or what to do for one to show.
            Repeater {
                model: panel.devices
                delegate: ShellButton {
                    required property var modelData
                    required property int index
                    objectName: "send.device" + (index + 1)
                    width: column.width
                    justified: true
                    text: modelData.alias + (modelData.model ? "  ·  " + modelData.model : "")
                    key: index < 9 ? String(index + 1) : ""
                    keyLayer: "plugin"
                    helpText: qsTr("Send to a device")
                    usable: !panel.running
                    active: panel.target !== null && panel.target.fingerprint === modelData.fingerprint && panel.phase !== ""
                    onTapped: panel.sendTo(modelData)
                }
            }
            Row {
                visible: panel.devices.length === 0
                spacing: Math.round(theme.fontSize * 0.6)
                width: parent.width
                Spinner {
                    busy: panel.looking !== null
                    delay: 0
                    bare: true
                }
                UiText {
                    width: parent.width - x
                    y: centredIn(theme.fontSize * 2.333)
                    wrapMode: Text.WordWrap
                    color: theme.darkForeground
                    text: panel.looking !== null ? qsTr("Looking for devices with LocalSend open, on this network…")
                                             : qsTr("No device answered. Open LocalSend on the phone, or send with LocalSend (L).")
                }
            }

            // How the send goes.
            Column {
                width: parent.width
                spacing: Math.round(theme.fontSize * 0.5)
                visible: panel.status !== ""
                UiText {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: panel.phase === "failed" || panel.phase === "declined" ? theme.red
                         : panel.phase === "sent" ? theme.accent : theme.foreground
                    text: (panel.phase === "sent" ? "✓ " : "") + panel.status
                }
                Rectangle {
                    visible: panel.phase === "sending" || panel.phase === "sent"
                    width: parent.width
                    height: Math.max(3, Math.round(theme.fontSize * 0.3))
                    radius: height / 2
                    color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.14)
                    Rectangle {
                        height: parent.height
                        radius: parent.radius
                        color: theme.accent
                        width: parent.width * (panel.totalBytes > 0 ? Math.min(1, panel.sentBytes / panel.totalBytes) : 0)
                        Behavior on width { NumberAnimation { duration: 100 } }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.15)
            }

            Item {
                width: parent.width
                height: jpegSwitch.height
                visible: panel.chosen.some(item => !item.video)
                UiText {
                    y: centredIn(parent.height)
                    text: qsTr("HEIC as JPEG")
                }
                Toggle {
                    id: jpegSwitch
                    anchors.right: parent.right
                    hint: jpegCommand.keyName
                    checked: plugin.compatible
                    onToggled: jpegCommand.trigger()
                }
            }
            Row {
                spacing: Math.round(theme.fontSize * 0.6)
                ShellButton {
                    text: qsTr("Send with LocalSend")
                    action: localSendCommand
                    usable: localSendCommand.when
                }
                ShellButton {
                    visible: panel.running
                    text: qsTr("Stop")
                    action: stopCommand
                }
            }
        }
    }

    Command {
        id: closeCommand
        name: "send.close"
        keys: ["Escape", "O"]
        keyLayer: "plugin"
        when: panel.shown
        onTriggered: panel.close()
    }
    Command {
        id: jpegCommand
        name: "send.jpeg"
        text: qsTr("HEIC as JPEG, or originals")
        section: "Send to phone"
        keys: ["J"]
        keyLayer: "plugin"
        when: panel.shown && !panel.running
        mouse: qsTr("The switch")
        touchpad: qsTr("The switch")
        onTriggered: {
            plugin.setCompatible(!plugin.compatible)
            panel.phase = ""
            panel.prepare()
        }
    }
    Command {
        id: localSendCommand
        name: "send.localsend"
        text: qsTr("Send with LocalSend's own window")
        section: "Send to phone"
        keys: ["L"]
        keyLayer: "plugin"
        when: panel.shown && !panel.preparing && panel.files.length > 0 && !panel.running
        onTriggered: panel.openLocalSend()
    }
    Command {
        id: stopCommand
        name: "send.stop"
        text: qsTr("Stop sending")
        section: "Send to phone"
        keys: ["X"]
        keyLayer: "plugin"
        when: panel.shown && panel.running
        onTriggered: panel.stopSending()
    }
}
