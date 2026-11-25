import QtQuick
import QtQuick.Window
import Pinhole.Plugin
import "Mosaic.js" as Mosaic

// A photo as a mosaic of the library's photos, over the window: the picture
// cut into square cells, each a photo of that colour (Mosaic.js), drawn by
// one shader from an atlas of the photos (mosaic.frag), so a tile's size
// changes as fast as a slider is dragged and the picture itself is a key
// away. A tile under the pointer shows its photo larger, and a click opens
// it; ← → make the mosaic of the photo before and after.
Item {
    id: mosaic
    readonly property var plugin: view.plugin
    readonly property bool shown: view.shown

    // The picture, by id, and where it is now.
    property string photo: ""
    readonly property int position: (plugin.revision, photo !== "" ? pinhole.library.indexOf(photo) : -1)
    readonly property var item: position >= 0 ? pinhole.library.item(position) : null
    // The picture itself shown, not its mosaic.
    property bool whole: false
    // How much of it is mosaic, 0 to 1: the tiles grow in (mosaic.frag).
    property real amount: 0
    // A tile's size wanted, in the window's pixels; the columns follow. One
    // of `sizes`, and between two of them while it is on its way: under a
    // pinch, or eased after + and − (zoom, a PinchZoom).
    readonly property real smallest: 5
    readonly property real biggest: 120
    readonly property var sizes: {
        const out = []
        for (let i = 0; i < 24; ++i)
            out.push(smallest * Math.pow(biggest / smallest, i / 23))
        return out
    }
    readonly property real tile: zoom.size * zoom.factor
    // Each tile moved to the picture's colour where it is: a clearer
    // picture, of photos a little less their own.
    property bool tinted: true

    // The atlas: `side` by `side` photos, each `texels` square.
    readonly property int side: 32
    readonly property int texels: 128
    readonly property var tiles: plugin.tiles

    // The picture's pixels once read (sampler), r, g, b: { of, data, width, height }.
    property var pixels: null
    // The mosaic laid out (Mosaic.plan), and for what.
    property var laid: null
    property int columns: 1
    property int rows: 1
    readonly property bool ready: laid !== null && laid.of === photo

    // Opened (Main.qml) on a photo, by its position.
    function start(index) {
        plugin.ensure()
        const id = index >= 0 ? pinhole.library.item(index).id ?? "" : ""
        whole = false
        if (id !== photo) {
            photo = id
        } else if (ready) {
            turnTo(1, 300)
        }
    }
    // The photo before or after, the grid's hidden ones passed over.
    function step(by) {
        let i = position + by
        while (i >= 0 && i < pinhole.library.count && pinhole.isHidden(i))
            i += by
        if (position < 0 || i < 0 || i >= pinhole.library.count)
            return
        photo = pinhole.library.item(i).id
    }
    function turnTo(to, duration) {
        fade.stop()
        fade.to = to
        fade.duration = duration
        fade.start()
    }
    function toggle() {
        whole = !whole
        turnTo(whole ? 0 : 1, 380)
    }
    NumberAnimation { id: fade; target: mosaic; property: "amount"; easing.type: Easing.InOutSine }

    // A size bigger or smaller, eased, as + and − zoom the grid.
    function resize(steps) { zoom.zoomBy(steps) }
    readonly property bool canGrow: zoom.level < sizes.length - 1
    readonly property bool canShrink: zoom.level > 0

    // The picture's shape, and the room it has.
    readonly property real shape: item && item.width > 0 && item.height > 0 ? item.width / item.height : 1.5
    readonly property real roomWidth: Math.max(64, stage.width)
    readonly property real roomHeight: Math.max(64, stage.height)
    readonly property real fitWidth: Math.min(roomWidth, roomHeight * shape)

    // Laid out again: for another picture, another size, other tiles. At
    // most once a frame, however fast the slider moves.
    function lay() {
        if (!pixels || pixels.of !== photo || !plugin.matcher || tiles.n === 0)
            return
        const across = Math.max(6, Math.min(240, Math.round(fitWidth / tile)))
        const down = Math.max(1, Math.min(240, Math.round(across / shape)))
        if (laid && laid.of === photo && laid.columns === across && laid.rows === down && laid.matcher === plugin.matcher)
            return
        const colours = Mosaic.cells(pixels.data, pixels.width, pixels.height, across, down)
        const made = Mosaic.plan(plugin.matcher, colours, across, down)
        made.of = photo
        made.columns = across
        made.rows = down
        made.matcher = plugin.matcher
        // Shown once its picture is read (plans): the next one laid out
        // meanwhile waits for it, the latest only.
        plans.show(made)
    }
    // The plan that shows, and its size, in one frame: the shader reads
    // one by the other.
    function showPlan(made, image) {
        const first = !ready
        columns = made.columns
        rows = made.rows
        canvas.plan = image
        laid = made
        if (first && !whole)
            turnTo(1, 650)
    }
    Timer { id: laying; interval: 0; onTriggered: mosaic.lay() }
    onTileChanged: laying.start()
    onFitWidthChanged: laying.start()
    onPixelsChanged: laying.start()
    Connections {
        target: mosaic.plugin
        function onMatcherChanged() { laying.start() }
    }

    // The tile at a point of the picture, as a number among the tiles, -1
    // for none; and its photo's id.
    function tileAt(x, y) {
        if (!ready || x < 0 || y < 0 || x >= canvas.width || y >= canvas.height)
            return -1
        const c = Math.min(columns - 1, Math.floor(x / canvas.width * columns))
        const r = Math.min(rows - 1, Math.floor(y / canvas.height * rows))
        return laid.which[r * columns + c]
    }
    readonly property int pointed: !whole && pointer.containsMouse ? (laid, tileAt(pointer.mouseX, pointer.mouseY)) : -1
    readonly property string pointedId: pointed >= 0 && pointed < tiles.n ? tiles.ids[pointed] : ""
    function open(id) {
        if (id !== "" && pinhole.library.indexOf(id) >= 0)
            pinhole.openPhotos([id], id, { comeBack: true })
    }
    // ⏎: the photo under the pointer, else the picture itself.
    function openPointed() { open(pointedId !== "" ? pointedId : photo) }

    // The mosaic as a file in Pictures, each tile as sharp as the atlas
    // has it, 6000 pixels across at most (grabToImage takes the size in the
    // window's points); and on the clipboard.
    property bool saving: false
    property real savingTile: 0
    function save() {
        if (saving || !ready)
            return
        const each = Math.max(8, Math.min(texels, Math.floor(6000 / columns)))
        const path = pinhole.picturePath(qsTr("Mosaic"), "jpg")
        saving = true
        savingTile = each
        const done = canvas.grabToImage(result => {
            mosaic.savingTile = 0
            if (!result.saveToFile(path)) {
                mosaic.saving = false
                saveButton.complain(qsTr("Could not save"))
                return
            }
            pinhole.deliver(path).then(how => {
                mosaic.saving = false
                pinhole.notify(how === "copied" ? qsTr("Mosaic saved to Pictures, and copied") : qsTr("Mosaic saved to Pictures"))
            }).catch(pinhole.warn)
        }, Qt.size(Math.round(columns * each / Screen.devicePixelRatio), Math.round(rows * each / Screen.devicePixelRatio)))
        if (!done) {
            saving = false
            savingTile = 0
            saveButton.complain(qsTr("Could not save"))
        }
    }

    // The picture's pixels: its thumbnail drawn small and read back, once
    // for each picture. Under the panel, where it is not seen; a canvas
    // paints only while it shows.
    Canvas {
        id: sampler
        width: 1024
        height: 1024
        renderStrategy: Canvas.Immediate
        renderTarget: Canvas.Image
        // What it is to read: a photo's id and its thumbnail.
        property string of: ""
        property string from: ""
        function read() {
            const wanted = mosaic.thumbnail
            if (mosaic.photo === "" || wanted === "" || (mosaic.pixels && mosaic.pixels.of === mosaic.photo))
                return
            if (from !== "" && from !== wanted)
                unloadImage(from)
            of = mosaic.photo
            from = wanted
            if (isImageLoaded(from)) {
                requestPaint()
            } else if (!isImageLoading(from)) {
                // Asked for afresh: one let go of while it loaded says
                // nothing when asked for again.
                unloadImage(from)
                loadImage(from)
            }
            again.restart()
        }
        // A thumbnail that never said it was in: asked for again.
        Timer { id: again; interval: 1500; onTriggered: sampler.read() }
        onImageLoaded: if (from !== "" && isImageLoaded(from)) requestPaint()
        onPaint: {
            if (of === "" || (mosaic.pixels && mosaic.pixels.of === of) || !isImageLoaded(from) || !mosaic.item)
                return
            // Drawn on a cleared canvas and found again by where it is
            // opaque: how a canvas's points become the pixels read back
            // depends on the display's scale.
            const ctx = getContext("2d")
            const scale = Math.min(1, 320 / Math.max(mosaic.item.width, mosaic.item.height, 1))
            ctx.clearRect(0, 0, width, height)
            ctx.drawImage(from, 0, 0, Math.max(1, Math.round(mosaic.item.width * scale)), Math.max(1, Math.round(mosaic.item.height * scale)))
            const read = ctx.getImageData(0, 0, width, height)
            const source = read.data, across = read.width
            let w = 0, h = 0
            while (w < across && source[w * 4 + 3] > 250)
                ++w
            while (h < read.height && source[h * across * 4 + 3] > 250)
                ++h
            if (w < 2 || h < 2)
                return
            const data = new Uint8Array(w * h * 3)
            for (let y = 0, o = 0; y < h; ++y)
                for (let x = 0, i = y * across * 4; x < w; ++x, i += 4, o += 3) {
                    data[o] = source[i]
                    data[o + 1] = source[i + 1]
                    data[o + 2] = source[i + 2]
                }
            mosaic.pixels = { of: of, data: data, width: w, height: h }
        }
    }
    // Read once the photo's own thumbnail is known: a handler of `photo`
    // may run before `item` follows it.
    readonly property string thumbnail: item ? item.thumbnail ?? "" : ""
    onThumbnailChanged: Qt.callLater(sampler.read)
    onPhotoChanged: {
        // Another picture: its tiles grow in over it, as the first's did.
        fade.stop()
        amount = 0
        sharp = ""
        Qt.callLater(sampler.read)
        Qt.callLater(sharpen)
    }
    onShownChanged: if (shown) Qt.callLater(sampler.read)

    // The picture itself sharper than its thumbnail, once it is here; a
    // photo turned or cut stays its thumbnail, which is drawn so.
    property string sharp: ""
    function sharpen() {
        if (photo === "" || !item || item.video)
            return
        const done = pinhole.adjustment(photo)
        if (done && (done.turn || done.crop))
            return
        const id = photo
        sharpening.start(() => pinhole.fetch(id, "medium"),
                         path => { if (mosaic.photo === id) mosaic.sharp = "image://file/" + encodeURIComponent(path) })
    }
    Job { id: sharpening; onFailed: error => {} }

    // Nothing over it reaches the photos beneath; the wheel makes the tiles
    // bigger and smaller.
    Panel {
        steps: Qt.Vertical
        onStepped: step => mosaic.resize(-step)
    }

    Flow {
        id: top
        x: 16
        y: 16
        width: parent.width - 32 - closeButton.width - 12
        spacing: 6
        ShellButton {
            text: qsTr("Mosaic")
            checkable: true
            active: !mosaic.whole
            hint: mosaic.whole ? "Space" : ""
            onTriggered: if (mosaic.whole) mosaic.toggle()
        }
        ShellButton {
            text: qsTr("Original")
            checkable: true
            active: mosaic.whole
            hint: mosaic.whole ? "" : "Space"
            onTriggered: if (!mosaic.whole) mosaic.toggle()
        }
        Item { width: 10; height: 1 }
        // The text is the key.
        ShellButton {
            text: "−"
            key: "-"
            keyLayer: "plugin"
            hint: ""
            usable: mosaic.canShrink
            onTriggered: mosaic.resize(-1)
        }
        // A tile's size, small to big, evenly by ratio.
        Item {
            id: slider
            width: Math.round(theme.fontSize * 14)
            height: shrinkProbe.height
            readonly property real span: Math.log(mosaic.biggest / mosaic.smallest)
            readonly property real at: Math.max(0, Math.min(1, Math.log(mosaic.tile / mosaic.smallest) / span))
            readonly property real knob: Math.round(theme.fontSize * 1.3)
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: 2
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.25)
                Rectangle {
                    width: slider.knob / 2 + slider.at * (slider.width - slider.knob)
                    height: parent.height
                    radius: 2
                    color: theme.accent
                }
            }
            Rectangle {
                x: slider.at * (slider.width - slider.knob)
                anchors.verticalCenter: parent.verticalCenter
                width: slider.knob
                height: slider.knob
                radius: width / 2
                color: theme.brightForeground
                border.width: 1
                border.color: theme.background
                scale: sliding.pressed ? 1.15 : 1
                Behavior on scale { NumberAnimation { duration: 100 } }
            }
            MouseArea {
                id: sliding
                anchors.fill: parent
                anchors.margins: -6
                cursorShape: Qt.SizeHorCursor
                function set(x) {
                    const t = Math.max(0, Math.min(1, (x - 6 - slider.knob / 2) / (slider.width - slider.knob)))
                    zoom.level = Math.round(t * (mosaic.sizes.length - 1))
                }
                onPressed: mouse => set(mouse.x)
                onPositionChanged: mouse => { if (pressed) set(mouse.x) }
            }
        }
        ShellButton {
            id: shrinkProbe
            text: "+"
            key: "+"
            keyLayer: "plugin"
            hint: ""
            usable: mosaic.canGrow
            onTriggered: mosaic.resize(1)
        }
        Item { width: 10; height: 1 }
        ShellButton {
            text: qsTr("Tint")
            checkable: true
            active: mosaic.tinted
            key: "T"
            keyLayer: "plugin"
            onTriggered: mosaic.tinted = !mosaic.tinted
        }
        ShellButton {
            text: qsTr("Other photos")
            key: "R"
            keyLayer: "plugin"
            onTriggered: mosaic.plugin.shuffle()
        }
        ShellButton {
            id: saveButton
            text: qsTr("Save")
            key: "S"
            keyLayer: "plugin"
            usable: mosaic.ready
            busy: mosaic.saving
            onTriggered: mosaic.save()
        }
    }
    ShellButton {
        id: closeButton
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        text: "✕"
        hint: "Esc"
        onTriggered: mosaic.view.close()
    }

    // The picture's room, under the buttons and over the line of words.
    Item {
        id: stage
        anchors.fill: parent
        anchors.topMargin: top.y + top.height + 16
        anchors.bottomMargin: 16 + words.height + 12
        anchors.leftMargin: 16
        anchors.rightMargin: 16

        // A pinch, and Ctrl with the wheel, make the tiles bigger and
        // smaller as they zoom the grid. What a PinchZoom holds it draws
        // larger while it zooms; here only the tiles grow, so what it
        // holds is drawn as much smaller again.
        PinchZoom {
            id: zoom
            anchors.fill: parent
            levels: mosaic.sizes
            level: 10
            Item {
                id: room
                width: stage.width
                height: stage.height
                transform: Scale {
                    origin.x: zoom.origin.x
                    origin.y: zoom.origin.y
                    xScale: 1 / zoom.factor
                    yScale: 1 / zoom.factor
                }
    // The picture itself, for the shader: never drawn by itself.
            Image {
                id: original
                visible: false
                source: mosaic.sharp !== "" ? mosaic.sharp : mosaic.item ? mosaic.item.thumbnail : ""
                // As large as its room, whatever the tiles' size: asked for once.
                    sourceSize: Qt.size(Math.round(stage.width * Screen.devicePixelRatio), Math.round(stage.height * Screen.devicePixelRatio))
                asynchronous: true
                mipmap: true
                // The thumbnail stays until the sharper one is in.
                retainWhileLoading: true
            }
            // The plan (Mosaic.plan), a pixel a cell, read as it is. A
            // picture given as a data: URL is read a moment later, so there
            // are two: one shows while the other reads the next plan, and
            // they change places once it has.
            Item {
                id: plans
                visible: false
                property var reading: null
                property var waiting: null
                property bool second: false
                function show(made) {
                    if (reading) {
                        waiting = made
                        return
                    }
                    reading = made
                    const image = second ? planA : planB
                    image.source = made.image
                    if (image.status === Image.Ready)
                        read(image)
                }
                function read(image) {
                    if (!reading || image !== (second ? planA : planB) || String(image.source) !== reading.image)
                        return
                    const made = reading
                    reading = null
                    if (image.status === Image.Ready && made.of === mosaic.photo) {
                        second = !second
                        mosaic.showPlan(made, image)
                    }
                    const next = waiting
                    waiting = null
                    if (next)
                        show(next)
                }
                Image {
                    id: planA
                    cache: false
                    smooth: false
                    onStatusChanged: if (status === Image.Ready || status === Image.Error) plans.read(planA)
                }
                Image {
                    id: planB
                    cache: false
                    smooth: false
                    onStatusChanged: if (status === Image.Ready || status === Image.Error) plans.read(planB)
                }
            }
            // The atlas: every tile's photo, its colour until the photo is in,
            // so a mosaic shows at once, in colours, and its photos come into
            // it.
            Item {
                id: photos
                width: mosaic.side * mosaic.texels
                height: width
                Repeater {
                    model: mosaic.tiles.ids
                    delegate: Rectangle {
                        id: slot
                        required property int index
                        required property string modelData
                        readonly property int position: (mosaic.plugin.revision, pinhole.library.indexOf(modelData))
                        readonly property var item: position >= 0 ? pinhole.library.item(position) : null
                        x: index % mosaic.side * mosaic.texels
                        y: Math.floor(index / mosaic.side) * mosaic.texels
                        width: mosaic.texels
                        height: mosaic.texels
                        color: "#" + ((1 << 24) | mosaic.tiles.rgb[3 * index] << 16 | mosaic.tiles.rgb[3 * index + 1] << 8
                                      | mosaic.tiles.rgb[3 * index + 2]).toString(16).slice(1)
                        Image {
                            anchors.fill: parent
                            source: slot.item ? slot.item.thumbnail : ""
                            sourceSize: Qt.size(mosaic.texels, mosaic.texels)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            opacity: status === Image.Ready ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 250 } }
                        }
                    }
                }
            }
            ShaderEffectSource {
                id: atlasSource
                visible: false
                sourceItem: photos
                hideSource: true
                textureSize: Qt.size(photos.width, photos.height)
                mipmap: true
                smooth: true
            }

            ShaderEffect {
                id: canvas
                // Whole tiles, in the middle of the room.
                readonly property real each: Math.min(mosaic.roomWidth / mosaic.columns, mosaic.roomHeight / mosaic.rows)
                width: mosaic.columns * each
                height: mosaic.rows * each
                anchors.centerIn: parent
                visible: mosaic.photo !== "" && (original.status === Image.Ready || mosaic.ready)
                readonly property real drawn: mosaic.savingTile > 0 ? mosaic.savingTile : each * Screen.devicePixelRatio

                property var atlas: atlasSource
                property var plan: planA
                property var picture: original
                property point cells: Qt.point(mosaic.columns, mosaic.rows)
                property real amount: mosaic.ready ? (mosaic.savingTile > 0 ? 1 : mosaic.amount) : 0
                property real tint: mosaic.tinted ? 0.55 : 0
                Behavior on tint { NumberAnimation { duration: 200 } }
                property real lod: Math.max(0, Math.min(6, Math.log(mosaic.texels / Math.max(1, drawn)) / Math.LN2))
                property real side: mosaic.side
                property real texels: mosaic.texels
                // A dark line between tiles big enough to be looked at as photos.
                property real seam: drawn >= 28 ? 1.2 / drawn : 0
                fragmentShader: "mosaic.frag.qsb"

                MouseArea {
                    id: pointer
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: mosaic.pointed >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: mouse => {
                        const t = mosaic.whole ? -1 : mosaic.tileAt(mouse.x, mouse.y)
                        if (t >= 0)
                            mosaic.open(mosaic.tiles.ids[t])
                    }
                }
                // The tile under the pointer, ringed, and its photo beside it,
                // larger than tiles mostly are.
                Rectangle {
                    visible: mosaic.pointed >= 0
                    x: Math.floor(pointer.mouseX / canvas.each) * canvas.each - 1
                    y: Math.floor(pointer.mouseY / canvas.each) * canvas.each - 1
                    width: canvas.each + 2
                    height: canvas.each + 2
                    color: "transparent"
                    border.width: 2
                    border.color: theme.brightForeground
                }
            }
            Rectangle {
                id: loupe
                readonly property real size: Math.round(theme.fontSize * 15)
                readonly property int at: mosaic.pointedId !== "" ? (mosaic.plugin.revision, pinhole.library.indexOf(mosaic.pointedId)) : -1
                readonly property var item: at >= 0 ? pinhole.library.item(at) : null
                // Beside the pointer, on the side with room for it.
                readonly property point from: canvas.mapToItem(room, pointer.mouseX, pointer.mouseY)
                visible: item !== null && canvas.each < size * 0.6
                x: from.x + 24 + width > stage.width ? from.x - 24 - width : from.x + 24
                y: Math.max(0, Math.min(stage.height - height, from.y - height / 2))
                width: size
                height: size
                color: item && item.avgColor ? item.avgColor : theme.darkBackground
                border.width: 2
                border.color: theme.brightForeground
                Image {
                    anchors.fill: parent
                    anchors.margins: 2
                    source: loupe.item ? loupe.item.thumbnail : ""
                    sourceSize: Qt.size(Math.round(width * Screen.devicePixelRatio), Math.round(height * Screen.devicePixelRatio))
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
            }
            Spinner {
                anchors.centerIn: parent
                visible: mosaic.photo !== "" && !canvas.visible
            }
            }
        }
    }

    // What it is made of.
    UiText {
        id: words
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        width: Math.min(implicitWidth, parent.width - 32)
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        color: theme.darkForeground
        text: mosaic.tiles.n === 0 ? qsTr("No photos with a colour to make a mosaic of yet.")
            : !mosaic.ready ? ""
            : qsTr("%1 tiles from %2 photos")
              .arg(Number(mosaic.columns * mosaic.rows).toLocaleString(Qt.locale(), "f", 0))
              .arg(Number(mosaic.laid.used).toLocaleString(Qt.locale(), "f", 0))
    }
}
