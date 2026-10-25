import QtQuick
import QtQuick.Shapes

// Something on its way, shown only once it is taking a while (photos
// mostly come quicker than a second): a small arc turning, as Omarchy's
// shell turns an icon while busy, on a chip's square so it reads over any
// photo.
Rectangle {
    id: spinner
    // Whether it is waiting; the spinner shows after `delay` of it.
    property bool busy: false
    property int delay: 1000

    readonly property bool shown: busy && !waiting.running
    width: height
    height: Math.round(theme.fontSize * 2.333)
    radius: Math.min(4, theme.radius)
    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.92)
    border.width: 1
    border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 150 } }

    Timer { id: waiting; interval: spinner.delay; running: spinner.busy }

    Shape {
        id: arc
        readonly property real size: Math.round(theme.fontSize * 0.9)
        anchors.centerIn: parent
        width: size
        height: size
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: theme.foreground
            strokeWidth: Math.max(1.5, theme.fontSize / 8)
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: arc.size / 2
                centerY: arc.size / 2
                radiusX: arc.size / 2 - 1
                radiusY: arc.size / 2 - 1
                startAngle: 0
                sweepAngle: 270
            }
        }
        RotationAnimation on rotation {
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
            running: spinner.visible
        }
    }
}
