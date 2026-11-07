import QtQuick
import Pinhole.Plugin
import Pinhole.Internal.Info
import "Places.js" as Geo

FocusScope {
    id: places
    objectName: "photoPlaces"
    // Shown in Main.qml's PluginView (M), made afresh each time it opens:
    // `view` is that, `view.plugin` Info's root.
    property var points: []
    property var groups: []
    // The world's groups at `clusteredAt` (Places.js clusters()), made
    // again only when the zoom or the photos change: a pan takes those
    // around the view from them.
    property var clustered: []
    property var clusteredFrom: null
    property real clusteredAt: -1
    // The markers' rows, by group key, so a pan adds and drops the markers
    // at the edges and keeps the rest as they are.
    property var groupByKey: ({})
    ListModel { id: markerRows }
    readonly property var noGroup: ({ key: "", x: 0, y: -10, cover: { id: "" }, points: [] })
    property var gallery: []
    property var pending: null
    property var focusPoint: null
    readonly property string selectedPlace: openedGroup?.cover.id ?? ""
    property string keyboardPlace: ""
    property var openedGroup: null
    property bool areaOpen: false
    readonly property bool galleryOpen: areaOpen || openedGroup !== null
    readonly property bool sideGallery: width >= theme.fontSize * 78 && height >= theme.fontSize * 30
    readonly property real viewWidth: Math.max(1, map.width - (galleryOpen && sideGallery ? galleryPanel.width : 0))
    readonly property real viewHeight: Math.max(1, map.height - (galleryOpen && !sideGallery ? galleryPanel.height : 0))
    readonly property real viewCenterX: cx + (viewWidth - map.width) / (2 * scale)
    readonly property real viewCenterY: cy + (viewHeight - map.height) / (2 * scale)
    readonly property var currentGroup: groups.find(group => group.points.some(p => p.id === keyboardPlace)) ?? null
    property bool loading: false
    property bool fitted: false
    property bool failed: false
    property alias cursor: photos.cursor
    property real cx: 0.5
    property real cy: 0.5
    property real zoom: 1
    readonly property real scale: Geo.scale(zoom)
    // Not those hidden from the grid (Screenshots'), as the grid.
    property int hiddenRevision: 0
    readonly property var shownPoints: (hiddenRevision, points.filter(p => !pinhole.isHidden(p.id)))
    readonly property var availablePoints: {
        // Info can have EXIF coordinates for a source that supplies no pair.
        if (focusPoint && !shownPoints.some(p => p.id === focusPoint.id))
            return shownPoints.concat([focusPoint]).sort(byDate)
        return shownPoints
    }
    readonly property bool hasPhotos: availablePoints.length > 0
    // Where they are, as two lists, for the grouping (Places.groupsAt).
    readonly property var availableXs: availablePoints.map(p => p.x)
    readonly property var availableYs: availablePoints.map(p => p.y)

    function start(focus) {
        focusPoint = focus
        if (focus) {
            camera(focus.x, focus.y, 14)
            fitted = true
        }
        // Called before the view shows: the keys once it does.
        Qt.callLater(() => map.forceActiveFocus())
        refresh()
    }
    function refresh() {
        if (pending)
            pending.cancel()
        failed = false
        loading = true
        readPage(null, [])
    }
    // Keyset paging stays within the plugin query budget on large libraries.
    function readPage(after, result) {
        const pageSize = 8000
        const job = pinhole.query("SELECT id, latitude, longitude, asset_date FROM assets "
            + "WHERE latitude IS NOT NULL " + (after === null ? "" : "AND id > ? ")
            + "ORDER BY id LIMIT ?", after === null ? [pageSize] : [after, pageSize])
        pending = job
        job.then(rows => {
            if (pending !== job)
                return
            for (const row of rows) {
                const p = Geo.point(row.id, row.latitude, row.longitude, row.asset_date)
                if (p)
                    result.push(p)
            }
            if (rows.length === pageSize) {
                readPage(rows[rows.length - 1].id, result)
                return
            }
            pending = null
            points = result.sort(byDate)
            loading = false
            if (!fitted && points.length > 0)
                fitAll()
            regroup()
        }).catch(error => {
            if (pending !== job)
                return
            pending = null
            loading = false
            failed = error.code !== "cancelled" && error.code !== "reloaded"
        })
    }
    function byDate(a, b) {
        return b.date - a.date || (a.id < b.id ? -1 : a.id === b.id ? 0 : 1)
    }
    function camera(x, y, level) {
        cx = Geo.wrap(x)
        zoom = Geo.clamp(level, 0, Geo.maxZoom)
        const half = Math.min(0.5, viewHeight / (2 * scale))
        const offset = (map.height - viewHeight) / (2 * scale)
        cy = Geo.clamp(y - offset, half, 1 - half) + offset
        scheduleGroups()
    }
    function scheduleGroups() {
        if (!settle.running)
            settle.start()
    }
    function centerOn(x, y, level) {
        // Put somewhere else (Fit all, a group, the keys): what glided stops.
        gestures.stop()
        const nextScale = Geo.scale(level)
        camera(x + (map.width - viewWidth) / (2 * nextScale),
               y + (map.height - viewHeight) / (2 * nextScale), level)
    }
    function fitAll() {
        const view = Geo.fit(availablePoints, viewWidth, viewHeight)
        fitted = hasPhotos
        centerOn(view.x, view.y, view.zoom)
    }
    function zoomAt(x, y, level) {
        const before = scale
        const after = Geo.scale(Geo.clamp(level, 0, Geo.maxZoom))
        camera(cx + (x - map.width / 2) * (1 / before - 1 / after),
               cy + (y - map.height / 2) * (1 / before - 1 / after), level)
    }
    function stepZoom(amount) {
        gestures.stop()
        const group = map.activeFocus && pinhole.keyboardInUse ? currentGroup : null
        zoomAt(group ? screenX(group) : viewWidth / 2, group ? screenY(group) : viewHeight / 2,
               zoom + amount)
    }
    function pan(dx, dy) {
        camera(cx - dx / scale, cy - dy / scale, zoom)
    }
    function screenX(p) { return map.width / 2 + Geo.delta(p.x, cx) * scale }
    function screenY(p) { return map.height / 2 + (p.y - cy) * scale }
    function isInView(p) {
        const x = screenX(p), y = screenY(p)
        return x >= 0 && x < viewWidth && y >= 0 && y < viewHeight
    }
    function regroup() {
        // While a zoom glides, grouped again only every half a step (all
        // the photos are gone through), and once more where it stops.
        const zoomed = gestures.moving ? Math.abs(clusteredAt - zoom) >= 0.5 : clusteredAt !== zoom
        if (clusteredFrom !== availablePoints || zoomed) {
            clustered = Geo.groupsOf(availablePoints, Places.groupsAt(availableXs, availableYs, zoom), zoom)
            clusteredFrom = availablePoints
            clusteredAt = zoom
        }
        const next = Geo.around(clustered, viewCenterX, viewCenterY, zoom, viewWidth, viewHeight)
        if (!Geo.sameGroups(groups, next))
            showGroups(next)
        if (!currentGroup || !isInView(currentGroup))
            choosePlace(nearestPlace())
        if (areaOpen)
            setGallery(availablePoints.filter(isInView), true)
    }
    function showGroups(next) {
        const byKey = {}
        for (const group of next)
            byKey[group.key] = group
        for (let i = markerRows.count - 1; i >= 0; --i)
            if (!(markerRows.get(i).key in byKey))
                markerRows.remove(i)
        const shown = new Set()
        for (let i = 0; i < markerRows.count; ++i)
            shown.add(markerRows.get(i).key)
        groupByKey = byKey
        groups = next
        for (const group of next)
            if (!shown.has(group.key))
                markerRows.append({ key: group.key })
    }
    function nearestPlace() {
        let closest = null, best = Infinity
        for (const group of groups) {
            if (!isInView(group))
                continue
            const score = (screenX(group) - viewWidth / 2) ** 2 + (screenY(group) - viewHeight / 2) ** 2
            if (score < best) {
                best = score
                closest = group
            }
        }
        return closest
    }
    function choosePlace(group) {
        keyboardPlace = group ? group.cover.id : ""
    }
    function movePlace(dx, dy) {
        if (!currentGroup) {
            const first = nearestPlace()
            if (first) {
                choosePlace(first)
                return
            }
        }
        const fromX = currentGroup ? screenX(currentGroup) : viewWidth / 2
        const fromY = currentGroup ? screenY(currentGroup) : viewHeight / 2
        let closest = null, best = Infinity
        for (const group of groups) {
            if (group === currentGroup || !isInView(group))
                continue
            const x = screenX(group) - fromX, y = screenY(group) - fromY
            const forward = x * dx + y * dy
            const sideways = Math.abs(x * dy - y * dx)
            if (forward <= 0 || sideways > forward * 2)
                continue
            const score = forward + sideways * 2
            if (score < best) {
                best = score
                closest = group
            }
        }
        if (closest)
            choosePlace(closest)
        else
            pan(-dx * viewWidth / 3, -dy * viewHeight / 3)
    }
    function keepInView(group) {
        const x = screenX(group), y = screenY(group)
        pan(Geo.clamp(x, 37, viewWidth - 37) - x, Geo.clamp(y, 37, viewHeight - 37) - y)
    }
    function showGroup(group) {
        choosePlace(group)
        if (group.points.length === 1) {
            view.plugin.viewMapPhoto(group.points[0].id)
            return
        }
        if (selectedPlace === group.cover.id) {
            photos.forceActiveFocus()
            return
        }
        areaOpen = false
        openedGroup = group
        setGallery(group.points, false)
        keepInView(group)
        photos.forceActiveFocus()
    }
    function zoomGroup(group) {
        if (!group)
            return
        const view = Geo.fit(group.points, viewWidth, viewHeight)
        const level = Math.min(Geo.maxZoom, zoom + 3, Math.max(zoom + 1, view.zoom))
        choosePlace(group)
        if (level <= view.zoom + 0.01) {
            centerOn(view.x, view.y, level)
            return
        }
        // Closer than the whole group fits: towards where most of it is,
        // not the empty middle between its ends.
        const radius = Math.min(viewWidth, viewHeight) / (2.5 * Geo.scale(level))
        const apart = (a, b) => Math.abs(a - b - Math.round(a - b))
        let best = group.points[0], most = 0
        for (const p of group.points) {
            let near = 0
            for (const q of group.points)
                if (apart(q.x, p.x) < radius && Math.abs(q.y - p.y) < radius)
                    ++near
            if (near > most) {
                most = near
                best = p
            }
        }
        const around = group.points.filter(q => apart(q.x, best.x) < radius && Math.abs(q.y - best.y) < radius)
        const middle = Geo.fit(around, viewWidth, viewHeight)
        centerOn(middle.x, middle.y, level)
    }
    function setGallery(items, keepCursor) {
        const previous = keepCursor ? gallery[cursor]?.id : undefined
        const next = items
        if (keepCursor && next.length === gallery.length && next.every((p, i) => p.id === gallery[i].id))
            return
        const nextCursor = previous ? next.findIndex(p => p.id === previous) : -1
        if (next.length === 0 && photos.activeFocus)
            map.forceActiveFocus()
        gallery = next
        cursor = nextCursor >= 0 ? nextCursor : 0
        if (nextCursor < 0)
            photos.view.positionViewAtBeginning()
    }
    function showVisible() {
        if (areaOpen) {
            hidePhotos()
            return
        }
        openedGroup = null
        areaOpen = true
        setGallery(availablePoints.filter(isInView), false)
        if (pinhole.keyboardInUse && gallery.length)
            photos.forceActiveFocus()
    }
    function hidePhotos() {
        openedGroup = null
        areaOpen = false
        gallery = []
        map.forceActiveFocus()
    }
    // Esc (PluginView): the photos shown put away first, then the map
    // closed.
    function back() {
        if (!galleryOpen)
            return false
        hidePhotos()
        return true
    }
    function goBack() {
        if (!back())
            view.close()
    }
    function openGalleryPhoto(at) {
        const photo = gallery[at]
        if (photo) {
            cursor = at
            view.plugin.viewMapPhoto(photo.id, gallery.map(p => p.id))
        }
    }
    // When the photos shown were taken: a month, months of a year, or years.
    readonly property string galleryTime: {
        let first = Infinity, last = 0
        for (const p of gallery) {
            if (!p.date) continue
            first = Math.min(first, p.date)
            last = Math.max(last, p.date)
        }
        if (!last) return ""
        const from = new Date(first), to = new Date(last)
        const locale = Qt.locale()
        if (from.getFullYear() !== to.getFullYear())
            return qsTr("%1 – %2").arg(from.getFullYear()).arg(to.getFullYear())
        if (from.getMonth() !== to.getMonth())
            return qsTr("%1 – %2 %3").arg(locale.standaloneMonthName(from.getMonth())).arg(locale.standaloneMonthName(to.getMonth())).arg(to.getFullYear())
        return qsTr("%1 %2").arg(locale.standaloneMonthName(to.getMonth())).arg(to.getFullYear())
    }
    // Back from the photos: on the one last looked at, in the gallery it
    // was opened from, or the map.
    function cameBack(photo) {
        const at = gallery.findIndex(p => p.id === photo)
        if (at >= 0) {
            cursor = at
            photos.view.positionViewAtIndex(at, GridView.Contain)
            photos.forceActiveFocus()
        } else {
            map.forceActiveFocus()
        }
    }
    onViewWidthChanged: scheduleGroups()
    onViewHeightChanged: camera(cx, cy, zoom)
    Timer { id: settle; interval: 65; onTriggered: places.regroup() }
    Timer { id: reload; interval: 300; onTriggered: places.refresh() }
    Connections {
        target: pinhole.library
        function onChanged() { reload.restart() }
    }
    Connections {
        target: pinhole
        function onHiddenChanged() {
            ++places.hiddenRevision
            places.scheduleGroups()
        }
    }
    Component.onDestruction: { if (pending) pending.cancel() }
    Panel {}

    PanelHeader {
        id: titlebar
        readonly property bool compact: width < theme.fontSize * 40
        title: qsTr("Map")
        // ✕ closes the view, from the photos too; M, its toggleKey, does.
        closeHint: "M"
        tools: [
            ShellButton {
                objectName: "placesFit"
                text: titlebar.compact ? qsTr("Fit") : qsTr("Fit all")
                helpText: qsTr("Fit all")
                action: fitCommand
                hint: titlebar.compact ? "" : fitCommand.keyName
            },
            ShellButton {
                objectName: "placesArea"
                text: titlebar.compact ? qsTr("Photos") : qsTr("Photos in view")
                helpText: qsTr("Photos in view")
                action: areaCommand
                active: places.areaOpen
                hint: titlebar.compact ? "" : areaCommand.keyName
            }
        ]
    }

    // How to get about, for a screen reader: the help (?) lists the keys.
    readonly property string inputHint: {
        if (pinhole.keyboardInUse)
            return photos.activeFocus
                ? qsTr("Arrows browse photos · ⏎ opens · Esc goes back")
                : qsTr("Arrows choose a place · ⇧ + arrows move · ⏎ opens")
        if (pinhole.input === "touchpad")
            return qsTr("Two fingers move · Pinch to zoom")
        return qsTr("Drag to move · Scroll to zoom")
    }

    Rectangle {
        id: map
        objectName: "placesViewport"
        anchors.top: titlebar.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        activeFocusOnTab: true
        Accessible.role: Accessible.Pane
        Accessible.name: places.currentGroup
            ? qsTr("%n photo(s) here", "", places.currentGroup.points.length) : qsTr("Map")
        Accessible.description: places.inputHint
        color: tiles.land
        clip: true

        MapTiles {
            id: tiles
            width: places.viewWidth
            height: places.viewHeight
            visible: places.hasPhotos && places.visible
            centerX: Geo.wrap(places.viewCenterX)
            centerY: places.viewCenterY
            zoom: places.zoom
            // The theme's colours: the window's background is the land.
            background: theme.background
            foreground: theme.foreground
            brightForeground: theme.brightForeground
            blue: theme.blue
            green: theme.green
            light: theme.light
            fontFamily: theme.font
            fontSize: theme.fontSize
            // Names keep clear of the photos' markers.
            avoid: places.groups.filter(g => places.isInView(g)).map(g =>
                Qt.rect(places.screenX(g) - 34, places.screenY(g) - 34, 74, 74))
        }
        // The kit's continuous pan and zoom, on the grid's physics: a swipe
        // or a drag coasts on, the wheel glides notch by notch.
        PanZoom {
            id: gestures
            anchors.fill: parent
            zoomPerNotch: 0.65
            onPanned: (dx, dy) => places.pan(dx, dy)
            onZoomed: (x, y, steps) => places.zoomAt(x, y, places.zoom + steps)
            onMovingChanged: moving ? map.forceActiveFocus() : places.scheduleGroups()
            onTapped: map.forceActiveFocus()
            onDoubleTapped: (x, y) => zoomBy(x, y, 1)
            HoverHandler { cursorShape: gestures.dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor }

            Repeater {
                model: markerRows
                delegate: Item {
                    id: marker
                    objectName: "placesMarker"
                    required property string key
                    readonly property var modelData: places.groupByKey[key] ?? places.noGroup
                    readonly property int count: modelData.points.length
                    readonly property bool focusedPhoto: places.focusPoint !== null
                        && modelData.points.some(p => p.id === places.focusPoint.id)
                    readonly property bool highlighted: focusedPhoto || (places.selectedPlace !== ""
                        && modelData.points.some(p => p.id === places.selectedPlace))
                    readonly property var photo: (pinhole.library.revision,
                        pinhole.library.item(focusedPhoto ? places.focusPoint.id : modelData.cover.id))
                    readonly property bool keyboardFocus: map.activeFocus && pinhole.keyboardInUse
                        && places.currentGroup === modelData
                    x: places.screenX(modelData) - width / 2
                    y: places.screenY(modelData) - height / 2
                    width: 56
                    height: 56
                    visible: places.isInView(modelData)
                    z: keyboardFocus || highlighted || hover.hovered ? 2 : 0
                    Accessible.role: Accessible.Button
                    Accessible.name: count > 1 ? qsTr("%n photo(s) here", "", count)
                                               : (photo.filename || qsTr("Open the photo"))
                    Accessible.onPressAction: places.showGroup(modelData)
                    // Omarchy's cards: square corners (or the theme's own
                    // rounding), a hairline; several photos as a second
                    // card behind, offset.
                    readonly property int corner: Math.min(4, theme.radius)
                    readonly property bool lit: marker.highlighted || hover.hovered || marker.keyboardFocus
                    Rectangle {
                        visible: marker.count > 1
                        x: 4; y: -4
                        width: parent.width; height: parent.height
                        radius: marker.corner
                        color: theme.background
                        border.width: 1
                        border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.55)
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: marker.corner
                        color: theme.background
                        Image {
                            anchors.fill: parent
                            anchors.margins: 2
                            source: marker.photo.thumbnail || ""
                            sourceSize: Qt.size(112, 112)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: marker.corner
                            color: "transparent"
                            border.width: marker.lit ? 2 : 1
                            border.color: marker.lit ? theme.accent
                                : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.55)
                        }
                    }
                    // How many, as a chip on the corner: tabular digits,
                    // centred on their capital height.
                    Rectangle {
                        visible: marker.count > 1
                        x: -5
                        y: parent.height - height + 5
                        width: Math.max(height, badge.implicitWidth + 12)
                        height: Math.round(theme.fontSize * 1.6)
                        radius: marker.corner
                        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.94)
                        border.width: 1
                        border.color: marker.lit ? theme.accent
                            : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)
                        UiText {
                            id: badge
                            x: Math.round((parent.width - implicitWidth) / 2)
                            y: centredIn(parent.height)
                            font.pixelSize: Math.round(theme.fontSize * 0.857)
                            font.features: { "tnum": 1 }
                            text: marker.count.toLocaleString(Qt.locale(), "f", 0)
                            color: marker.lit ? theme.accent : theme.brightForeground
                        }
                    }
                    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            if (tapCount === 1)
                                places.showGroup(marker.modelData)
                        }
                        onDoubleTapped: places.zoomGroup(marker.modelData)
                    }
                }
            }
        }

        Column {
            x: Math.max(0, places.viewWidth - width - 8)
            anchors.bottom: mapFooter.top
            anchors.bottomMargin: 8
            spacing: 4
            ShellButton {
                text: "+"
                action: zoomIn
                width: minus.width
                hint: ""
                floating: true
            }
            ShellButton {
                id: minus
                text: "−"
                action: zoomOut
                hint: ""
                floating: true
            }
        }

        Row {
            id: mapFooter
            x: Math.max(0, places.viewWidth - width - 8)
            y: Math.max(0, places.viewHeight - height - 8)
            spacing: 6
            ShellButton {
                text: qsTr("Retry map")
                visible: tiles.unavailable
                floating: true
                onTriggered: tiles.retry()
            }
            ShellButton {
                text: qsTr("OpenFreeMap © OpenMapTiles © OpenStreetMap")
                helpText: qsTr("Map data attribution")
                floating: true
                visible: tiles.visible
                onTriggered: Qt.openUrlExternally("https://openfreemap.org/")
            }
        }
        UiText {
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - 30)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: theme.brightForeground
            visible: !places.hasPhotos
            text: places.loading ? qsTr("Finding places…") : places.failed
                ? qsTr("Could not read photo locations. Try opening the map again.")
                : qsTr("No photos with a location in this library yet.")
        }
    }

    Rectangle {
        id: galleryPanel
        objectName: "placesDrawer"
        width: places.sideGallery ? Math.min(places.width * 0.35, theme.fontSize * 32) : places.width
        height: places.sideGallery ? map.height : Math.min(map.height * 0.44, theme.fontSize * 25)
        visible: places.galleryOpen
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        color: theme.background
        Panel {}
        Rectangle {
            color: theme.darkForeground
            width: places.sideGallery ? 1 : parent.width
            height: places.sideGallery ? parent.height : 1
        }
        Item {
            id: galleryBar
            width: parent.width
            height: galleryControls.height + 12
            Column {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.right: galleryControls.left
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                UiText {
                    width: parent.width
                    color: theme.brightForeground
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: Math.round(theme.fontSize * 0.75)
                    font.pixelSize: theme.fontSize
                    text: qsTr("%n photo(s) here", "", places.gallery.length)
                }
                UiText {
                    width: parent.width
                    visible: text !== ""
                    color: theme.darkForeground
                    font.pixelSize: Math.round(theme.fontSize * 0.857)
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: Math.round(theme.fontSize * 0.7)
                    text: places.galleryTime
                }
            }
            Row {
                id: galleryControls
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                ShellButton {
                    objectName: "placesZoomGroup"
                    text: qsTr("Zoom to group")
                    action: groupZoom
                    visible: places.openedGroup !== null
                    hint: galleryBar.width < theme.fontSize * 32 ? "" : groupZoom.keyName
                }
                ShellButton {
                    objectName: "placesHideGallery"
                    text: "✕"
                    helpText: qsTr("Hide photos")
                    onTriggered: places.hidePhotos()
                }
            }
        }
        UiText {
            anchors.centerIn: parent
            visible: places.areaOpen && places.gallery.length === 0
            text: qsTr("No photos in this area")
            color: theme.foreground
        }
        PhotoTiles {
            id: photos
            objectName: "placesGallery"
            anchors.top: galleryBar.bottom
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            clip: true
            visible: places.gallery.length > 0
            activeFocusOnTab: true
            library: pinhole.library
            photos: places.gallery.map(p => p.id)
            active: places.visible && photos.activeFocus
            minCell: theme.fontSize * 8
            Accessible.name: galleryBar.visible ? qsTr("%n photo(s) here", "", places.gallery.length) : ""
            onOpened: (photo, index) => places.openGalleryPhoto(index)
        }
    }
    Item {
        anchors.fill: parent
        TapHandler {
            acceptedButtons: Qt.BackButton
            onTapped: places.goBack()
        }
    }

    Command {
        id: fitCommand
        name: "places.fit"
        text: qsTr("Fit all")
        section: qsTr("Map")
        keyLayer: "plugin"
        keys: ["Home"]
        // Home is the gallery's first photo while it has the keys.
        usable: places.visible && places.hasPhotos && !photos.activeFocus
        onTriggered: places.fitAll()
    }
    Command {
        id: areaCommand
        name: "places.area"
        text: qsTr("Photos in view")
        section: qsTr("Map")
        keyLayer: "plugin"
        keys: ["G"]
        usable: places.visible && (places.hasPhotos || places.areaOpen)
        onTriggered: places.showVisible()
    }
    Command {
        id: groupZoom
        name: "places.groupZoom"
        text: qsTr("Zoom to group")
        section: qsTr("Map")
        keyLayer: "plugin"
        keys: ["Z"]
        usable: places.visible && places.zoom < Geo.maxZoom
            && (places.openedGroup !== null || (map.activeFocus && places.currentGroup !== null))
        onTriggered: places.zoomGroup(places.openedGroup || places.currentGroup)
    }
    Repeater {
        model: [["Left", -1, 0], ["Right", 1, 0], ["Up", 0, -1], ["Down", 0, 1]]
        delegate: Item {
            required property var modelData
            Command {
                name: "places.choose." + modelData[0]
                keyLayer: "plugin"
                keys: [modelData[0]]
                // The gallery's own keys while it has them (PhotoTiles).
                usable: places.visible && map.activeFocus
                autoRepeat: true
                onTriggered: places.movePlace(modelData[1], modelData[2])
            }
            Command {
                name: "places.pan." + modelData[0]
                keyLayer: "plugin"
                keys: ["Shift+" + modelData[0]]
                usable: places.visible
                autoRepeat: true
                onTriggered: { gestures.stop(); places.pan(-modelData[1] * 80, -modelData[2] * 80) }
            }
        }
    }
    Command {
        name: "places.open"
        keyLayer: "plugin"
        keys: ["Return", "Enter", "Space"]
        usable: places.visible && map.activeFocus && places.currentGroup !== null
        onTriggered: places.showGroup(places.currentGroup)
    }
    Command {
        id: zoomIn
        name: "places.zoomIn"
        text: qsTr("Zoom in")
        section: qsTr("Map")
        keyLayer: "plugin"
        keys: ["+", "="]
        usable: places.visible && places.hasPhotos && places.zoom < Geo.maxZoom
        autoRepeat: true
        onTriggered: places.stepZoom(1)
    }
    Command {
        id: zoomOut
        name: "places.zoomOut"
        text: qsTr("Zoom out")
        section: qsTr("Map")
        keyLayer: "plugin"
        keys: ["-"]
        usable: places.visible && places.hasPhotos && places.zoom > 0
        autoRepeat: true
        onTriggered: places.stepZoom(-1)
    }
}
