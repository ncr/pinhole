import QtQuick
import QtQuick.Effects
import QtQuick.Shapes

// A mark lit like a neon tube, as Pinhole's grid lights its marks seen
// from afar (app/src/marks.cpp: neonMark(), photogrid.cpp: neonLit()): a
// tube white-hot in a tint of `colour`, the light it throws in the colour,
// and under them a wider dark shade so it shows on bright photos too. Its
// shape in a square `markPx` big, the light spreading past it.
//
// strike() lights it as the grid's tubes strike: dark until its own moment
// within the first kStagger seconds (by `seed`, so each cell's is its own),
// then stuttering on and off every 40 ms for kStrike, more on than off as
// it warms, then steady; in kSteps of brightness, as the grid draws it.
Item {
  id: neon

  // "video", a play triangle's outline; "dot", a round dot.
  property string shape: "video"
  property color colour: "#449dab"
  property real markPx: 22
  property string seed: ""
  // From 0, the tube dark, to 1, lit.
  property real lit: 1

  readonly property real kStagger: 0.8
  readonly property real kStrike: 0.5
  readonly property int kSteps: 6

  width: markPx
  height: markPx

  function hash(text, salt) {
    var h = 2166136261 ^ salt
    for (var i = 0; i < text.length; i++) {
      h ^= text.charCodeAt(i)
      h = Math.imul(h, 16777619)
    }
    h ^= h >>> 16
    h = Math.imul(h, 0x7feb352d)
    h ^= h >>> 15
    return (h >>> 0) / 4294967296
  }

  function litAt(t) {
    var s = t - kStagger * hash(seed, 0)
    if (s < 0) return 0
    if (s >= kStrike) return 1
    var on = hash(seed, Math.floor(s / 0.04) + 1) < 0.2 + 0.8 * s / kStrike
    return on ? 1 : 0.2
  }

  property real startedAt: 0
  function strike() {
    startedAt = Date.now()
    lit = 0
    striker.restart()
  }

  Timer {
    id: striker
    interval: 16
    repeat: true
    onTriggered: {
      var t = (Date.now() - neon.startedAt) / 1000
      var l = neon.litAt(t)
      neon.lit = Math.round(l * (neon.kSteps - 1)) / (neon.kSteps - 1)
      if (t > neon.kStagger + neon.kStrike) {
        neon.lit = 1
        stop()
      }
    }
  }

  // White-hot when lit, the colour's dark glass when not.
  readonly property color core: {
    var hot = function(c) { return c * 0.3 + (c + (1 - c) * 0.3 - c * 0.3) * neon.lit }
    return Qt.rgba(hot(colour.r), hot(colour.g), hot(colour.b), 0.55 + 0.45 * lit)
  }

  // The tube's shape, white: what the shade and the light are made of.
  component Tube: Shape {
    id: tubeShape
    property color ink: "white"
    // As thick as this many tubes: the light is the tube blurred, and a
    // thicker one throws more of it.
    property real swell: 1
    anchors.fill: parent
    preferredRendererType: Shape.CurveRenderer
    ShapePath {
      strokeColor: neon.shape === "video" ? tubeShape.ink : "transparent"
      strokeWidth: neon.shape === "video" ? neon.markPx * 0.08 * tubeShape.swell : 0
      fillColor: neon.shape === "video" ? "transparent" : tubeShape.ink
      joinStyle: ShapePath.RoundJoin
      capStyle: ShapePath.RoundCap
      PathPolyline {
        path: neon.shape === "video"
          ? [Qt.point(neon.markPx * 0.35, neon.markPx * 0.25), Qt.point(neon.markPx * 0.35, neon.markPx * 0.75),
             Qt.point(neon.markPx * 0.77, neon.markPx * 0.5), Qt.point(neon.markPx * 0.35, neon.markPx * 0.25)]
          : []
      }
      PathAngleArc {
        centerX: neon.markPx / 2
        centerY: neon.markPx / 2
        radiusX: neon.shape === "dot" ? neon.markPx * 0.2 * Math.sqrt(tubeShape.swell) : 0
        radiusY: radiusX
        startAngle: 0
        sweepAngle: neon.shape === "dot" ? 360 : 0
      }
    }
  }

  Tube {
    id: source
    visible: false
    layer.enabled: true
  }
  Tube {
    id: wide
    swell: 1.8
    visible: false
    layer.enabled: true
  }

  // The shade under it, wider than the light.
  MultiEffect {
    source: wide
    anchors.fill: wide
    blurEnabled: true
    blur: 1
    blurMax: Math.round(neon.markPx / 4)
    brightness: -1
    opacity: (0.4 + 0.6 * neon.lit) * 0.5
  }
  // The light it throws: soft around it, and bright hugging it.
  MultiEffect {
    source: wide
    anchors.fill: wide
    blurEnabled: true
    blur: 1
    blurMax: Math.round(neon.markPx / 5)
    colorization: 1
    colorizationColor: neon.colour
    opacity: 0.85 * neon.lit
  }
  MultiEffect {
    source: source
    anchors.fill: source
    blurEnabled: true
    blur: 1
    blurMax: Math.max(2, Math.round(neon.markPx / 14))
    colorization: 1
    colorizationColor: neon.colour
    opacity: neon.lit
  }
  Tube {
    ink: neon.core
  }
}
