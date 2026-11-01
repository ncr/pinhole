import QtQuick
import QtQuick.Controls
import Pinhole.Plugin

// PINHOLE_SELFTEST=through (app/src/selftestthrough.cpp): settings with
// scrollables of their own inside the side panel, which scrolls too: a list
// of forty rows, and a long text in a field of a few lines, a ScrollList
// and a ScrollArea. The wheel and the touchpad move each while it can
// still go their way, then the panel.
PinholePlugin {
    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: 8
            ScrollList {
                objectName: "testNestedList"
                width: parent.width
                height: 160
                model: 40
                delegate: UiText {
                    required property int index
                    width: ListView.view.width
                    height: 24
                    text: qsTr("Row %1").arg(index + 1)
                }
            }
            // A TextArea scrolled as Qt has it done: in a Flickable of its
            // own, here the ScrollArea.
            ScrollArea {
                objectName: "testNestedTextView"
                width: parent.width
                height: 100
                TextArea.flickable: TextArea {
                    objectName: "testNestedText"
                    wrapMode: TextEdit.Wrap
                    text: Array.from({ length: 30 }, (_, i) => qsTr("Line %1 of a long text").arg(i + 1)).join("\n")
                }
            }
            // Room below the two, so that with either at its end the panel
            // can still go on down, whatever comes after these settings.
            Item {
                width: parent.width
                height: 1200
            }
        }
    }
}
