import QtQuick
import Pinhole

// A grid of cells that scrolls as Pinhole's grid does: a GridView, its
// wheel, touchpad and pinch taken by a Scroller (scroller.h). In a
// PinchZoom, its cells grow and shrink under the fingers.
GridView {
    id: view
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    Scroller { flickable: view }
}
