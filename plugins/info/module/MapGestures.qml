import QtQuick

// Unlike the grid, the map has a continuous camera. Use the gesture deltas
// in viewport pixels and zoom around the pointer or touchpad centroid.
Item {
    id: gestures
    default property alias markers: markerLayer.data
    signal pan(real dx, real dy)
    signal backgroundTapped()
    signal dragStarted()
    signal zoomAt(real x, real y, real amount)

    Item {
        id: markerLayer
        anchors.fill: parent
    }

    HoverHandler { cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor }

    TapHandler {
        onTapped: point => {
            const at = point.position
            if (!markerLayer.childAt(at.x, at.y))
                gestures.backgroundTapped()
        }
        onDoubleTapped: point => {
            const at = point.position
            if (!markerLayer.childAt(at.x, at.y))
                gestures.zoomAt(at.x, at.y, 1)
        }
    }

    DragHandler {
        id: drag
        target: null
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        maximumPointCount: 1
        onActiveChanged: if (active) gestures.dragStarted()
        onTranslationChanged: delta => gestures.pan(delta.x, delta.y)
    }
    PinchHandler {
        target: null
        minimumScale: 0.0001
        maximumScale: 10000
        onScaleChanged: delta => {
            if (delta > 0)
                gestures.zoomAt(centroid.position.x, centroid.position.y, Math.log2(delta))
        }
        onTranslationChanged: delta => gestures.pan(delta.x, delta.y)
    }
}
