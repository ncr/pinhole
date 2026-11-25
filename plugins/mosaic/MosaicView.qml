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
    readonly property real smallest: 9
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
    // The tiles the atlas shows: the plugin's, from the moment a mosaic of
    // them is laid out and shows (showPlan). Other tiles chosen (R) take the
    // old ones' place in that one frame, the plan and the atlas together.
    property var tiles: plugin.none

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
    // The photos that suit a mosaic (the plugin's `suited`), listed under
    // the picture; ← → go through them, from the first when the picture is
    // none of them. Before any are found: the photo before or after in the
    // library, the grid's hidden ones passed over.
    readonly property var suited: plugin.suited
    readonly property int among: suited.indexOf(photo)
    function step(by) {
        if (suited.length > 0) {
            let k = among < 0 ? (by > 0 ? 0 : suited.length - 1) : among + by
            while (k >= 0 && k < suited.length && !offered(suited[k]))
                k += by
            if (k >= 0 && k < suited.length)
                photo = suited[k]
            return
        }
        let i = position + by
        while (i >= 0 && i < pinhole.library.count && pinhole.isHidden(i))
            i += by
        if (position < 0 || i < 0 || i >= pinhole.library.count)
            return
        photo = pinhole.library.item(i).id
    }
    // In the library shown, and not hidden from its grid.
    function offered(id) {
        const i = pinhole.library.indexOf(id)
        return i >= 0 && !pinhole.isHidden(i)
    }
    onAmongChanged: if (among >= 0) strip.positionViewAtIndex(among, ListView.Contain)
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
        if (!pixels || pixels.of !== photo || !plugin.matcher || plugin.tiles.n === 0)
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
        if (tiles !== made.matcher.set)
            tiles = made.matcher.set
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
        // Other tiles: laid out, and shown once their plan is read.
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

    // Exporting: E opens what to export it as (the card at the foot of this
    // file), kept as the plugin's settings, so the next mosaic is exported
    // the same way, to the same folder; ⏎ there exports, Esc puts the card
    // away. A mosaic can be a picture of hundreds of megapixels: it goes to
    // a folder of the person's choosing, never into the library shown nor
    // onto the clipboard.
    //
    //   pixels   its longer side, in pixels;
    //   print    a paper's size and how many dots an inch: as many pixels
    //            as fit on it, and the file says its dots an inch;
    //   size     no more megabytes than said, as sharp as the atlas has it.
    //
    // A tile is at most `texels` pixels of its photo: a file larger than
    // that a tile is only the same photos drawn bigger.
    property bool exporting: false
    readonly property var papers: ({ "10x15": [15, 10], "13x18": [18, 13], "a4": [29.7, 21], "a3": [42, 29.7],
                                     "30x40": [40, 30], "50x70": [70, 50] })
    readonly property int longest: 12000
    readonly property size exportSize: {
        const shape = columns / rows, by = pinhole.settings.exportBy
        let w, h
        if (by === "print") {
            const paper = papers[pinhole.settings.exportPaper] ?? papers.a4, dots = pinhole.settings.exportDpi / 2.54
            const across = (shape >= 1 ? paper[0] : paper[1]) * dots, down = (shape >= 1 ? paper[1] : paper[0]) * dots
            w = Math.min(across, down * shape)
            h = w / shape
        } else {
            const side = by === "size" ? Math.max(columns, rows) * texels : pinhole.settings.exportPixels
            w = shape >= 1 ? side : side * shape
            h = w / shape
        }
        const most = Math.min(1, longest / Math.max(w, h))
        return Qt.size(Math.round(w * most), Math.round(h * most))
    }
    function back() {
        if (!exporting)
            return false
        exporting = false
        return true
    }
    function askSave() {
        if (ready && !saving)
            exporting = true
    }
    // The folder it goes to: the one chosen last.
    readonly property string folder: pinhole.settings.exportFolder ?? ""
    function chooseFolder(thenExport) {
        pinhole.chooseFolder(qsTr("Export the mosaic to")).then(path => {
            pinhole.settings.exportFolder = path
            if (thenExport)
                mosaic.save()
        }, () => {})
    }
    // The library a folder is in, when a source shown or added is a folder
    // over it: its name, else "".
    function inLibrary(path) {
        const into = path + "/"
        for (const source of pinhole.sources.list)
            if (source.path && (into.startsWith(source.path.replace(/\/$/, "") + "/")))
                return source.name || source.path
        return ""
    }
    // A name of its own there: "<photo> mosaic.jpg", then "… mosaic 2.jpg".
    function freeName() {
        const stem = ((item && item.filename) || qsTr("Photo")).replace(/\.[^.\/]*$/, "").replace(/[\/]/g, "-")
        for (let n = 1; n < 10000; ++n) {
            const path = folder + "/" + qsTr("%1 mosaic").arg(stem) + (n > 1 ? " " + n : "") + ".jpg"
            if (!pinhole.exists(path))
                return path
        }
        return folder + "/" + qsTr("%1 mosaic").arg(stem) + " " + Date.now() + ".jpg"
    }
    property bool saving: false
    property real savingTile: 0
    function failed(error) {
        saving = false
        savingTile = 0
        saveButton.complain(qsTr("Could not export"))
        if (error)
            pinhole.warn(error)
    }
    // Drawn at its size into a file of the cache as it is, which
    // ImageMagick makes the JPEG of: of a quality, or of a size.
    function save() {
        if (saving || !ready)
            return
        if (folder === "" || !pinhole.exists(folder)) {
            chooseFolder(true)
            return
        }
        exporting = false
        saving = true
        if (grabbed > 0) {
            draw()
            return
        }
        // How many pixels a grab makes of a point, asked once of a small
        // one: on a display scaled by a fraction it is not what Screen says.
        const asked = canvas.grabToImage(result => {
            probe.held = result
            probe.source = result.url
        }, Qt.size(64, 64))
        if (!asked)
            failed(null)
    }
    property real grabbed: 0
    Image {
        id: probe
        visible: false
        cache: false
        property var held: null
        onStatusChanged: {
            if (status !== Image.Ready && status !== Image.Error)
                return
            mosaic.grabbed = status === Image.Ready && implicitWidth > 0 ? implicitWidth / 64 : Screen.devicePixelRatio
            held = null
            source = ""
            if (mosaic.saving)
                mosaic.draw()
        }
    }
    // A file of no more megabytes than said comes first from a lower JPEG
    // quality; below `fineQuality` it is made smaller instead, a fifth at a
    // time, until it is that good: a smaller picture of clean tiles rather
    // than a large one of blocks.
    readonly property int fineQuality: 80
    function draw() {
        const wide = exportSize.width, high = exportSize.height
        const by = pinhole.settings.exportBy
        const drawn = pinhole.cacheDir + "/mosaic.bmp", path = freeName()
        savingTile = wide / columns
        // grabToImage takes the size in the window's points; ImageMagick
        // makes it the very size asked for.
        const asked = canvas.grabToImage(result => {
            mosaic.savingTile = 0
            if (!result.saveToFile(drawn))
                mosaic.failed(null)
            else
                mosaic.encode(drawn, path, by, 1)
        }, Qt.size(Math.ceil(wide / grabbed), Math.ceil(high / grabbed)))
        if (!asked)
            failed(null)
    }
    function encode(drawn, path, by, scale) {
        const wide = Math.round(exportSize.width * scale), high = Math.round(exportSize.height * scale)
        const how = [drawn, "-resize", wide + "x" + high + "!"]
        if (by === "print")
            how.push("-units", "PixelsPerInch", "-density", String(pinhole.settings.exportDpi))
        if (by === "size")
            how.push("-define", "jpeg:extent=" + pinhole.settings.exportMb + "MB")
        else
            how.push("-quality", String(pinhole.settings.exportQuality))
        how.push(path)
        const done = () => {
            pinhole.run("rm", ["-f", drawn]).catch(pinhole.warn)
            mosaic.saving = false
            pinhole.notify(qsTr("Mosaic exported to %1: %2 × %3 px, %4").arg(mosaic.folder.split("/").pop())
                           .arg(wide).arg(high).arg(pinhole.sizeText(pinhole.fileSize(path))))
        }
        if (by !== "size") {
            making.start(() => pinhole.run("magick", how, { timeout: 180000 }), done)
            return
        }
        making.start(() => pinhole.run("magick", how, { timeout: 180000 })
                               .then(() => pinhole.run("magick", ["identify", "-format", "%Q", path])), result => {
            const quality = parseInt(result.stdout)
            if (quality < mosaic.fineQuality && scale > 0.2)
                mosaic.encode(drawn, path, by, scale * 0.8)
            else
                done()
        })
    }
    Job { id: making; onFailed: error => mosaic.failed(error) }

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
            text: qsTr("Export…")
            key: "E"
            keyLayer: "plugin"
            usable: mosaic.ready
            busy: mosaic.saving
            onTriggered: mosaic.askSave()
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
        anchors.bottomMargin: 16 + words.height + 12 + (suitedRow.visible ? suitedRow.height + 12 : 0)
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
            level: 8
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
                // How many of its photos have been asked for: 48 more every
                // 40 ms, whatever each of them does (one not in the library
                // asks for nothing).
                property int arrived: 0
                function restart() {
                    arrived = 0
                    asking.restart()
                }
                Timer {
                    id: asking
                    interval: 40
                    repeat: true
                    onTriggered: {
                        photos.arrived += 48
                        if (photos.arrived >= mosaic.tiles.n)
                            stop()
                    }
                }
                width: mosaic.side * mosaic.texels
                height: width
                Repeater {
                    model: mosaic.tiles.ids
                    onModelChanged: photos.restart()
                    delegate: Rectangle {
                        id: slot
                        required property int index
                        required property string modelData
                        // Where it was as the tiles were chosen, while the
                        // library is as it was then: a thousand looked up by
                        // id take a second.
                        readonly property int position: mosaic.plugin.revision === mosaic.tiles.revision ? mosaic.tiles.at[index]
                                                                                                       : pinhole.library.indexOf(modelData)
                        readonly property var item: position >= 0 ? pinhole.library.item(position) : null
                        x: index % mosaic.side * mosaic.texels
                        y: Math.floor(index / mosaic.side) * mosaic.texels
                        width: mosaic.texels
                        height: mosaic.texels
                        color: "#" + ((1 << 24) | mosaic.tiles.rgb[3 * index] << 16 | mosaic.tiles.rgb[3 * index + 1] << 8
                                      | mosaic.tiles.rgb[3 * index + 2]).toString(16).slice(1)
                        Image {
                            anchors.fill: parent
                            // A few dozen more asked for each moment
                            // (photos.arrived): all thousand at once would
                            // keep the next plan waiting behind them.
                            source: slot.item && slot.index < photos.arrived ? slot.item.thumbnail : ""
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

    // The photos that suit a mosaic, best first: a click makes the mosaic
    // of one. Each finds its photo as it comes into view.
    Item {
        id: suitedRow
        visible: mosaic.suited.length > 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: words.top
        anchors.bottomMargin: 12
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        readonly property int cell: Math.round(theme.fontSize * 5.5)
        height: cell
        ScrollList {
            id: strip
            anchors.fill: parent
            orientation: ListView.Horizontal
            clip: true
            spacing: 4
            model: mosaic.suited
            delegate: Item {
                id: offer
                required property string modelData
                readonly property int position: (mosaic.plugin.revision, pinhole.library.indexOf(modelData))
                readonly property var item: position >= 0 && !(mosaic.plugin.hiddenRevision, pinhole.isHidden(position))
                                            ? pinhole.library.item(position) : null
                readonly property bool current: modelData === mosaic.photo
                // One no longer in the library, or hidden from its grid,
                // takes no room.
                visible: item !== null
                width: item !== null ? suitedRow.cell : -strip.spacing
                height: suitedRow.cell
                Rectangle {
                    anchors.fill: parent
                    color: offer.item && offer.item.avgColor ? offer.item.avgColor : theme.darkBackground
                    Image {
                        anchors.fill: parent
                        source: offer.item ? offer.item.thumbnail : ""
                        sourceSize: Qt.size(Math.round(width * Screen.devicePixelRatio), Math.round(height * Screen.devicePixelRatio))
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }
                    Rectangle {
                        anchors.fill: parent
                        visible: offer.current || over.hovered
                        color: "transparent"
                        border.width: offer.current ? 3 : 1
                        border.color: offer.current ? theme.accent : theme.brightForeground
                    }
                }
                HoverHandler { id: over; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: mosaic.photo = offer.modelData }
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
        text: mosaic.plugin.tiles.n === 0 ? qsTr("No photos with a colour to make a mosaic of yet.")
            : !mosaic.ready ? ""
            : qsTr("%1 tiles from %2 photos")
              .arg(Number(mosaic.columns * mosaic.rows).toLocaleString(Qt.locale(), "f", 0))
              .arg(Number(mosaic.laid.used).toLocaleString(Qt.locale(), "f", 0))
              + (mosaic.suited.length > 0 ? "  ·  " + qsTr("under it, the photos that suit a mosaic best")
                 : mosaic.plugin.paused ? "  ·  " + qsTr("background work is paused, so none are suggested")
                 : "  ·  " + qsTr("looking for the photos that suit a mosaic…"))
    }

    // What to save it as.
    Panel {
        visible: mosaic.exporting
        color: Qt.rgba(0, 0, 0, 0.45)
        onTriggered: mosaic.exporting = false
    }
    Card {
        id: card
        visible: mosaic.exporting
        anchors.centerIn: parent
        readonly property int pad: Math.round(theme.fontSize * 1.6)
        width: Math.min(parent.width - 32, Math.max(choices.implicitWidth, theme.fontSize * 30) + 2 * pad)
        height: choices.implicitHeight + 2 * pad
        Column {
            id: choices
            x: card.pad
            y: card.pad
            width: card.width - 2 * card.pad
            spacing: Math.round(theme.fontSize * 0.9)
            readonly property string by: pinhole.settings.exportBy
            UiText {
                text: qsTr("Export the mosaic")
                color: theme.brightForeground
                font.pixelSize: Math.round(theme.fontSize * 1.25)
            }
            Choices {
                setting: "exportBy"
                numbered: false
                options: [{ value: "pixels", text: qsTr("Pixels") }, { value: "print", text: qsTr("For print") },
                          { value: "size", text: qsTr("File size") }]
            }
            Flow {
                width: parent.width
                spacing: 6
                visible: choices.by === "pixels"
                UiText { text: qsTr("Longer side"); color: theme.darkForeground; height: pixelChoices.height; verticalAlignment: Text.AlignVCenter }
                Choices {
                    id: pixelChoices
                    setting: "exportPixels"
                    numbered: false
                    options: [2000, 3000, 4000, 6000, 8000, 12000].map(px => ({ value: px, text: Number(px).toLocaleString(Qt.locale(), "f", 0) }))
                }
            }
            Flow {
                width: parent.width
                spacing: 6
                visible: choices.by === "print"
                UiText { text: qsTr("Paper, cm"); color: theme.darkForeground; height: paperChoices.height; verticalAlignment: Text.AlignVCenter }
                Choices {
                    id: paperChoices
                    setting: "exportPaper"
                    numbered: false
                    options: [{ value: "10x15", text: "10 × 15" }, { value: "13x18", text: "13 × 18" }, { value: "a4", text: "A4" },
                              { value: "a3", text: "A3" }, { value: "30x40", text: "30 × 40" }, { value: "50x70", text: "50 × 70" }]
                }
            }
            Flow {
                width: parent.width
                spacing: 6
                visible: choices.by === "print"
                UiText { text: qsTr("Dots an inch"); color: theme.darkForeground; height: dpiChoices.height; verticalAlignment: Text.AlignVCenter }
                Choices {
                    id: dpiChoices
                    setting: "exportDpi"
                    numbered: false
                    options: [150, 200, 300].map(dpi => ({ value: dpi, text: String(dpi) }))
                }
            }
            Flow {
                width: parent.width
                spacing: 6
                visible: choices.by === "size"
                UiText { text: qsTr("At most, MB"); color: theme.darkForeground; height: mbChoices.height; verticalAlignment: Text.AlignVCenter }
                Choices {
                    id: mbChoices
                    setting: "exportMb"
                    numbered: false
                    options: [1, 2, 5, 10, 20].map(mb => ({ value: mb, text: String(mb) }))
                }
            }
            Flow {
                width: parent.width
                spacing: 6
                visible: choices.by !== "size"
                UiText { text: qsTr("JPEG quality"); color: theme.darkForeground; height: qualityChoices.height; verticalAlignment: Text.AlignVCenter }
                Choices {
                    id: qualityChoices
                    setting: "exportQuality"
                    numbered: false
                    options: [70, 80, 90, 95, 100].map(q => ({ value: q, text: String(q) }))
                }
            }
            // Where it goes.
            Row {
                width: parent.width
                spacing: 8
                UiText {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, parent.width - changeFolder.width - 8)
                    elide: Text.ElideMiddle
                    color: mosaic.folder !== "" ? theme.foreground : theme.darkForeground
                    text: mosaic.folder !== "" ? mosaic.folder.replace(/^\/home\/[^\/]+/, "~") : qsTr("No folder chosen yet")
                }
                ShellButton {
                    id: changeFolder
                    text: mosaic.folder !== "" ? qsTr("Change…") : qsTr("Choose…")
                    onTriggered: mosaic.chooseFolder(false)
                }
            }
            UiText {
                readonly property string library: mosaic.folder !== "" ? mosaic.inLibrary(mosaic.folder) : ""
                visible: library !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.red
                text: qsTr("This folder is in your library %1: the mosaic would show up among your photos.").arg(library)
            }
            // What that comes to, and when it is more than the tiles have.
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                color: theme.darkForeground
                readonly property real each: mosaic.exportSize.width / mosaic.columns
text: (choices.by === "size" ? qsTr("Up to %1 × %2 px: smaller if that is what keeps it sharp in so few megabytes.")
                                              .arg(mosaic.exportSize.width).arg(mosaic.exportSize.height)
                       : qsTr("%1 × %2 px, each tile %3 px").arg(mosaic.exportSize.width).arg(mosaic.exportSize.height).arg(Math.round(each)))
                      + (choices.by !== "size" && each > mosaic.texels * 1.05
                         ? ". " + qsTr("A tile's photo is %1 px: smaller tiles would fill this size sharper.").arg(mosaic.texels) : "")
            }
            Row {
                spacing: 6
                ShellButton {
                    text: qsTr("Export")
                    active: true
                    hint: "⏎"
                    onTriggered: mosaic.save()
                }
                ShellButton {
                    text: qsTr("Cancel")
                    hint: "Esc"
                    onTriggered: mosaic.exporting = false
                }
            }
        }
    }
}
