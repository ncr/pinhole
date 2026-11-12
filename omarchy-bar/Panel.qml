import QtQuick
import Quickshell
import qs.Commons
import qs.Commons as Commons
import qs.Ui

// Pinhole on the bar: its camera obscura, lit in the accent with the number
// of photos that came since the panel was last opened. A click opens the
// newest as a small grid to drag them from (PinholeStore, RecentPopup).
// A right click opens Pinhole; the icon dragged off the bar by the right
// button opens it where it is let go (the left button's drag is the bar's
// own, which moves its widgets).
//
// The bar makes a widget on every monitor; what they show is the store's,
// one for the shell.
Panel {
  id: root

  moduleName: "io.github.ncr.pinhole"
  ipcTarget: "io.github.ncr.pinhole"

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  readonly property color accent: Commons.Color.accent
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property int freshCount: PinholeStore.freshCount
  readonly property bool lit: freshCount > 0
  // Files held over the icon that it would add (DropArea below).
  property bool dropping: false
  // What the badge says: how many came, or how a drop on the icon went.
  readonly property string result: PinholeStore.addResult
  readonly property string badgeText: result === "added" ? "\uf00c" : result === "problem" ? "\uf12a"
    : lit ? Commons.I18n.formatNumber(freshCount) : ""
  readonly property color badgeColor: result === "problem" ? Commons.Color.urgent : accent

  readonly property real iconWidth: Math.round(Style.bar.iconSize * 1.17)
  readonly property real iconHeight: Math.round(Style.bar.iconSize * 0.88)
  readonly property int badgeWidth: badgeText !== ""
    ? Math.max(Style.space(12), (result !== "" ? 1 : badgeText.length) * Style.space(6) + Style.space(8)) : 0
  readonly property int barContentWidth: iconWidth + (badgeWidth > 0 ? badgeWidth + Style.space(5) : 0)

  // A Panel has no size of its own: the bar is given one from what it
  // shows, never from a child filling it.
  readonly property int barSlot: barContentWidth + Style.space(10)
  readonly property real openPanelIndicatorWidth: barContentWidth
  readonly property real openPanelIndicatorHeight: barContentWidth
  implicitWidth: bar && bar.vertical ? (bar ? bar.barSize : Style.bar.sizeHorizontal) : barSlot
  implicitHeight: bar && bar.vertical ? barSlot : (bar ? bar.barSize : Style.bar.sizeHorizontal)

  // The camera obscura of Pinhole's icon, in one colour: the box, with the
  // pinhole and its light cut out of it, so what is behind shows through.
  function paintBox(c, x, y, w, h, ink) {
    c.reset()
    c.translate(x, y)
    c.globalCompositeOperation = "source-over"
    c.fillStyle = ink
    var r = Math.max(1.5, h * 0.2)
    c.beginPath()
    c.roundedRect(0, 0, w, h, r, r)
    c.fill()
    c.globalCompositeOperation = "destination-out"
    c.globalAlpha = 0.6
    c.beginPath()
    c.moveTo(w * 0.5, h * 0.5)
    c.lineTo(w * 0.86, h * 0.17)
    c.lineTo(w * 0.86, h * 0.83)
    c.closePath()
    c.fill()
    c.globalAlpha = 1
    c.beginPath()
    c.arc(w * 0.5, h * 0.5, Math.max(1.1, h * 0.14), 0, Math.PI * 2)
    c.fill()
  }

  Component.onCompleted: PinholeStore.fontFamily = root.fontFamily
  onFontFamilyChanged: PinholeStore.fontFamily = root.fontFamily

  // The button and the panel follow each other both ways: opened over IPC,
  // by a click, or closed by a click off the card. Only the widget that
  // opened it places it; the other monitors' only light their button.
  onOpenedChanged: {
    if (root.opened) {
      if (!PinholeStore.open) PinholeStore.showOn(button, root.bar, root)
    } else if (PinholeStore.open && PinholeStore.anchorOwner === root) {
      PinholeStore.hide()
    }
  }
  Connections {
    target: PinholeStore
    function onOpenChanged() {
      if (PinholeStore.open && !root.opened && PinholeStore.anchorOwner === root) root.open()
      else if (!PinholeStore.open && root.opened) root.close()
    }
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    slotSize: root.barSlot
    hasVisualContent: true
    tooltipText: root.lit ? strings.tn("tooltip.fresh", root.freshCount).arg(Commons.I18n.formatNumber(root.freshCount))
                          : strings.t("tooltip")

    Row {
      anchors.centerIn: parent
      spacing: Style.space(5)

      Item {
        id: icon
        anchors.verticalCenter: parent.verticalCenter
        width: root.iconWidth
        height: root.iconHeight
        readonly property color ink: root.result === "problem" ? Commons.Color.urgent
          : root.lit || root.dropping || PinholeStore.adding || root.result === "added" ? root.accent : root.foreground
        onInkChanged: picture.requestPaint()
        // Held over it, the box opens a little for what is dropped.
        scale: root.dropping ? 1.18 : 1
        Behavior on scale {
          NumberAnimation { duration: Style.duration(160); easing.type: Easing.Bezier; easing.bezierCurve: Style.easeOut }
        }

        // While a drop is being added: how far, as a line under the box.
        Rectangle {
          anchors.top: parent.bottom
          anchors.topMargin: Math.max(2, Style.space(2.5))
          width: parent.width
          height: Math.max(1.5, Style.space(2))
          radius: height / 2
          color: Util.alpha(root.accent, 0.28)
          visible: PinholeStore.adding
          Rectangle {
            width: Math.max(parent.height, parent.width * PinholeStore.addProgress)
            height: parent.height
            radius: parent.radius
            color: root.accent
            Behavior on width { NumberAnimation { duration: Style.duration(200); easing.type: Easing.OutCubic } }
          }
        }

        Canvas {
          id: picture
          anchors.fill: parent
          renderStrategy: Canvas.Cooperative
          onWidthChanged: requestPaint()
          onHeightChanged: requestPaint()
          onPaint: root.paintBox(getContext("2d"), 0, 0, width, height, icon.ink)
        }
      }

      // How many photos came since the panel was last opened.
      Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        visible: root.badgeText !== ""
        height: Style.space(12)
        width: root.badgeWidth
        radius: height / 2
        color: root.badgeColor

        FontMetrics { id: badgeMetrics; font: badgeLabel.font }
        Text {
          id: badgeLabel
          anchors.centerIn: parent
          readonly property real pixelRatio: QsWindow.window ? QsWindow.window.devicePixelRatio : 1
          anchors.verticalCenterOffset: Math.round((Math.round(height / 2) - baselineOffset
            + badgeMetrics.capitalHeight / 2) * pixelRatio) / pixelRatio
          text: root.badgeText
          textFormat: Text.PlainText
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
          font.features: { "tnum": 1 }
          renderType: Text.NativeRendering
          color: Commons.Color.background
        }
      }
    }

    // Left: the newest photos. Right: Pinhole itself.
    onPressed: function(b) {
      if (b === Qt.RightButton) PinholeStore.openPinhole()
      else root.toggle()
    }

    // The icon, carried off the bar by the right button: a real Wayland
    // drag, so its picture follows the pointer over any window, of
    // something no window takes. Where it ends is where Pinhole opens;
    // back over the bar, nowhere.
    DragHandler {
      target: null
      acceptedButtons: Qt.RightButton
      dragThreshold: Style.space(6)
      onActiveChanged: {
        if (!active || carried.go) return
        if (root.bar) root.bar.hideTooltip(button)
        PinholeStore.open = false
        carried.grabToImage(function(result) {
          if (result) carried.Drag.imageSource = result.url
          carried.go = true
        })
      }
    }

    // What the drag carries: the icon, a size up, beside the pointer and
    // not under the hand that holds it (the picture hangs from its top left
    // corner). Never seen on the bar.
    Canvas {
      id: carried
      property bool go: false
      readonly property real aside: Style.space(13)
      width: aside + Math.round(root.iconWidth * 1.4)
      height: aside + Math.round(root.iconHeight * 1.4)
      opacity: 0
      renderStrategy: Canvas.Cooperative
      onWidthChanged: requestPaint()
      onHeightChanged: requestPaint()
      onPaint: root.paintBox(getContext("2d"), aside, aside, width - aside, height - aside, root.foreground)
      Connections {
        target: root
        function onForegroundChanged() { carried.requestPaint() }
      }

      Drag.active: go
      Drag.dragType: Drag.Automatic
      Drag.supportedActions: Qt.MoveAction
      Drag.mimeData: ({ "application/x-pinhole-window": "pinhole" })
      Drag.onDragFinished: {
        go = false
        // The bar's strip on its screen, in the layout's coordinates: the
        // bar's window is as big as its screen.
        var screen = QsWindow.window ? QsWindow.window.screen : null
        var from = root.mapToItem(null, 0, 0)
        var vertical = root.bar && root.bar.vertical
        var start = vertical ? (screen ? screen.x : 0) + from.x : (screen ? screen.y : 0) + from.y
        PinholeStore.openHere(vertical ? "x" : "y", start, start + (vertical ? root.width : root.height))
      }
    }
  }

  // Photos dropped on the icon are added to Pinhole, as on its window.
  DropArea {
    anchors.fill: parent
    keys: ["text/uri-list"]
    onEntered: function(drag) {
      var wanted = PinholeStore.droppable(drag.urls).length > 0
      drag.accepted = wanted
      root.dropping = wanted
    }
    onExited: root.dropping = false
    onDropped: function(drop) {
      root.dropping = false
      var paths = PinholeStore.droppable(drop.urls)
      if (paths.length === 0) return
      drop.accept(Qt.CopyAction)
      PinholeStore.add(paths)
    }
  }
}
