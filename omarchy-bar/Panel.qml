import QtQuick
import Quickshell
import qs.Commons
import qs.Commons as Commons
import qs.Ui

// Pinhole on the bar: its camera obscura, lit in the accent with the number
// of photos that came since the panel was last opened. A click opens the
// newest as a small grid to drag them from (PinholeStore, RecentPopup).
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

  readonly property real iconWidth: Math.round(Style.bar.iconSize * 1.17)
  readonly property real iconHeight: Math.round(Style.bar.iconSize * 0.88)
  readonly property int badgeWidth: lit
    ? Math.max(Style.space(12), String(freshCount).length * Style.space(6) + Style.space(8)) : 0
  readonly property int barContentWidth: iconWidth + (lit ? badgeWidth + Style.space(5) : 0)

  // A Panel has no size of its own: the bar is given one from what it
  // shows, never from a child filling it.
  readonly property int barSlot: barContentWidth + Style.space(10)
  readonly property real openPanelIndicatorWidth: barContentWidth
  readonly property real openPanelIndicatorHeight: barContentWidth
  implicitWidth: bar && bar.vertical ? (bar ? bar.barSize : Style.bar.sizeHorizontal) : barSlot
  implicitHeight: bar && bar.vertical ? barSlot : (bar ? bar.barSize : Style.bar.sizeHorizontal)

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

      // The camera obscura of Pinhole's icon, in one colour: the box, the
      // pinhole, and the light falling through it.
      Item {
        id: icon
        anchors.verticalCenter: parent.verticalCenter
        width: root.iconWidth
        height: root.iconHeight
        readonly property color ink: root.lit ? root.accent : root.foreground
        onInkChanged: picture.requestPaint()

        // The pinhole and its light are cut out of the box, so the bar
        // shows through them, see-through or not.
        Canvas {
          id: picture
          anchors.fill: parent
          renderStrategy: Canvas.Cooperative
          onWidthChanged: requestPaint()
          onHeightChanged: requestPaint()
          onPaint: {
            var c = getContext("2d")
            var w = width, h = height
            c.reset()
            c.globalCompositeOperation = "source-over"
            c.fillStyle = icon.ink
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
        }
      }

      // How many photos came since the panel was last opened.
      Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        visible: root.lit
        height: Style.space(12)
        width: root.badgeWidth
        radius: height / 2
        color: root.accent

        FontMetrics { id: badgeMetrics; font: badgeLabel.font }
        Text {
          id: badgeLabel
          anchors.centerIn: parent
          readonly property real pixelRatio: QsWindow.window ? QsWindow.window.devicePixelRatio : 1
          anchors.verticalCenterOffset: Math.round((Math.round(height / 2) - baselineOffset
            + badgeMetrics.capitalHeight / 2) * pixelRatio) / pixelRatio
          text: Commons.I18n.formatNumber(root.freshCount)
          textFormat: Text.PlainText
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
          font.features: { "tnum": 1 }
          renderType: Text.NativeRendering
          color: Commons.Color.background
        }
      }
    }

    onPressed: function(b) { root.toggle() }
  }
}
