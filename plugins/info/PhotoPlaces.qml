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
    property string selectedPlace: ""
    property bool loading: false
    property bool fitted: false
    property bool failed: false
    property int cursor: 0
    property real cx: 0.5
    property real cy: 0.5
    property real zoom: 1
    readonly property real scale: Geo.scale(zoom)
    readonly property int columns: Math.max(1, Math.floor(photos.width / (theme.fontSize * 8)))
    readonly property bool hasPhotos: points.length > 0 || focusPoint !== null

    function start(focus) {
        focusPoint = focus
        if (focus) {
            camera(focus.x, focus.y, 14)
            fitted = true
        }
        forceActiveFocus()
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
            points = result
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
    function mappedPoints() {
        if (!focusPoint)
            return points
        // Info can have EXIF coordinates for a source that supplies no pair.
        const at = points.findIndex(p => p.id === focusPoint.id)
        if (at < 0)
            return points.concat([focusPoint])
        return points
    }
    function camera(x, y, level) {
        cx = Geo.wrap(x)
        zoom = Geo.clamp(level, 0, Geo.maxZoom)
        const half = Math.min(0.5, map.height / (2 * scale))
        cy = Geo.clamp(y, half, 1 - half)
        settle.restart()
    }
    function fitAll() {
        const view = Geo.fit(mappedPoints(), map.width, map.height)
        fitted = hasPhotos
        camera(view.x, view.y, view.zoom)
    }
    function zoomAt(x, y, level) {
        const before = scale
        const after = Geo.scale(Geo.clamp(level, 0, Geo.maxZoom))
        camera(cx + (x - map.width / 2) * (1 / before - 1 / after),
               cy + (y - map.height / 2) * (1 / before - 1 / after), level)
    }
    function stepZoom(amount) {
        zoomAt(map.width / 2, map.height / 2, zoom + amount)
    }
    function pan(dx, dy) {
        camera(cx - dx / scale, cy - dy / scale, zoom)
    }
    function regroup() {
        groups = Geo.clusters(mappedPoints(), cx, cy, zoom, map.width, map.height)
    }
    function showGroup(group) {
        if (group.points.length === 1)
            owner.viewMapPhoto(group.points[0].id)
        else {
            showPhotos(group.points)
            selectedPlace = group.cover.id
            camera(group.x, group.y, zoom)
        }
    }
    function zoomGroup(group) {
        const view = Geo.fit(group.points, map.width, map.height)
        camera(view.x, view.y, Math.max(zoom + 2, view.zoom))
    }
    function showPhotos(photosInPlace) {
        selectedPlace = ""
        gallery = photosInPlace.slice().sort((a, b) => b.date - a.date)
        cursor = 0
        photos.positionViewAtBeginning()
        if (gallery.length)
            photos.forceActiveFocus()
    }
    function showVisible() {
        regroup()
        const found = []
        for (const g of groups)
            for (const p of g.points)
                if (Math.abs(Geo.delta(p.x, cx) * scale) <= map.width / 2
                        && Math.abs((p.y - cy) * scale) <= map.height / 2)
                    found.push(p)
        showPhotos(found)
    }
    function hidePhotos() {
        selectedPlace = ""
        gallery = []
        forceActiveFocus()
    }
    function goBack() {
        if (gallery.length)
            hidePhotos()
        else
            owner.closeMap()
    }
    onVisibleChanged: {
        if (visible) {
            if (gallery.length)
                photos.forceActiveFocus()
            else
                forceActiveFocus()
        }
    }
    function moveCursor(by) {
        cursor = Geo.clamp(cursor + by, 0, gallery.length - 1)
        photos.positionViewAtIndex(cursor, GridView.Contain)
    }

    Timer { id: settle; interval: 55; onTriggered: places.regroup() }
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
        height: close.height + 20
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
                text: titlebar.compact ? qsTr("Fit") : qsTr("Fit all")
                helpText: qsTr("Fit all")
                enabled: places.hasPhotos
                onTapped: places.fitAll()
            }
            ShellButton {
                text: titlebar.compact ? qsTr("Photos") : qsTr("Photos in view")
                helpText: qsTr("Photos in view")
                enabled: places.groups.length > 0
                onTapped: places.showVisible()
            }
            ShellButton {
                id: close
                text: "✕"
                onTapped: places.owner.closeMap()
            }
        }
    }

    Rectangle {
        id: map
        objectName: "placesViewport"
        anchors.top: titlebar.bottom
        anchors.bottom: galleryPanel.top
        width: parent.width
        color: Qt.tint(theme.background, Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.08))
        clip: true
        onWidthChanged: settle.restart()
        onHeightChanged: places.camera(places.cx, places.cy, places.zoom)

        MapTiles {
            id: tiles
            anchors.fill: parent
            visible: places.hasPhotos && places.visible
            centerX: places.cx
            centerY: places.cy
            zoom: places.zoom
        }
        MapGestures {
            anchors.fill: parent
            onPan: (dx, dy) => places.pan(dx, dy)
            onZoomAt: (x, y, amount) => places.zoomAt(x, y, places.zoom + amount)
        }

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
                x: map.width / 2 + Geo.delta(modelData.x, places.cx) * places.scale - width / 2
                y: map.height / 2 + (modelData.y - places.cy) * places.scale - height / 2
                width: 56
                height: 56
                visible: x + width > 0 && x < map.width && y + height > 0 && y < map.height
                z: highlighted || activeFocus || hover.hovered ? 2 : 0
                activeFocusOnTab: visible
                Accessible.role: Accessible.Button
                Accessible.name: count > 1 ? qsTr("%n photo(s) here", "", count)
                                           : (photo.filename || qsTr("Open the photo"))
                Accessible.onPressAction: places.showGroup(modelData)
                Keys.onReturnPressed: places.showGroup(modelData)
                Keys.onEnterPressed: places.showGroup(modelData)
                Keys.onSpacePressed: places.showGroup(modelData)
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
                    color: marker.highlighted || marker.activeFocus || hover.hovered ? theme.accent : theme.brightForeground
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
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    exclusiveSignals: TapHandler.SingleTap | TapHandler.DoubleTap
                    onSingleTapped: places.showGroup(marker.modelData)
                    onDoubleTapped: places.zoomGroup(marker.modelData)
                }
            }
        }

        Column {
            anchors.right: parent.right
            anchors.bottom: mapFooter.top
            anchors.margins: 10
            spacing: 4
            ShellButton {
                text: "+"
                helpText: qsTr("Zoom in")
                width: minus.width
                floating: true
                onTapped: places.stepZoom(1)
            }
            ShellButton {
                id: minus
                text: "−"
                helpText: qsTr("Zoom out")
                floating: true
                onTapped: places.stepZoom(-1)
            }
        }

        Row {
            id: mapFooter
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 8
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
        width: parent.width
        height: visible ? Math.min(places.height * 0.44, theme.fontSize * 25) : 0
        visible: places.gallery.length > 0
        anchors.bottom: parent.bottom
        color: theme.background
        Item {
            id: galleryBar
            width: parent.width
            height: dismissGallery.height + 12
            UiText {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                color: theme.brightForeground
                text: qsTr("%n photo(s) here", "", places.gallery.length)
            }
            ShellButton {
                id: dismissGallery
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Hide photos")
                onTapped: places.hidePhotos()
            }
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
            activeFocusOnTab: true
            cellWidth: width / places.columns
            cellHeight: cellWidth
            delegate: Item {
                id: thumb
                required property int index
                readonly property string photoId: places.gallery[index]?.id ?? ""
                readonly property var photo: (places.pinhole.library.revision, places.pinhole.library.item(photoId))
                width: photos.cellWidth - 5
                height: photos.cellHeight - 5
                Accessible.role: Accessible.Button
                Accessible.name: photo.filename || qsTr("Open the photo")
                Accessible.onPressAction: places.owner.viewMapPhoto(photoId)
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
                    border.width: 2
                    border.color: theme.accent
                    visible: keymap.input === "keyboard" && places.cursor === thumb.index
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: {
                    places.cursor = thumb.index
                    places.owner.viewMapPhoto(thumb.photoId)
                } }
            }
        }
    }
    Command {
        name: "places.close"
        keyLayer: "plugin"
        keys: ["Escape"]
        when: places.visible
        onTriggered: places.goBack()
    }
    Repeater {
        model: [
            ["Left", 1, 0], ["Right", -1, 0], ["Up", 0, 1], ["Down", 0, -1]
        ]
        delegate: Command {
            required property var modelData
            name: "places." + modelData[0]
            keyLayer: "plugin"
            keys: [modelData[0]]
            when: places.visible
            autoRepeat: true
            onTriggered: photos.activeFocus
                ? places.moveCursor(-modelData[1] - places.columns * modelData[2])
                : places.pan(modelData[1] * 80, modelData[2] * 80)
        }
    }
    Command {
        name: "places.open"
        keyLayer: "plugin"
        keys: ["Return", "Enter"]
        when: places.visible && places.gallery.length > 0 && photos.activeFocus
        onTriggered: places.owner.viewMapPhoto(places.gallery[places.cursor]?.id ?? "")
    }
    Command {
        name: "places.zoomIn"
        keyLayer: "plugin"
        keys: ["+", "="]
        when: places.visible
        autoRepeat: true
        onTriggered: places.stepZoom(1)
    }
    Command {
        name: "places.zoomOut"
        keyLayer: "plugin"
        keys: ["-"]
        when: places.visible
        autoRepeat: true
        onTriggered: places.stepZoom(-1)
    }
}
