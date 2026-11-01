import QtQuick
import Pinhole.Plugin

// F8: the library's files, by how many are videos and how many know where
// they were taken, asked of its index (docs/sources.md), in a ScrollArea
// wider and taller than the window.
PinholePlugin {
    id: plugin
    property bool open: false
    property string report: ""
    covering: open
    actions: [PluginAction { place: "grid"; text: qsTr("Places"); key: "F8"; onTriggered: plugin.ask() }]
    Command { name: "places.close"; keys: ["Escape"]; keyLayer: "plugin"; when: plugin.open; onTriggered: plugin.open = false }

    function ask() {
        open = true
        pinhole.query("SELECT is_video, has_location, count(*) AS n FROM assets GROUP BY is_video, has_location")
            .then(rows => report = rows.map(r => (r.is_video ? "video" : "photo") + (r.has_location ? ", placed: " : ": ") + r.n).join("\n"))
            .catch(pinhole.warn)
    }

    Panel {
        visible: plugin.open
        ScrollArea {
            anchors.fill: parent
            anchors.margins: 32
            contentWidth: text.implicitWidth
            contentHeight: text.implicitHeight
            UiText { id: text; text: plugin.report; font.pixelSize: 96 }
        }
    }
}
