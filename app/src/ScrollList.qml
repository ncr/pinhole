import QtQuick
import Pinhole

// A list that scrolls as the grid does: a ListView, its wheel, touchpad and
// pinch taken by a Scroller (scroller.h), so they move it with the grid's
// own inertia, glide and springs, and none reaches what is beneath. Across,
// as a strip, with `orientation: ListView.Horizontal`. Inside another (a
// list in a panel), the inner one moves first, to its end.
ListView {
    id: view
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    Scroller { flickable: view }
}
