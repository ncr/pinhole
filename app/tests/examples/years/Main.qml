import QtQuick
import Pinhole.Plugin

// F5: how many photos each year has, on a screen of its own.
PinholePlugin {
    id: plugin
    property bool open: false
    property var years: []
    covering: open
    function count() {
        const counts = {}
        for (const day of pinhole.library.field("day"))
            if (day > 0)
                counts[Math.floor(day / 10000)] = (counts[Math.floor(day / 10000)] ?? 0) + 1
        years = Object.keys(counts).reverse().map(year => ({ year: year, count: counts[year] }))
    }
    actions: [PluginAction { place: "grid"; text: qsTr("Years"); key: "F5"; onTriggered: { plugin.count(); plugin.open = true } }]
    Command { name: "years.close"; keys: ["Escape"]; keyLayer: "plugin"; when: plugin.open; onTriggered: plugin.open = false }

    Panel {
        visible: plugin.open
        ScrollList {
            anchors.fill: parent
            anchors.margins: 32
            model: plugin.years
            delegate: UiText {
                required property var modelData
                height: 40
                text: modelData.year + " · " + qsTr("%n photo(s)", "", modelData.count)
            }
        }
    }
}
