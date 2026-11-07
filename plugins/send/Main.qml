import QtQuick
import Pinhole.Plugin

// Send to phone: photos to a phone or another computer nearby with
// LocalSend, which Omarchy has, from a photo shown full window or what is
// ticked in select mode (O). The devices with LocalSend open show as they
// answer; picking one fetches the files and asks its owner (with its PIN,
// if it has one), and once they say yes the originals go, a Live Photo's
// video with its photo, or HEIC as JPEG (the service's, `jpeg`, which
// Export makes too). pinhole-send (sender/) speaks LocalSend's protocol
// with LocalSend's own code; for a device that does not show, LocalSend's
// own window takes the files.
// Nothing is fetched or sent but on a press of a device.
PinholePlugin {
    id: plugin

    // Installed beside this file; in a checkout, where cargo builds it.
    readonly property string sender: pinhole.exists(pinhole.dir + "/pinhole-send")
                                     ? "pinhole-send" : pinhole.dir + "/sender/target/release/pinhole-send"

    // HEIC as JPEG rather than the originals (settings.compatible): kept.
    function setCompatible(on) {
        pinhole.settings.compatible = on
    }

    // The devices sent to, the last first: where they were, asked first
    // next time (a phone whose LocalSend has been open a while does not
    // announce itself) and listed in this order.
    function known() {
        const list = pinhole.state.devices
        return Array.isArray(list) ? list : []
    }
    function remember(device) {
        const list = [device].concat(known().filter(d => d.fingerprint !== device.fingerprint)).slice(0, 6)
        pinhole.state.devices = list
    }

    // What it keeps, kept small: folders staged for LocalSend's window go
    // after a day (and the JPEGs it made itself before the service made
    // them, at once); each source's, as it loads, every hour and when
    // another source is shown. Not while a send may be taking from it.
    function tidy() {
        if (!running)
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

    // The panel (SendPanel.qml), made with the plugin; a send goes on with
    // it closed.
    readonly property var panel: sendView.item
    readonly property bool running: panel !== null && panel.running
    readonly property string label: running ? qsTr("Sending") : qsTr("Send")

    actions: [
        // The photo shown, or those ticked (API 15): one action in both.
        PluginAction {
            id: sendAction
            places: ["viewer", "selection"]
            text: plugin.label
            key: "O"
            busy: plugin.running
            onTriggered: plugin.panel.open(photos)
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
            action: sendAction
        },
        {
            title: qsTr("Pick a device"),
            text: qsTr("Devices with LocalSend open show as they answer, each on its number. Its owner accepts, and the originals go, or HEIC as JPEG with J. L hands them to LocalSend's own window instead.")
        }
    ]

    settings: Component {
        SettingNote {
            width: parent ? parent.width : 0
            text: qsTr("LocalSend has to be open on the phone, on the same network. Sends %1.")
                  .arg(pinhole.settings.compatible ? qsTr("HEIC as JPEG") : qsTr("originals"))
        }
    }

    PluginView { id: sendView; source: "SendPanel.qml"; toggleKey: "O"; lazy: false }
}
