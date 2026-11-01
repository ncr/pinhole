import QtQuick
import Pinhole

// What it holds, zoomed by a pinch, Ctrl and the wheel, and the sideways
// wheel through a few sizes, as the grid zooms through its columns
// (zoomer.h): laid out at `size`, one of `levels`, and drawn larger or
// smaller under the fingers until it lands on one. `zoomBy(1)` and
// `zoomBy(-1)` step, eased, as + and - do on the grid. A ScrollList,
// ScrollGrid or ScrollArea in it passes its pinches on to it.
Zoomer {
    id: zoom
    default property alias content: holder.data
    Item {
        id: holder
        width: zoom.width
        height: zoom.height
        transform: Scale {
            origin.x: zoom.origin.x
            origin.y: zoom.origin.y
            xScale: zoom.factor
            yScale: zoom.factor
        }
    }
}
