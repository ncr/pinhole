import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.Commons
import qs.Commons as Commons
import qs.Ui

// The panel: a card under the bar icon with the newest photos as a small
// grid, the way Pinhole draws its own. A layer-shell surface of its own
// rather than the kit's KeyboardPanel: that one takes input over the whole
// screen, which would cover every window a photo could be dropped on.
// While a drag is under way only the card takes input, so the drop reaches
// the window beneath.
PanelWindow {
  id: popup

  property QtObject store: null
  property Item anchorItem: null
  property QtObject bar: null
  property QtObject owner: null
  property bool open: false
  property bool dragging: false
  signal dismissed()

  function close() { popup.dismissed() }

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  readonly property QtObject coordinatorKey: owner || popup
  readonly property var anchorWindow: anchorItem ? anchorItem.QsWindow.window : null
  readonly property string barPosition: bar ? bar.position : "top"
  readonly property real gap: Style.gapsOut
  readonly property real barThickness: !bar ? 0
    : (typeof bar.barSizeFor === "function" && screen ? bar.barSizeFor(screen.name) : bar.barSize)

  // The grid: square cells with a hairline between them, as Pinhole's.
  readonly property int columns: store ? store.columns : 4
  readonly property int rows: Math.max(1, Math.ceil(Math.max(1, store ? store.items.length : 0) / columns))
  // Every length a whole number of the screen's pixels, so at a scale such
  // as 1.6 no photo lands between pixels and is drawn soft.
  readonly property real dpr: devicePixelRatio > 0 ? devicePixelRatio : 1
  function px(v) { return Math.round(v * dpr) / dpr }
  readonly property real cell: px(Style.space(84))
  readonly property real cellGap: px(Math.max(2, Style.space(2)))
  readonly property real pad: px(Style.space(10))
  readonly property real headerHeight: px(Style.space(30))
  readonly property real footerHeight: px(Style.space(22))
  readonly property real gridWidth: columns * cell + (columns - 1) * cellGap
  readonly property real gridHeight: rows * cell + (rows - 1) * cellGap
  readonly property real cardWidth: gridWidth + pad * 2
  readonly property real cardHeight: headerHeight + gridHeight + footerHeight + pad * 2

  readonly property point cardPosition: {
    anchorTracker.transform
    var p = anchorItem && anchorWindow ? anchorItem.mapToItem(anchorWindow.contentItem, 0, 0) : Qt.point(width - cardWidth, 0)
    var aw = anchorItem ? anchorItem.width : 0
    var ah = anchorItem ? anchorItem.height : 0
    var x = p.x + aw / 2 - cardWidth / 2
    var y = barThickness + gap
    if (barPosition === "bottom") y = height - barThickness - cardHeight - gap
    else if (barPosition === "left") { x = barThickness + gap; y = p.y + ah / 2 - cardHeight / 2 }
    else if (barPosition === "right") { x = width - barThickness - cardWidth - gap; y = p.y + ah / 2 - cardHeight / 2 }
    return Qt.point(px(Math.max(gap, Math.min(x, width - cardWidth - gap))),
                    px(Math.max(gap, Math.min(y, height - cardHeight - gap))))
  }
  // Where the card grows from: the icon's side.
  readonly property real originX: anchorItem && anchorWindow
    ? anchorItem.mapToItem(anchorWindow.contentItem, anchorItem.width / 2, 0).x - cardPosition.x : cardWidth / 2

  TransformWatcher {
    id: anchorTracker
    a: popup.anchorItem
    b: popup.anchorWindow ? popup.anchorWindow.contentItem : null
  }

  screen: anchorWindow ? anchorWindow.screen : null
  visible: open || card.opacity > 0
  color: "transparent"
  exclusionMode: ExclusionMode.Ignore
  anchors { top: true; bottom: true; left: true; right: true }
  WlrLayershell.namespace: "io.github.ncr.pinhole"
  WlrLayershell.layer: WlrLayer.Overlay
  WlrLayershell.keyboardFocus: !open || dragging ? WlrKeyboardFocus.None : WlrKeyboardFocus.OnDemand
  mask: Region {
    x: popup.dragging || !popup.open ? card.x : 0
    y: popup.dragging || !popup.open ? card.y : 0
    width: !popup.open ? 0 : (popup.dragging ? card.width : popup.width)
    height: !popup.open ? 0 : (popup.dragging ? card.height : popup.height)
  }

  onOpenChanged: {
    if (open) {
      body.forceActiveFocus()
      if (bar) bar.requestPopout(coordinatorKey)
    } else if (bar && bar.activePopout === coordinatorKey) {
      bar.releasePopout(coordinatorKey)
    }
  }

  // A click anywhere off the card closes it, on the bar too.
  MouseArea {
    anchors.fill: parent
    enabled: popup.open && !popup.dragging
    acceptedButtons: Qt.AllButtons
    onPressed: popup.close()
  }

  // And on the other monitors, where drags still reach their windows.
  Variants {
    model: popup.open && !popup.dragging ? Quickshell.screens : []
    delegate: Component {
      PanelWindow {
        required property var modelData
        screen: modelData
        visible: !!popup.screen && modelData.name !== popup.screen.name
        color: "transparent"
        exclusionMode: ExclusionMode.Ignore
        anchors { top: true; bottom: true; left: true; right: true }
        WlrLayershell.namespace: "io.github.ncr.pinhole-dismiss"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onPressed: popup.close() }
      }
    }
  }

  BorderSurface {
    id: card
    x: popup.cardPosition.x; y: popup.cardPosition.y
    width: popup.cardWidth; height: popup.cardHeight
    color: Commons.Color.popups.background
    borderSpec: Border.surfaceSpec("popups", "border", Commons.Color.popups.border, Math.max(1, Style.space(2)))
    padding: 0
    radius: Style.cornerRadius

    // Grows out of the icon, and goes quicker than it came.
    opacity: popup.open ? 1 : 0
    scale: popup.open ? 1 : 0.96
    transformOrigin: popup.barPosition === "bottom" ? Item.Bottom : Item.Top
    Behavior on opacity { NumberAnimation { duration: Style.duration(popup.open ? 170 : 110); easing.type: Easing.OutCubic } }
    Behavior on scale {
      NumberAnimation { duration: Style.duration(popup.open ? 170 : 110); easing.type: Easing.Bezier; easing.bezierCurve: Style.easeOut }
    }

    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

    FocusScope {
      id: body
      anchors.fill: parent
      anchors.margins: popup.pad
      focus: true
      Keys.onEscapePressed: {
        if (popup.store && popup.store.selectedCount > 0) popup.store.selected = ({})
        else popup.close()
      }

      // ---- Header: what this is, and how fresh -----------------------
      Item {
        id: header
        width: parent.width
        height: popup.headerHeight

        Text {
          anchors.left: parent.left
          anchors.verticalCenter: parent.verticalCenter
          anchors.verticalCenterOffset: -Style.space(3)
          text: strings.t("title")
          textFormat: Text.PlainText
          font.family: popup.store ? popup.store.fontFamily : Style.font.family
          font.pixelSize: Style.font.title
          font.weight: Font.DemiBold
          color: Commons.Color.foreground
          renderType: Text.NativeRendering
        }

        Text {
          id: freshness
          anchors.right: parent.right
          anchors.verticalCenter: parent.verticalCenter
          anchors.verticalCenterOffset: -Style.space(3)
          readonly property int fresh: popup.store ? popup.store.items.filter(function(i) { return popup.store.isFresh(i) }).length : 0
          text: fresh > 0 ? strings.tn("fresh", fresh).arg(Commons.I18n.formatNumber(fresh))
                          : (popup.store && popup.store.items.length > 0 ? popup.age(popup.store.items[0].added) : "")
          textFormat: Text.PlainText
          font.family: popup.store ? popup.store.fontFamily : Style.font.family
          font.pixelSize: Style.font.caption
          color: fresh > 0 ? Commons.Color.accent : Commons.Color.muted
          renderType: Text.NativeRendering
        }
      }

      // ---- The grid ---------------------------------------------------
      Grid {
        id: grid
        y: header.height
        columns: popup.columns
        spacing: popup.cellGap
        visible: !!popup.store && popup.store.items.length > 0

        Repeater {
          model: popup.store ? popup.store.items : []
          delegate: PhotoCell {
            required property var modelData
            store: popup.store
            item: modelData
            dpr: popup.dpr
            width: popup.cell
            height: popup.cell
          }
        }
      }

      // Nothing to show: said in the grid's place, one row high.
      Text {
        y: header.height
        width: popup.gridWidth
        height: popup.cell
        visible: !grid.visible
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.WordWrap
        text: !popup.store ? "" : popup.store.missing ? strings.t("missing")
              : !popup.store.loaded ? strings.t("loading") : strings.t("empty")
        textFormat: Text.PlainText
        font.family: popup.store ? popup.store.fontFamily : Style.font.family
        font.pixelSize: Style.font.body
        color: Commons.Color.muted
        renderType: Text.NativeRendering
      }

      // ---- Footer: what a hand does here -----------------------------
      Text {
        anchors.bottom: parent.bottom
        width: parent.width
        height: popup.footerHeight
        verticalAlignment: Text.AlignBottom
        horizontalAlignment: Text.AlignLeft
        visible: !!popup.store && popup.store.items.length > 0
        readonly property int chosen: popup.store ? popup.store.selectedCount : 0
        text: chosen > 0 ? strings.tn("chosen", chosen).arg(Commons.I18n.formatNumber(chosen)) : strings.t("hint")
        textFormat: Text.PlainText
        elide: Text.ElideNone
        wrapMode: Text.NoWrap
        fontSizeMode: Text.HorizontalFit
        minimumPixelSize: Style.font.caption - 2
        font.family: popup.store ? popup.store.fontFamily : Style.font.family
        font.pixelSize: Style.font.caption
        color: chosen > 0 ? Commons.Color.accent : Commons.Color.muted
        renderType: Text.NativeRendering
      }
    }
  }

  // "just now", "12 min ago", "3 h ago", then the day.
  property real now: Date.now()
  Timer { interval: 30000; repeat: true; running: popup.open; triggeredOnStart: true; onTriggered: popup.now = Date.now() }
  function age(ms) {
    var minutes = Math.floor((popup.now - ms) / 60000)
    if (minutes < 1) return strings.t("now")
    if (minutes < 60) return strings.tn("minutes", minutes).arg(Commons.I18n.formatNumber(minutes))
    var hours = Math.floor(minutes / 60)
    if (hours < 24) return strings.tn("hours", hours).arg(Commons.I18n.formatNumber(hours))
    return new Date(ms).toLocaleDateString(Qt.locale(Commons.I18n.formatLocale), "d MMMM")
  }
}
