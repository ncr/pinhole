import QtQuick
import Pinhole.Plugin

// F8: the library's files, by how many are videos and how many know where
// they were taken, asked of its index with SQL, in a ScrollArea
// wider and taller than the window.
PinholePlugin {
    id: plugin
    property bool open: false
    property string report: ""
    property var pending: null
    covering: open
    actions: [PluginAction { place: "grid"; text: qsTr("Places"); key: "F8"; busy: plugin.pending !== null; onTriggered: plugin.ask() }]
    Command { name: "places.close"; keys: ["Escape"]; keyLayer: "plugin"; when: plugin.open; onTriggered: plugin.open = false }

    onOpenChanged: if (!open) cancelQuery()
    function cancelQuery() {
        const job = pending
        pending = null
        if (job) job.cancel()
    }
    function ask() {
        cancelQuery()
        open = true
        report = ""
        const job = pinhole.query("SELECT is_video, has_location, count(*) AS n FROM assets GROUP BY is_video, has_location")
        pending = job
        job.then(rows => {
            if (pending !== job || !open) return
            pending = null
            report = rows.map(r => (r.is_video ? "video" : "photo") + (r.has_location ? ", placed: " : ": ") + r.n).join("\n")
        }).catch(error => {
            if (pending !== job) return
            pending = null
            if (error.code !== "cancelled") pinhole.warn(error)
        })
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
