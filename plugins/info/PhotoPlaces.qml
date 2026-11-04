import QtQuick
import Pinhole.Plugin
import Pinhole.Internal
import "Places.js" as Geo

FocusScope {
    id: places
    objectName: "photoPlaces"
    required property var owner
    readonly property var pinhole: owner.pinhole
    property var points: []
    property var groups: []
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
    property int cursor: 0
    property real cx: 0.5
    property real cy: 0.5
    property real zoom: 1
    readonly property real scale: Geo.scale(zoom)
    readonly property int columns: Math.max(1, Math.floor(photos.width / (theme.fontSize * 8)))
    readonly property var availablePoints: {
        // Info can have EXIF coordinates for a source that supplies no pair.
        if (focusPoint && !points.some(p => p.id === focusPoint.id))
            return points.concat([focusPoint]).sort(byDate)
        return points
    }
    readonly property bool hasPhotos: availablePoints.length > 0

    function start(focus) {
        focusPoint = focus
        if (focus) {
            camera(focus.x, focus.y, 14)
            fitted = true
        }
        map.forceActiveFocus()
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
            failed = error.code !== "cancelled"
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
        const group = map.activeFocus && keymap.input === "keyboard" ? currentGroup : null
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
        const next = Geo.clusters(availablePoints, viewCenterX, viewCenterY, zoom, viewWidth, viewHeight)
        if (!Geo.sameGroups(groups, next))
            groups = next
        if (!currentGroup || !isInView(currentGroup))
            choosePlace(nearestPlace())
        if (areaOpen)
            setGallery(availablePoints.filter(isInView), true)
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
            owner.viewMapPhoto(group.points[0].id)
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
        choosePlace(group)
        centerOn(view.x, view.y, Math.min(Geo.maxZoom, zoom + 3, Math.max(zoom + 1, view.zoom)))
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
            photos.positionViewAtBeginning()
    }
    function showVisible() {
        if (areaOpen) {
            hidePhotos()
            return
        }
        openedGroup = null
        areaOpen = true
        setGallery(availablePoints.filter(isInView), false)
        if (keymap.input === "keyboard" && gallery.length)
            photos.forceActiveFocus()
    }
    function hidePhotos() {
        openedGroup = null
        areaOpen = false
        gallery = []
        map.forceActiveFocus()
    }
    function goBack() {
        if (galleryOpen)
            hidePhotos()
        else
            owner.closeMap()
    }
    function openGalleryPhoto(at) {
        const photo = gallery[at]
        if (photo) {
            cursor = at
            returnToGallery = true
            owner.viewMapPhoto(photo.id)
        }
    }
    property bool returnToGallery: false
    onVisibleChanged: {
        if (visible) {
            if (returnToGallery && gallery.length)
                photos.forceActiveFocus()
            else
                map.forceActiveFocus()
            returnToGallery = false
        }
    }
    function moveCursor(by) {
        cursor = Geo.clamp(cursor + by, 0, gallery.length - 1)
        photos.positionViewAtIndex(cursor, GridView.Contain)
    }
    onViewWidthChanged: scheduleGroups()
    onViewHeightChanged: camera(cx, cy, zoom)
    Timer { id: settle; interval: 65; onTriggered: places.regroup() }
    Timer { id: reload; interval: 300; onTriggered: places.refresh() }
    Connections {
        target: places.pinhole.library
        function onChanged() { reload.restart() }
    }
    Component.onDestruction: { if (pending) pending.cancel() }
    Panel {}

    Item {
        id: titlebar
        width: parent.width
        readonly property bool compact: width < theme.fontSize * 40
        height: close.height + 16
        UiText {
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.right: toolbar.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Map")
            font.pixelSize: Math.round(theme.fontSize * 1.35)
            color: theme.brightForeground
            elide: Text.ElideRight
        }
        Row {
            id: toolbar
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            ShellButton {
                objectName: "placesFit"
                text: titlebar.compact ? qsTr("Fit") : qsTr("Fit all")
                helpText: qsTr("Fit all")
                action: fitCommand
                hint: titlebar.compact ? "" : fitCommand.keyName
            }
            ShellButton {
                objectName: "placesArea"
                text: titlebar.compact ? qsTr("Photos") : qsTr("Photos in view")
                helpText: qsTr("Photos in view")
                action: areaCommand
                active: places.areaOpen
                hint: titlebar.compact ? "" : areaCommand.keyName
            }
            ShellButton {
                id: close
                objectName: "placesClose"
                text: "✕"
                action: closeCommand
                hint: ""
            }
        }
    }

    Item {
        id: hintRow
        anchors.top: titlebar.bottom
        width: parent.width
        height: visible ? Math.ceil(theme.fontSize * 1.8) : 0
        visible: places.hasPhotos && width >= theme.fontSize * 32
        UiText {
            id: inputHint
            x: 14
            width: Math.max(0, parent.width - 28)
            elide: Text.ElideRight
            color: theme.foreground
            text: {
                if (keymap.input === "keyboard")
                    return photos.activeFocus
                        ? qsTr("Arrows browse photos · ⏎ opens · Esc goes back")
                        : qsTr("Arrows choose a place · ⇧ + arrows move · ⏎ opens")
                if (keymap.input === "touchpad")
                    return qsTr("Two fingers move · Pinch to zoom")
                return qsTr("Drag to move · Scroll to zoom")
            }
        }
    }

    Rectangle {
        id: map
        objectName: "placesViewport"
        anchors.top: hintRow.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        activeFocusOnTab: true
        Accessible.role: Accessible.Pane
        Accessible.name: places.currentGroup
            ? qsTr("%n photo(s) here", "", places.currentGroup.points.length) : qsTr("Map")
        Accessible.description: inputHint.text
        color: Qt.tint(theme.background, Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.08))
        clip: true

        MapTiles {
            id: tiles
            width: places.viewWidth
            height: places.viewHeight
            visible: places.hasPhotos && places.visible
            centerX: Geo.wrap(places.viewCenterX)
            centerY: places.viewCenterY
            zoom: places.zoom
        }
        MapWheel {
            width: places.viewWidth
            height: places.viewHeight
            enabled: keymap.layer === "plugin" && keymap.plugin === places.pinhole.id
            onPan: (dx, dy) => {
                map.forceActiveFocus()
                places.pan(dx, dy)
            }
            onZoomAt: (x, y, amount) => {
                map.forceActiveFocus()
                places.zoomAt(x, y, places.zoom + amount)
            }
        }
        MapGestures {
            anchors.fill: parent
            onPan: (dx, dy) => places.pan(dx, dy)
            onZoomAt: (x, y, amount) => places.zoomAt(x, y, places.zoom + amount)
            onBackgroundTapped: map.forceActiveFocus()
            onDragStarted: map.forceActiveFocus()

            Repeater {
                model: places.groups
                delegate: Item {
                    id: marker
                    objectName: "placesMarker"
                    required property var modelData
                    readonly property int count: modelData.points.length
                    readonly property bool focusedPhoto: places.focusPoint !== null
                        && modelData.points.some(p => p.id === places.focusPoint.id)
                    readonly property bool highlighted: focusedPhoto || (places.selectedPlace !== ""
                        && modelData.points.some(p => p.id === places.selectedPlace))
                    readonly property var photo: (places.pinhole.library.revision,
                        places.pinhole.library.item(focusedPhoto ? places.focusPoint.id : modelData.cover.id))
                    readonly property bool keyboardFocus: map.activeFocus && keymap.input === "keyboard"
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
                    Rectangle {
                        visible: marker.count > 1
                        x: 5; y: -4
                        width: parent.width; height: parent.height
                        radius: 7
                        rotation: 8
                        color: theme.background
                        border.color: theme.foreground
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: marker.highlighted || hover.hovered ? theme.accent : theme.brightForeground
                        Image {
                            anchors.fill: parent
                            anchors.margins: 3
                            source: marker.photo.thumbnail || ""
                            sourceSize: Qt.size(96, 96)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                    }
                    Rectangle {
                        visible: marker.count > 1
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.margins: -4
                        width: Math.max(21, badge.implicitWidth + 12)
                        height: 20
                        radius: 10
                        color: theme.background
                        border.color: theme.foreground
                        UiText {
                            id: badge
                            anchors.centerIn: parent
                            text: marker.count.toLocaleString(Qt.locale(), "f", 0)
                            color: theme.brightForeground
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -4
                        radius: 9
                        color: "transparent"
                        border.width: 2
                        border.color: theme.accent
                        visible: marker.keyboardFocus
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
                onTapped: tiles.retry()
            }
            ShellButton {
                text: qsTr("© OpenStreetMap")
                helpText: qsTr("OpenStreetMap attribution")
                floating: true
                visible: tiles.visible
                onTapped: Qt.openUrlExternally("https://www.openstreetmap.org/copyright")
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
            UiText {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.right: galleryControls.left
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                color: theme.brightForeground
                elide: Text.ElideRight
                text: qsTr("%n photo(s) here", "", places.gallery.length)
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
                    onTapped: places.hidePhotos()
                }
            }
        }
        UiText {
            anchors.centerIn: parent
            visible: places.areaOpen && places.gallery.length === 0
            text: qsTr("No photos in this area")
            color: theme.foreground
        }
        ScrollGrid {
            id: photos
            objectName: "placesGallery"
            anchors.top: galleryBar.bottom
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            clip: true
            model: places.gallery.length
            visible: count > 0
            activeFocusOnTab: true
            keyNavigationEnabled: false
            Accessible.role: Accessible.List
            Accessible.name: galleryBar.visible ? qsTr("%n photo(s) here", "", count) : ""
            cellWidth: width / places.columns
            cellHeight: cellWidth
            delegate: Item {
                id: thumb
                objectName: "placesPhoto_" + index
                required property int index
                readonly property string photoId: places.gallery[index]?.id ?? ""
                readonly property var photo: (places.pinhole.library.revision, places.pinhole.library.item(photoId))
                width: photos.cellWidth - 5
                height: photos.cellHeight - 5
                Accessible.role: Accessible.Button
                Accessible.name: photo.filename || qsTr("Open the photo")
                Accessible.onPressAction: places.openGalleryPhoto(index)
                Image {
                    anchors.fill: parent
                    source: thumb.photo.thumbnail || ""
                    sourceSize: Qt.size(width * 1.5, height * 1.5)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: hover.hovered ? 1 : 2
                    border.color: theme.accent
                    visible: hover.hovered || (photos.activeFocus && keymap.input === "keyboard" && places.cursor === thumb.index)
                }
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: places.openGalleryPhoto(thumb.index) }
            }
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
        id: closeCommand
        name: "places.close"
        keyLayer: "plugin"
        keys: ["M"]
        when: places.visible
        onTriggered: places.owner.closeMap()
    }
    Command {
        name: "places.back"
        keyLayer: "plugin"
        keys: ["Escape"]
        when: places.visible
        onTriggered: places.goBack()
    }
    Command {
        id: fitCommand
        name: "places.fit"
        text: qsTr("Fit all")
        section: qsTr("Map")
        keyLayer: "plugin"
        keys: ["Home"]
        when: places.visible && places.hasPhotos
        onTriggered: places.fitAll()
    }
    Command {
        id: areaCommand
        name: "places.area"
        text: qsTr("Photos in view")
        section: qsTr("Map")
        keyLayer: "plugin"
        keys: ["G"]
        when: places.visible && (places.hasPhotos || places.areaOpen)
        onTriggered: places.showVisible()
    }
    Command {
        id: groupZoom
        name: "places.groupZoom"
        text: qsTr("Zoom to group")
        section: qsTr("Map")
        keyLayer: "plugin"
        keys: ["Z"]
        when: places.visible && places.zoom < Geo.maxZoom
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
                when: places.visible && (map.activeFocus || photos.activeFocus)
                autoRepeat: true
                onTriggered: photos.activeFocus
                    ? places.moveCursor(modelData[1] + places.columns * modelData[2])
                    : places.movePlace(modelData[1], modelData[2])
            }
            Command {
                name: "places.pan." + modelData[0]
                keyLayer: "plugin"
                keys: ["Shift+" + modelData[0]]
                when: places.visible
                autoRepeat: true
                onTriggered: places.pan(-modelData[1] * 80, -modelData[2] * 80)
            }
        }
    }
    Command {
        name: "places.open"
        keyLayer: "plugin"
        keys: ["Return", "Enter", "Space"]
        when: places.visible && (photos.activeFocus && places.gallery.length > 0
                                  || map.activeFocus && places.currentGroup !== null)
        onTriggered: {
            if (photos.activeFocus)
                places.openGalleryPhoto(places.cursor)
            else
                places.showGroup(places.currentGroup)
        }
    }
    Command {
        id: zoomIn
        name: "places.zoomIn"
        text: qsTr("Zoom in")
        section: qsTr("Map")
        keyLayer: "plugin"
        keys: ["+", "="]
        when: places.visible && places.hasPhotos && places.zoom < Geo.maxZoom
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
        when: places.visible && places.hasPhotos && places.zoom > 0
        autoRepeat: true
        onTriggered: places.stepZoom(-1)
    }
}
