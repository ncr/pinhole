import QtQuick
import QtQuick.Controls
import Pinhole.Plugin

// PINHOLE_SELFTEST=through (app/src/selftestthrough.cpp): settings with
// scrollables of their own inside the side panel, which scrolls too: a list
// of forty rows, and a long text in a field of a few lines, each with a
// Scroller over it. The wheel and the touchpad move each while it can
// still go their way, then the panel.
PinholePlugin {
    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: 8
            Item {
                width: parent.width
                height: 160
                ListView {
                    id: list
                    objectName: "testNestedList"
                    anchors.fill: parent
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: 40
                    delegate: UiText {
                        required property int index
                        width: ListView.view.width
                        height: 24
                        text: "Row " + (index + 1)
                    }
                }
                Scroller { anchors.fill: parent; flickable: list }
            }
            // A TextArea scrolled as Qt has it done: in a Flickable of its own.
            Item {
                width: parent.width
                height: 100
                Flickable {
                    id: textView
                    objectName: "testNestedTextView"
                    anchors.fill: parent
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    TextArea.flickable: TextArea {
                        objectName: "testNestedText"
                        wrapMode: TextEdit.Wrap
                        text: Array.from({ length: 30 }, (_, i) => "Line " + (i + 1) + " of a long text").join("\n")
                    }
                }
                Scroller { anchors.fill: parent; flickable: textView }
            }
        }
    }
}
