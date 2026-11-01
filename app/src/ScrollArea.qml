import QtQuick
import Pinhole

// Anything larger than its room, scrolled as the grid scrolls: a Flickable
// (give it a contentWidth and a contentHeight), its wheel, touchpad and
// pinch taken by a Scroller (scroller.h); up and down, and across where the
// content is wider than it. A long text: `TextArea.flickable: TextArea {}`
// in it.
Flickable {
    id: view
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    Scroller { flickable: view }
}
