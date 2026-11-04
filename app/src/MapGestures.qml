import QtQuick

// Unlike the grid, the map has a continuous camera. Use the gesture deltas
// in viewport pixels and zoom around the pointer or touchpad centroid.
Item {
    id: gestures
    signal pan(real dx, real dy)
    signal zoomAt(real x, real y, real amount)

    DragHandler {
        target: null
        acceptedButtons: Qt.LeftButton
        maximumPointCount: 1
        onTranslationChanged: delta => gestures.pan(delta.x, delta.y)
    }
    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            if (!(event.modifiers & Qt.ControlModifier)
                    && (event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0)) {
                gestures.pan(event.pixelDelta.x, event.pixelDelta.y)
            } else {
                const dy = event.angleDelta.y || event.pixelDelta.y
                if (dy)
                    gestures.zoomAt(event.x, event.y, dy / 120 * 0.65)
            }
            event.accepted = true
        }
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
