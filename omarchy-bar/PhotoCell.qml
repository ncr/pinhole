import QtQuick
import QtQuick.Effects
import Quickshell
import qs.Commons
import qs.Commons as Commons

// One photo in the panel's grid, and the thing you drag.
//
// As in Pinhole: a square of the photo's middle, its average colour until
// the thumbnail is read, a video's length in its corner, and a border of
// the accent around one chosen. A photo new since the panel was last
// opened has a dot of the accent.
//
// The drag is a real Wayland one (Drag.Automatic): text/uri-list of the
// files `pinhole export` made, which file managers, browsers and chats
// take. A file not made yet is waited for while the button is held, the
// cell saying so; the drag starts the moment it is there.
Item {
  id: cell

  property QtObject store: null
  property var item: ({})

  readonly property string photoId: item ? String(item.id || "") : ""
  readonly property bool chosen: store && store.selected ? store.selected[photoId] === true : false
  readonly property bool fresh: store && item && item.added !== undefined ? store.isFresh(item) === true : false
  readonly property bool waiting: wantDrag && !go
  readonly property real radius: Math.max(2, Math.min(Style.space(5), Style.cornerRadius))

  // The photos this drag carries, fixed when it begins.
  property var dragIds: []
  property bool wantDrag: false
  property bool go: false

  function ready() {
    if (!store) return false
    for (var i = 0; i < dragIds.length; i++) if (!store.isReady(dragIds[i])) return false
    return dragIds.length > 0
  }
  function tryDrag() {
    if (!wantDrag || go || !fileDrag.active || !ready()) return
    var uris = store.uriList(dragIds)
    Drag.mimeData = {
      "text/uri-list": uris.join("\r\n") + "\r\n",
      "text/plain": uris.join("\n")
    }
    go = true
  }
  Connections {
    target: cell.store
    function onPreparedChanged() { cell.tryDrag() }
  }

  Drag.active: go
  Drag.dragType: Drag.Automatic
  Drag.supportedActions: Qt.CopyAction
  Drag.onDragStarted: if (store) store.dragging = true
  // A drag is the end of the errand: the panel goes with it. Left open, its
  // click catcher would take the next click in the window dropped into.
  // On Wayland the action handed back says nothing of whether the drop
  // landed, so it closes after every drag.
  Drag.onDragFinished: {
    go = false
    wantDrag = false
    if (!store) return
    store.dragging = false
    store.open = false
  }

  DragHandler {
    id: fileDrag
    target: null
    acceptedButtons: Qt.LeftButton
    dragThreshold: Style.space(6)
    onActiveChanged: {
      if (active) {
        cell.dragIds = cell.store ? cell.store.dragIds(cell.photoId) : []
        if (cell.store) cell.store.prepare(cell.dragIds)
        cell.wantDrag = true
        cell.tryDrag()
      } else if (!cell.go) {
        cell.wantDrag = false
      }
    }
  }

  // ---- The picture --------------------------------------------------

  Item {
    id: frame
    anchors.fill: parent
    layer.enabled: true
    layer.smooth: true
    layer.effect: MultiEffect {
      maskEnabled: true
      maskSource: roundMask
      maskThresholdMin: 0.5
      maskSpreadAtMin: 1.0
    }

    Rectangle {
      anchors.fill: parent
      color: cell.item ? cell.item.color : "#303030"
    }

    Image {
      id: thumbnail
      anchors.fill: parent
      source: cell.item ? cell.item.thumb : ""
      // Read at the size it is drawn, on a HiDPI display too.
      sourceSize.width: Math.ceil(cell.width * 2)
      sourceSize.height: Math.ceil(cell.height * 2)
      fillMode: Image.PreserveAspectCrop
      asynchronous: true
      cache: true
      smooth: true
      mipmap: true
      opacity: status === Image.Ready ? 1 : 0
      Behavior on opacity { NumberAnimation { duration: Style.duration(140); easing.type: Easing.OutCubic } }
      scale: mouse.containsMouse && !cell.store.dragging ? 1.045 : 1
      Behavior on scale {
        NumberAnimation { duration: Style.duration(220); easing.type: Easing.Bezier; easing.bezierCurve: Style.easeOut }
      }
    }

    // Shade for what is drawn over the photo, only where it is.
    Rectangle {
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.bottom: parent.bottom
      height: parent.height * 0.42
      visible: cell.item && cell.item.video
      gradient: Gradient {
        GradientStop { position: 0; color: "transparent" }
        GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0.45) }
      }
    }

    // Waiting for the file a drag carries: the photo dims under a ring.
    Rectangle {
      anchors.fill: parent
      color: Qt.rgba(0, 0, 0, 0.38)
      opacity: cell.waiting ? 1 : 0
      Behavior on opacity { NumberAnimation { duration: Style.duration(120) } }
    }
  }

  Rectangle {
    id: roundMask
    anchors.fill: parent
    radius: cell.radius
    visible: false
    layer.enabled: true
  }

  // A video's length, white with a soft shadow like Pinhole's marks.
  Text {
    id: length
    anchors.left: parent.left
    anchors.bottom: parent.bottom
    anchors.margins: Style.space(5)
    visible: !!cell.item && cell.item.video
    text: cell.clock(cell.item ? cell.item.duration : 0)
    textFormat: Text.PlainText
    font.family: cell.store ? cell.store.fontFamily : Style.font.family
    font.pixelSize: Style.font.caption
    font.weight: Font.DemiBold
    font.features: { "tnum": 1 }
    color: "white"
    style: Text.Raised
    styleColor: Qt.rgba(0, 0, 0, 0.35)
    renderType: Text.NativeRendering
  }

  // New since the panel was last looked at.
  Rectangle {
    anchors.top: parent.top
    anchors.right: parent.right
    anchors.margins: Style.space(6)
    width: Style.space(8)
    height: width
    radius: width / 2
    visible: cell.fresh && !cell.chosen
    color: Commons.Color.accent
    border.width: Math.max(1, Style.space(1.5))
    border.color: Qt.rgba(0, 0, 0, 0.35)
  }

  // Chosen: the accent around it, inside the cell, and a tick.
  Rectangle {
    anchors.fill: parent
    radius: cell.radius
    color: "transparent"
    border.width: Math.max(2, Style.space(3))
    border.color: Commons.Color.accent
    visible: cell.chosen
  }
  Rectangle {
    anchors.top: parent.top
    anchors.right: parent.right
    anchors.margins: Style.space(5)
    width: Style.space(16)
    height: width
    radius: width / 2
    visible: cell.chosen
    color: Commons.Color.accent
    Text {
      anchors.centerIn: parent
      text: ""
      textFormat: Text.PlainText
      font.family: cell.store ? cell.store.fontFamily : Style.font.family
      font.pixelSize: Style.space(9)
      color: Commons.Color.background
      renderType: Text.NativeRendering
    }
  }

  // Under the pointer: a hairline of the foreground.
  Rectangle {
    anchors.fill: parent
    radius: cell.radius
    color: "transparent"
    border.width: Math.max(1, Style.spacing.hairline)
    border.color: Util.alpha(Commons.Color.foreground, 0.55)
    visible: mouse.containsMouse && !cell.chosen && !cell.store.dragging
  }

  // The ring that turns while the file is fetched.
  Item {
    anchors.centerIn: parent
    width: Style.space(22)
    height: width
    visible: cell.waiting
    Rectangle {
      anchors.fill: parent
      radius: width / 2
      color: "transparent"
      border.width: Style.space(2)
      border.color: Qt.rgba(1, 1, 1, 0.25)
    }
    Rectangle {
      width: Style.space(5)
      height: width
      radius: width / 2
      color: "white"
      x: parent.width / 2 - width / 2
      y: -width / 2 + Style.space(1)
      transformOrigin: Item.Center
    }
    RotationAnimation on rotation {
      running: cell.waiting
      from: 0; to: 360
      duration: 900
      loops: Animation.Infinite
    }
  }

  MouseArea {
    id: mouse
    anchors.fill: parent
    hoverEnabled: true
    acceptedButtons: Qt.LeftButton
    cursorShape: fileDrag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
    // A picture of the cell for the drag to carry, taken on press: well
    // before the drag threshold is crossed.
    onPressed: {
      cell.grabToImage(function(result) { if (result) cell.Drag.imageSource = result.url },
                       Qt.size(Math.round(cell.width * 0.9), Math.round(cell.height * 0.9)))
    }
    onClicked: if (cell.store) cell.store.toggleSelected(cell.photoId)
    onDoubleClicked: if (cell.store) { cell.store.toggleSelected(cell.photoId); cell.store.openPhoto(cell.photoId) }
  }

  function clock(seconds) {
    var s = Math.round(seconds)
    var m = Math.floor(s / 60)
    var h = Math.floor(m / 60)
    var pad = function(n) { return (n < 10 ? "0" : "") + n }
    return h > 0 ? h + ":" + pad(m % 60) + ":" + pad(s % 60) : m + ":" + pad(s % 60)
  }
}
