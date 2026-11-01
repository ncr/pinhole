import QtQuick
import Pinhole.Plugin

// Send to phone: photos to a phone or another computer nearby with
// LocalSend, which Omarchy has, from a photo shown full window or what is
// ticked in select mode (O). The devices with LocalSend open show as they
// answer; picking one fetches the files and asks its owner (with its PIN,
// if it has one), and once they say yes the originals go, a Live Photo's
// video with its photo, or HEIC as JPEG. pinhole-send (sender/) speaks
// LocalSend's protocol with LocalSend's own code, and makes the JPEGs; for
// a device that does not show, LocalSend's own window takes the files.
// Nothing is fetched or sent but on a press of a device.
PinholePlugin {
    id: plugin
    covering: panel.shown

    // Installed beside this file; in a checkout, where cargo builds it.
    readonly property string sender: pinhole.exists(pinhole.dir + "/pinhole-send")
                                     ? "pinhole-send" : pinhole.dir + "/sender/target/release/pinhole-send"

    // HEIC as JPEG rather than the originals: kept.
    readonly property bool compatible: pinhole.settings.compatible === true
    function setCompatible(on) {
        pinhole.settings.compatible = on
    }

    // The devices sent to, the last first: where they were, asked first
    // next time (a phone whose LocalSend has been open a while does not
    // announce itself) and listed in this order.
    function known() {
        const list = pinhole.settings.devices
        return Array.isArray(list) ? list : []
    }
    function remember(device) {
        const list = [device].concat(known().filter(d => d.fingerprint !== device.fingerprint)).slice(0, 6)
        pinhole.settings.devices = list
    }

    // What it keeps, kept small: folders staged for LocalSend's window go
    // after a day, JPEGs after thirty, and the oldest past 2 GB; each
    // source's, as it loads, every hour and when another source is shown.
    // Not while a send may be taking from it.
    function tidy() {
        if (!panel.running)
            pinhole.run(sender, ["tidy", pinhole.cacheDir]).catch(pinhole.warn)
    }
    Component.onCompleted: tidy()
    Connections {
        target: pinhole
        function onCacheDirChanged() { plugin.tidy() }
    }
    Timer {
        interval: 60 * 60 * 1000
        running: true
        repeat: true
        onTriggered: plugin.tidy()
    }

    readonly property string label: panel.running ? qsTr("Sending") : qsTr("Send")

    actions: [
        PluginAction {
            id: viewerAction
            place: "viewer"
            text: plugin.label
            key: "O"
            busy: panel.running
            onTriggered: panel.open([pinhole.viewer.index])
        },
        PluginAction {
            id: selectionAction
            place: "selection"
            text: plugin.label
            key: "O"
            busy: panel.running
            usable: pinhole.selectionCount > 0 || panel.running
            onTriggered: panel.open(Array.from(pinhole.selection()))
        }
    ]

    help: [
        ["O", qsTr("Send to a phone nearby")],
        ["1–9", qsTr("Send to that device")],
        ["J", qsTr("HEIC as JPEG, or originals")],
        ["L", qsTr("In LocalSend's own window")],
        ["X", qsTr("Stop sending")]
    ]

    tour: [
        {
            title: qsTr("To your phone"),
            text: qsTr("O sends the photo shown, or what is ticked in select mode, to a phone or computer nearby with LocalSend open on it. A Live Photo goes with its video."),
            open: () => pinhole.currentIndex(),
            action: viewerAction
        },
        {
            title: qsTr("Pick a device"),
            text: qsTr("Devices with LocalSend open show as they answer, each on its number. Its owner accepts, and the originals go, or HEIC as JPEG with J. L hands them to LocalSend's own window instead.")
        },
        {
            title: qsTr("Settings"),
            text: qsTr("Here, in the side panel (Tab), this tour can be shown again."),
            settings: true
        }
    ]

    settings: Component {
        UiText {
            width: parent ? parent.width : 0
            wrapMode: Text.WordWrap
            color: theme.foreground
            font.pixelSize: Math.round(theme.fontSize * 0.857)
            text: qsTr("LocalSend has to be open on the phone, on the same network. Sends %1.")
                  .arg(plugin.compatible ? qsTr("HEIC as JPEG") : qsTr("originals"))
        }
    }

    SendPanel {
        id: panel
        objectName: "sendPanel"
        anchors.fill: parent
        plugin: plugin
    }
}
