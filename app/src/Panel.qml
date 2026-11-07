import QtQuick
import Pinhole

// A plugin's screen over the photos: its parent's whole room, in the
// theme's background or a `color` of its own, taking every click, touch,
// wheel turn, swipe, pinch and hover over it (Shield), so the photos beneath
// get none. Its controls go after it. `triggered()` is a click on it where no
// control is (on a dimmed one beside a card, it puts the card away); with
// `steps` (Qt.Horizontal, Qt.Vertical), the wheel and a swipe step through
// something: `stepped(1)` the next, `stepped(-1)` the one before.
Rectangle {
    id: panel
    property alias steps: shield.steps
    signal triggered()
    signal stepped(int step)
    anchors.fill: parent
    color: theme.background
    Shield {
        id: shield
        anchors.fill: parent
        onClicked: panel.triggered()
        onStepped: step => panel.stepped(step)
    }
}
