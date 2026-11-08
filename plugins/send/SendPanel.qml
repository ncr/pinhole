import QtQuick
import Pinhole.Plugin

// The photos to send and where to: the LocalSend devices nearby as they
// answer, each on its number. Picking one fetches the files, asks its
// owner, and the files go once they say yes, the bar filling as they do.
// A send goes on with the panel closed; the button turns while it does,
// and says how it ended at the foot of the window. Shown in a PluginView
// (Main.qml), made with the plugin.
Item {
    id: panel
    objectName: "sendPanel"
    // Its PluginView, and the plugin (Main.qml).
    property var view
    readonly property var plugin: view ? view.plugin : null
    readonly property bool shown: view ? view.shown : false

    // The photos, as they were when it opened: {id, filename, video, live}.
    property var chosen: []
    // Their files once fetched, in order: {path, name}; a Live Photo's video
    // after its photo. Fetched for a send, kept for the next (a PIN asked
    // for, another device), as JPEGs or not, if none was missing.
    property var files: []
    property bool filesCompatible: false
    // Fetching: how many of how many, and what is under way, to stop; how
    // many could not be fetched, and how many HEICs not made JPEGs.
    property int fetched: 0
    property int fetching: 0
    property int missing: 0
    property int unconverted: 0
    property var jobs: []
    property int generation: 0

    // The devices that answered, by fingerprint, in the order shown.
    property var devices: []
    // The run listening for them, while it does.
    property var looking: null

    // The send: "" none; "fetching", "asking", "sending" under way; then
    // "sent", or what stopped it, as pinhole-send says (outcome()).
    property string phase: ""
    property var target: null
    property real sentBytes: 0
    property real totalBytes: 0
    property int sentFiles: 0
    property string detail: ""
    property var sending: null
    readonly property bool running: phase === "fetching" || phase === "asking" || phase === "sending"
    readonly property bool askingPin: phase === "pin" || phase === "wrongpin"

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
            if (JSON.stringify(list) !== JSON.stringify(chosen)) {
                chosen = list
                files = []
            }
            // A PIN asked for while it was closed is still asked for, the
            // photos it then sends those it shows.
            if (!askingPin)
                phase = ""
        }
        view.open()
        look()
    }
    function close() { view.close() }
    // The PIN's field has the keys only while it shows and asks: left with
    // them, hidden, Pinhole's keys would type into it (each a shortcut a
    // field takes first).
    onShownChanged: {
        pinFocus()
        if (!shown)
            stopLooking()
    }
    onAskingPinChanged: pinFocus()
    function pinFocus() {
        if (shown && askingPin)
            Qt.callLater(() => {
                if (shown && askingPin)
                    pinField.forceActiveFocus()
            })
        else
            pinField.focus = false
        if (!askingPin)
            pinField.text = ""
    }

    // The files, then done(files): originals as taken; HEIC as JPEG, the
    // photo as it looks now (an edit, if it has one), as Export does.
    // Videos as they are. Those fetched before, if all of them were and
    // are still there; else all of them again.
    function prepare(done) {
        const compatible = pinhole.settings.compatible
        if (files.length > 0 && filesCompatible === compatible && missing === 0 && unconverted === 0
                && files.every(file => pinhole.exists(file.path)))
            return done(files)
        files = []
        const job = ++generation
        const wanted = []
        for (const item of chosen) {
            wanted.push({ item: item, kind: item.video || !compatible ? "original" : "full" })
            if (item.live && !item.video)
                wanted.push({ item: item, kind: "live" })
        }
        const got = new Array(wanted.length)
        let left = wanted.length
        fetched = 0
        fetching = wanted.length
        missing = 0
        unconverted = 0
        const finish = () => {
            if (job !== generation)
                return
            jobs = []
            const list = []
            got.forEach((path, k) => {
                if (path)
                    list.push({ path: path, name: nameOf(wanted[k].item.filename, path) })
            })
            files = list
            filesCompatible = compatible
            done(list)
        }
        // HEIC as JPEG: the service's (`jpeg`), made once into its cache
        // from the same full, a few at once there. Each not made is left
        // out, and said so.
        const convert = () => {
            const heic = []
            got.forEach((path, k) => {
                if (path && compatible && wanted[k].kind !== "live" && /\.hei[cf]$/i.test(path))
                    heic.push(k)
            })
            if (heic.length === 0)
                return finish()
            let waiting = heic.length
            jobs = heic.map(k => {
                const made = pinhole.fetch(wanted[k].item.id, "jpeg")
                made.then(path => {
                    if (job === generation)
                        got[k] = path
                }, error => {
                    if (job !== generation || error.code === "cancelled")
                        return
                    pinhole.warn(error)
                    got[k] = null
                    ++unconverted
                }).catch(pinhole.warn).then(() => {
                    if (job === generation && --waiting === 0)
                        finish()
                })
                return made
            })
        }
        const fetches = wanted.map((w, k) => {
            const fetch = pinhole.fetch(w.item.id, w.kind)
            fetch.then(path => {
                if (job !== generation)
                    return
                got[k] = path
                ++fetched
            }, error => {
                if (job !== generation || error.code === "cancelled")
                    return
                pinhole.warn(error)
                ++missing
            }).catch(pinhole.warn).then(() => {
                if (job === generation && --left === 0)
                    convert()
            })
            return fetch
        })
        jobs = fetches
        if (wanted.length === 0)
            finish()
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
        if (!looking)
            return
        // Gone at once: opened again straight away, it looks anew.
        const run = looking
        looking = null
        run.cancel()
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
            // Never a plain HTTP claim over one proven by its certificate.
            if (device.protocol === "http" && list[at].protocol !== "http")
                return
            list[at] = device
        } else {
            // After the rest: a device's number never changes under the
            // finger. Those sent to before are asked first, and so tend to
            // be first.
            list.push(device)
        }
        devices = list
    }

    function sendTo(device, pin) {
        if (running || !device)
            return
        target = device
        sentBytes = 0
        totalBytes = 0
        sentFiles = 0
        detail = ""
        plugin.remember(device)
        phase = "fetching"
        prepare(list => deliver(list, pin || ""))
    }
    function deliver(list, pin) {
        if (phase !== "fetching")
            return
        if (list.length === 0)
            return outcome(missing === 0 && unconverted > 0 ? "unconverted" : "unfetched")
        phase = "asking"
        // What it says is heard as it goes; ended any way but by saying so,
        // it failed. The PIN with the files, on its stdin: an argument any
        // process can read.
        const run = pinhole.run(plugin.sender, ["send", JSON.stringify(target)],
                                { json: { files: list, pin: pin }, output: line => heard(line) })
        sending = run
        run.then(() => {}, error => {
            // Stopped before it could say so (the host kills a second
            // after asking), or ended any other way.
            if (running)
                outcome(error.code === "cancelled" ? "cancelled" : "failed")
        }).catch(pinhole.warn).then(() => {
            if (sending === run)
                sending = null
        })
    }
    function heard(line) {
        const words = line.split(" ")
        switch (words[0]) {
        case "asking":
            break
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
            outcome("sent")
            break
        default:
            detail = words.slice(1).join(" ")
            outcome(words[0])
        }
    }
    // How it ended: in the panel, or at the foot of the window if it is
    // closed by then.
    function outcome(how) {
        phase = how
        jobs = []
        if (shown) {
            if (how === "sent")
                closing.restart()
        } else if (how !== "cancelled") {
            pinhole.notify(status)
        }
    }
    function stopSending() {
        if (phase === "fetching") {
            ++generation
            jobs.forEach(job => job.cancel())
            outcome("cancelled")
        } else if (sending) {
            sending.cancel()
        }
    }
    // A moment to read that they went.
    Timer { id: closing; interval: 1600; onTriggered: if (panel.phase === "sent") panel.close() }

    // LocalSend's own window, with the files: for a device that does not
    // show here. Started apart from Pinhole (uwsm-app, as Omarchy starts
    // apps), so it outlives it; one already running takes them over. Not
    // installed, it says so first: started apart, nothing would.
    function openLocalSend() {
        if (running)
            return
        // LocalSend there to hand them to.
        if (!pinhole.has("localsend"))
            return outcome("nolocalsend")
        phase = "fetching"
        target = null
        prepare(list => {
            if (phase !== "fetching")
                return
            phase = ""
            if (list.length === 0)
                return outcome(missing === 0 && unconverted > 0 ? "unconverted" : "unfetched")
            const folder = pinhole.cacheDir + "/localsend/" + Date.now()
            pinhole.run(plugin.sender, ["stage", folder], { json: list }).then(result => {
                const paths = result.stdout.split("\n").filter(l => l !== "")
                // The folder for very many, which LocalSend takes as a folder.
                const given = paths.length > 500 ? [folder] : paths
                return pinhole.run("sh", ["-c", 'if command -v uwsm-app >/dev/null; then set -- uwsm-app -- localsend "$@"; else set -- localsend "$@"; fi; setsid -f "$@" >/dev/null 2>&1 </dev/null',
                                          "sh"].concat(given))
            }).then(() => panel.close(), error => {
                pinhole.warn(error)
                outcome("failed")
            }).catch(pinhole.warn)
        })
    }

    readonly property string what: chosen.every(item => item.video) ? qsTr("Send %n video(s)", "", chosen.length)
                                                                   : qsTr("Send %n photo(s)", "", chosen.length)
    readonly property string also: videos > 0 ? qsTr("With the video of %n Live Photo(s)", "", videos) : ""
    readonly property string status: {
        const name = target ? target.alias : ""
        switch (phase) {
        case "fetching":
            return fetching <= 1 ? qsTr("Fetching…") : qsTr("Fetching %1 of %2…").arg(fetched).arg(fetching)
        case "asking": return qsTr("Waiting for %1 to accept…").arg(name)
        case "sending": return qsTr("Sending to %1: %2 of %3").arg(name).arg(pinhole.sizeText(sentBytes)).arg(pinhole.sizeText(totalBytes))
        case "sent": {
            const parts = [qsTr("Sent %n file(s) to %1", "", sentFiles).arg(name)]
            if (missing > 0)
                parts.push(qsTr("%n could not be fetched", "", missing))
            if (unconverted > 0)
                parts.push(qsTr("%n could not be made JPEG", "", unconverted))
            return parts.length > 1 ? parts.join("; ") + "." : parts[0]
        }
        case "declined": return qsTr("%1 declined.").arg(name)
        case "busy": return qsTr("%1 is busy with another transfer.").arg(name)
        case "pin": return qsTr("%1 asks for a PIN.").arg(name)
        case "wrongpin": return qsTr("Not the PIN %1 asks for.").arg(name)
        case "blocked": return qsTr("%1 won't take files from this computer after three wrong PINs: close and reopen LocalSend on it.").arg(name)
        case "nothing": return qsTr("%1 took none of them.").arg(name)
        case "stalled": return qsTr("%1 stopped taking them.").arg(name)
        case "unreachable": return qsTr("%1 could not be reached. Is LocalSend still open on it?").arg(name)
        case "mismatch": return qsTr("Something else answered where %1 was: nothing sent.").arg(name)
        case "rejected": return qsTr("%1 refused them (%2).").arg(name).arg(detail)
        case "unfetched": return qsTr("The photos could not be fetched.")
        case "unconverted": return qsTr("The photos could not be made JPEG.")
        case "nolocalsend": return qsTr("LocalSend is not installed.")
        case "cancelled": return qsTr("Stopped.")
        case "failed": return target ? qsTr("Could not send to %1.").arg(name) : qsTr("Could not send.")
        }
        return ""
    }
    // A PIN being typed: its keys are its own.
    readonly property bool typing: askingPin && pinField.activeFocus
    readonly property bool bad: phase !== "" && !running && phase !== "sent" && phase !== "cancelled" && !askingPin

    // Behind the card: the window dimmed, a tap there closes; nothing
    // reaches the photos beneath.
    Panel {
        color: Qt.rgba(0, 0, 0, 0.45)
        onTriggered: panel.close()
    }

    Card {
        id: card
        readonly property int pad: Math.round(theme.fontSize * 1.4)
        // In the middle, or from the top when the window is short.
        x: Math.round((parent.width - width) / 2)
        y: Math.max(pad, Math.round((parent.height - height) / 2))
        width: Math.min(parent.width - 2 * pad, Math.round(theme.fontSize * 30))
        height: column.implicitHeight + 2 * pad

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
                    action: panel.view ? panel.view.closeCommand : null
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
                    readonly property bool chosen: panel.target !== null && panel.target.fingerprint === modelData.fingerprint
                                                   && panel.phase !== ""
                    // Plain HTTP: nothing proves it is the device it says.
                    text: modelData.alias + (modelData.model ? "  ·  " + modelData.model : "")
                          + (modelData.protocol === "http" ? "  ·  " + qsTr("unencrypted") : "")
                    key: index < 9 && !panel.typing ? String(index + 1) : ""
                    keyLayer: "plugin"
                    helpText: qsTr("Send to a device")
                    // The one sent to stays readable while it goes.
                    usable: !panel.running || chosen
                    active: chosen
                    onTriggered: panel.sendTo(modelData)
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
                    color: panel.bad ? theme.red : panel.phase === "sent" ? theme.accent : theme.foreground
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

            // The PIN the device asks for, sent with Enter.
            Row {
                visible: panel.askingPin
                spacing: Math.round(theme.fontSize * 0.6)
                Rectangle {
                    width: Math.round(theme.fontSize * 9)
                    height: Math.round(theme.fontSize * 2.333)
                    radius: Math.min(4, theme.radius)
                    color: "transparent"
                    border.width: 1
                    border.color: pinField.activeFocus ? theme.accent
                                : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)
                    TextInput {
                        id: pinField
                        objectName: "send.pin"
                        enabled: panel.shown && panel.askingPin
                        anchors.fill: parent
                        anchors.leftMargin: Math.round(theme.fontSize * 0.6)
                        anchors.rightMargin: anchors.leftMargin
                        verticalAlignment: TextInput.AlignVCenter
                        font.family: theme.font
                        font.pixelSize: theme.fontSize
                        color: theme.foreground
                        selectionColor: theme.accent
                        clip: true
                        onAccepted: pinSend.press()
                        Keys.onEscapePressed: panel.close()
                    }
                    UiText {
                        visible: pinField.text === ""
                        x: pinField.x
                        y: centredIn(parent.height)
                        color: theme.darkForeground
                        text: qsTr("PIN")
                    }
                }
                ShellButton {
                    id: pinSend
                    text: qsTr("Send")
                    hint: "⏎"
                    usable: pinField.text !== ""
                    onTriggered: {
                        const pin = pinField.text
                        pinField.text = ""
                        panel.sendTo(panel.target, pin)
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
                    Accessible.name: qsTr("HEIC as JPEG")
                    anchors.right: parent.right
                    hint: jpegCommand.keyName
                    checked: pinhole.settings.compatible
                    // Not while a PIN is typed or a send goes.
                    usable: jpegCommand.usable
                    onToggled: jpegCommand.trigger()
                }
            }
            Row {
                spacing: Math.round(theme.fontSize * 0.6)
                ShellButton {
                    text: qsTr("Send with LocalSend")
                    action: localSendCommand
                    usable: localSendCommand.usable
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
        id: jpegCommand
        name: "send.jpeg"
        text: qsTr("HEIC as JPEG, or originals")
        section: "Send to phone"
        keys: ["J"]
        keyLayer: "plugin"
        usable: panel.shown && !panel.running && !panel.typing
        mouse: qsTr("The switch")
        touchpad: qsTr("The switch")
        onTriggered: {
            plugin.setCompatible(!pinhole.settings.compatible)
            panel.phase = ""
        }
    }
    Command {
        id: localSendCommand
        name: "send.localsend"
        text: qsTr("In LocalSend's own window")
        section: "Send to phone"
        keys: ["L"]
        keyLayer: "plugin"
        usable: panel.shown && panel.chosen.length > 0 && !panel.running && !panel.typing
        onTriggered: panel.openLocalSend()
    }
    Command {
        id: stopCommand
        name: "send.stop"
        text: qsTr("Stop sending")
        section: "Send to phone"
        keys: ["X"]
        keyLayer: "plugin"
        usable: panel.shown && panel.running
        onTriggered: panel.stopSending()
    }
}
