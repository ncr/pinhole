import QtQuick
import QtQuick.Effects
import QtQuick.Shapes
import "Engine.js" as Engine

// A series as its clip, at one moment (`time`): the preview and the file
// are both this, so what is made is what was seen. As big as the item is.
Item {
    id: root
    // [{thumbnail, sharp, aspect, shift: {x, y}}]: each shot's thumbnail at
    // once, its sharper picture over it once here.
    property var shots: []
    property var recipe: Engine.DEFAULT_RECIPE
    property real time: 0
    // How big the pictures are decoded: the preview's needs, or a file's.
    property int detail: 1600

    readonly property var timeline: Engine.timeline(recipe, shots.length)
    readonly property var moment: timeline.at(time)
    readonly property var geometry: Engine.geometry(shots, recipe, width, height)
    // Every picture asked for is here (or will not come).
    readonly property bool ready: pending === 0
    property int pending: 0
    clip: true

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    Item {
        id: picture
        anchors.fill: parent
        readonly property var look: Engine.EFFECTS[root.recipe.look]
        layer.enabled: look !== null
        layer.effect: MultiEffect {
            saturation: picture.look ? picture.look.saturation : 0
            contrast: picture.look ? picture.look.contrast : 0
            brightness: picture.look ? picture.look.brightness : 0
            colorization: picture.look ? picture.look.colorization : 0
            colorizationColor: picture.look ? picture.look.color : "black"
        }

        // Under a pile: the series itself, blurred and darkened, so the
        // prints land in its colours.
        Item {
            anchors.fill: parent
            visible: root.recipe.motion === "pile" && root.shots.length > 0
            layer.enabled: visible
            layer.effect: MultiEffect {
                blurEnabled: true
                blur: 1
                blurMax: 48
                brightness: -0.32
                saturation: 0.15
            }
            Image {
                readonly property var shot: root.shots[Math.floor(root.shots.length / 2)]
                readonly property var size: shot ? Engine.cover(shot.aspect, root.width, root.height) : { w: 0, h: 0 }
                anchors.centerIn: parent
                width: size.w * 1.25
                height: size.h * 1.25
                source: shot ? shot.thumbnail : ""
                asynchronous: true
                smooth: true
            }
        }

        Repeater {
            model: root.shots.length
            delegate: Item {
                id: shot
                required property int index
                readonly property var shotData: root.shots[index]
                readonly property var at: Engine.place(index, root.moment, root.recipe, root.geometry, root.shots, root.width, root.height)
                visible: at !== null
                z: at ? at.z : 0
                opacity: at ? at.opacity : 0
                x: at ? at.x - width / 2 : 0
                y: at ? at.y - height / 2 : 0
                width: at ? at.w + 2 * at.border : 0
                height: at ? at.h + 2 * at.border : 0
                scale: at ? at.scale : 1
                rotation: at ? at.rotation : 0
                antialiasing: rotation !== 0

                // A print's paper, and its shadow on the pile.
                Rectangle {
                    visible: shot.at !== null && shot.at.print
                    anchors.fill: parent
                    anchors.topMargin: 3
                    anchors.leftMargin: 1
                    anchors.rightMargin: -1
                    anchors.bottomMargin: -5
                    color: Qt.rgba(0, 0, 0, 0.35)
                    antialiasing: true
                }
                Rectangle {
                    visible: shot.at !== null && shot.at.print
                    anchors.fill: parent
                    color: "#f7f5f0"
                    antialiasing: true
                }
                Image {
                    id: thumb
                    anchors.fill: parent
                    anchors.margins: shot.at ? shot.at.border : 0
                    source: shot.shotData ? shot.shotData.thumbnail : ""
                    asynchronous: true
                    smooth: true
                    mipmap: true
                    visible: sharp.status !== Image.Ready
                }
                Image {
                    id: sharp
                    anchors.fill: thumb
                    source: shot.shotData && shot.shotData.sharp ? shot.shotData.sharp : ""
                    sourceSize.width: root.detail
                    sourceSize.height: root.detail
                    autoTransform: true
                    asynchronous: true
                    smooth: true
                    mipmap: true
                    // Counted while on their way, for `ready`.
                    property bool waiting: false
                    function count() {
                        const now = source.toString() !== "" && status === Image.Loading
                        if (now !== waiting) {
                            root.pending += now ? 1 : -1
                            waiting = now
                        }
                    }
                    onStatusChanged: count()
                    onSourceChanged: count()
                    Component.onDestruction: if (waiting) root.pending -= 1
                }
            }
        }
    }

    // Film: grain, moving eight times a second, and a soft vignette, as
    // old lenses had.
    Item {
        anchors.fill: parent
        visible: root.recipe.look === "film"
        Image {
            readonly property int frame: Math.floor(root.time * 8)
            readonly property var offset: {
                const r = Engine.random(frame + 1)
                return { x: Math.floor(r() * 256), y: Math.floor(r() * 256) }
            }
            source: "grain.png"
            fillMode: Image.Tile
            x: -offset.x
            y: -offset.y
            width: root.width + 256
            height: root.height + 256
            opacity: 0.09
            smooth: false
        }
        Shape {
            anchors.fill: parent
            ShapePath {
                strokeWidth: -1
                fillGradient: RadialGradient {
                    centerX: root.width / 2; centerY: root.height / 2
                    focalX: centerX; focalY: centerY
                    centerRadius: Math.hypot(root.width, root.height) / 2
                    GradientStop { position: 0.55; color: "transparent" }
                    GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0.32) }
                }
                PathRectangle { width: root.width; height: root.height }
            }
        }
    }
}
