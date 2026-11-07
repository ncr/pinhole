import QtQuick
import Pinhole.Plugin

// F6: the newest photos' colours as tiles, grown and shrunk by a pinch, Ctrl
// and the wheel, + and -, through three sizes, as the grid zooms.
PinholePlugin {
    id: plugin
    property bool open: false
    covering: open
    actions: [PluginAction { place: "grid"; text: qsTr("Tiles"); key: "F6"; onTriggered: plugin.open = true }]
    Command { name: "tiles.close"; keys: ["Escape"]; keyLayer: "plugin"; usable: plugin.open; onTriggered: plugin.open = false }
    Command { name: "tiles.in"; keys: ["+", "="]; keyLayer: "plugin"; usable: plugin.open; onTriggered: zoom.zoomBy(1) }
    Command { name: "tiles.out"; keys: ["-"]; keyLayer: "plugin"; usable: plugin.open; onTriggered: zoom.zoomBy(-1) }

    Panel {
        visible: plugin.open
        PinchZoom {
            id: zoom
            anchors.fill: parent
            levels: [24, 48, 96]
            level: 1
            Flow {
                anchors.fill: parent
                Repeater {
                    model: plugin.open ? Math.min(pinhole.library.count, Math.floor(parent.width / zoom.size) * Math.floor(parent.height / zoom.size)) : 0
                    delegate: Rectangle {
                        required property int index
                        width: zoom.size
                        height: zoom.size
                        color: pinhole.library.item(pinhole.library.count - 1 - index).avgColor || "transparent"
                    }
                }
            }
        }
    }
}
