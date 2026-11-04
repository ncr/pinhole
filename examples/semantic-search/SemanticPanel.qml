import QtQuick
import Pinhole.Plugin

Panel {
    id: panel
    property bool shown: false
    property bool returning: false
    property string sourceKey: ""
    property int generation: 0
    property int catalogGeneration: 0
    property var server: null
    property var queryJob: null
    property var catalogJob: null
    property var statusJob: null
    property var tagJob: null
    property string tagAsset: ""
    property var tagSuggestions: []
    property var savedTags: []
    property string tagError: ""
    property bool ready: false
    property bool catalogReady: false
    property var status: ({ indexed: 0, total: 0, running: false, errors: 0 })
    property string failure: ""
    property var results: []
    property int selected: -1
    property var allResults: []
    property bool tagQuery: false
    property string tagName: ""
    readonly property string indexPath: pinhole.cacheDir + "/clip-hybrid-v1.sqlite"
    readonly property string helper: pinhole.dir + "/pinhole-semantic"
    property string checkout: ""
    visible: shown

    function show() {
        shown = true
        returning = false
        if (sourceKey !== pinhole.library.key) reset()
        sourceKey = pinhole.library.key
        if (search.text.trim().startsWith("#")) lookup()
        if (!server) launch()
        Qt.callLater(() => search.input.forceActiveFocus())
    }
    function cancel(job) { if (job) job.cancel() }
    function reset() {
        ++generation
        ++catalogGeneration
        cancel(queryJob); queryJob = null
        cancel(catalogJob); catalogJob = null
        cancel(statusJob); statusJob = null
        closeTags(false)
        cancel(server); server = null
        ready = false; catalogReady = false
        results = []; allResults = []; selected = -1
        tagQuery = false; tagName = ""
        failure = ""
    }
    function close() {
        shown = false; returning = false
        reset()
    }
    function report(error) {
        if (error.code !== "cancelled") {
            failure = error.message || String(error)
            if (error.stdout) {
                try { failure = JSON.parse(error.stdout).error || failure } catch (_) {}
            }
            if (failure.includes("reboot")) failure = qsTr("The NPU stopped responding. Restart the computer and repeat its smoke test before indexing again. Existing results still work.")
            pinhole.warn(error)
        }
    }
    function launch() {
        const current = generation
        const job = pinhole.run(helper, ["config"])
        server = job
        job.then(result => {
            if (server !== job || current !== generation) return
            checkout = JSON.parse(result.stdout).checkout
            launchWorker()
        }).catch(error => {
            if (server !== job || current !== generation) return
            server = null; report(error)
        })
    }
    function launchWorker() {
        const current = generation
        const base = checkout + "/.tools/photo-search/"
        const job = pinhole.run(helper, ["serve", "--index", indexPath,
            "--library", pinhole.library.dir, "--library-key", pinhole.library.key,
            "--models", base + "models", "--model", "clip", "--backend", "hybrid-ane",
            "--cache", base + "resident-tiles", "--compiler", checkout + "/.tools/ane-compiler-package/usr/bin/mil-hwxc",
            "--ane-library", checkout + "/.tools/omarchy-ane/libane/libane.so",
            "--smoke-receipt", base + "ane-smoke.json"], {
            timeout: 0,
            output: line => {
                if (current !== generation) return
                try {
                    const event = JSON.parse(line)
                    if (event.event === "ready") {
                        ready = true
                        status = event.status
                        catalog()
                    } else if (event.event === "error") failure = event.error
                } catch (_) { /* Third-party informational output is not protocol. */ }
            }
        })
        server = job
        job.then(() => {
            if (server !== job) return
            server = null; ready = false; catalogReady = false
        }).catch(error => {
            if (server !== job) return
            server = null; ready = false; catalogReady = false
            report(error)
        })
    }
    function request(message) {
        return pinhole.run(helper, ["request", "--index", indexPath], {
            input: JSON.stringify(message), timeout: 120000
        })
    }
    function unpack(result) {
        const value = JSON.parse(result.stdout)
        if (!value.ok) throw new Error(value.error)
        return value.result
    }
    function catalog() {
        const revision = ++catalogGeneration
        cancel(catalogJob)
        cancel(queryJob); queryJob = null
        closeTags(true)
        results = []; allResults = []; selected = -1
        catalogReady = false
        if (tagQuery) findTags()
        const current = generation
        const job = pinhole.query("SELECT id, thumb_path AS path FROM assets WHERE is_video=0 AND thumb_path IS NOT NULL AND thumb_path<>'' ORDER BY id LIMIT 100001")
        catalogJob = job
        job.then(assets => {
            if (current !== generation || revision !== catalogGeneration || catalogJob !== job) return
            const sync = request({ op: "catalog", assets: assets })
            catalogJob = sync
            return sync.then(result => {
                if (current !== generation || revision !== catalogGeneration || catalogJob !== sync) return
                catalogJob = null; catalogReady = true
                status = unpack(result)
                lookup()
            })
        }).catch(error => {
            if (current !== generation || revision !== catalogGeneration) return
            catalogJob = null
            report(error)
        })
    }
    function lookup() {
        debounce.stop()
        cancel(queryJob); queryJob = null
        const text = search.text.trim()
        results = []; allResults = []; selected = -1
        tagQuery = text.startsWith("#")
        tagName = tagQuery ? text.slice(1).trim().normalize("NFC").toLowerCase() : ""
        if (tagQuery) {
            findTags()
            return
        }
        if (!catalogReady || !text) return
        const current = generation
        const job = request({ op: "query", text: text, limit: 500 })
        queryJob = job
        job.then(result => {
            if (current !== generation || queryJob !== job) return
            queryJob = null
            const value = unpack(result)
            allResults = value.results
            status = value.status
            filterResults()
        }).catch(error => {
            if (current !== generation || queryJob !== job) return
            queryJob = null
            report(error)
        })
    }
    function findTags() {
        const entries = sourceKey ? pinhole.state["tags:" + sourceKey] || {} : {}
        const matches = []
        for (const [key, tags] of Object.entries(entries)) {
            if (!key.startsWith("photo:") || !tags.length) continue
            if (tagName && !tags.some(tag => tag.label.normalize("NFC").toLowerCase() === tagName)) continue
            const id = key.slice(6)
            const position = pinhole.library.indexOf(id)
            if (position >= 0) matches.push({ id: id, position: position })
        }
        matches.sort((a, b) => a.position - b.position)
        allResults = matches
        filterResults()
    }
    function filterResults() {
        const previous = results[selected]?.id
        const visibleResults = allResults.filter(row => {
            const position = pinhole.library.indexOf(row.id)
            return position >= 0 && !pinhole.isHidden(position)
        })
        results = tagQuery ? visibleResults : visibleResults.slice(0, 100)
        const retained = results.findIndex(row => row.id === previous)
        selected = retained >= 0 ? retained : results.length ? 0 : -1
        if (tagAsset) {
            const taggedPosition = pinhole.library.indexOf(tagAsset)
            if (taggedPosition < 0 || pinhole.isHidden(taggedPosition)) closeTags(true)
        }
    }
    function poll() {
        if (!ready || !catalogReady || statusJob) return
        const current = generation
        const job = request({ op: "status" })
        statusJob = job
        job.then(result => {
            if (current !== generation || statusJob !== job) return
            statusJob = null
            const wasRunning = status.running
            status = unpack(result)
            if (status.error) failure = status.error
            if (wasRunning && !status.running) lookup()
        }).catch(error => {
            if (current !== generation || statusJob !== job) return
            statusJob = null; report(error)
        })
    }
    function indexing() {
        failure = ""
        const current = generation
        request({ op: status.running ? "stop" : "start" }).then(result => {
            if (current === generation) status = unpack(result)
        }).catch(error => { if (current === generation) report(error) })
    }
    function importBenchmark() {
        failure = ""
        const current = generation
        const base = checkout + "/.tools/photo-search/"
        request({ op: "import", run: base + "full-v2/clip-hybrid-ane", corpus: base + "icloud-pl-v1/corpus.json" }).then(result => {
            if (current !== generation) return
            status = unpack(result)
            lookup()
        }).catch(error => { if (current === generation) report(error) })
    }
    function openResult(index) {
        if (index < 0 || index >= results.length) return
        const id = results[index].id
        if (pinhole.library.indexOf(id) < 0) return
        selected = index
        shown = false; returning = true
        pinhole.openPhoto(id)
    }

    function openTags() {
        if (!sourceKey || (!tagQuery && !catalogReady) || selected < 0 || selected >= results.length) return
        const asset = results[selected].id
        savedTags = (pinhole.state["tags:" + sourceKey] || {})["photo:" + asset] || []
        tagAsset = asset
        tagError = ""
        if (!catalogReady) return
        const job = request({ op: "suggest_tags", id: asset })
        tagJob = job
        job.then(result => {
            if (tagJob !== job) return
            tagSuggestions = unpack(result)
            tagJob = null
        }).catch(error => {
            if (tagJob !== job) return
            tagJob = null
            if (error.code !== "cancelled") {
                tagError = qsTr("Tag suggestions could not be loaded.")
                pinhole.warn(error)
            }
        })
    }
    function closeTags(restoreFocus) {
        const wasOpen = tagAsset !== ""
        cancel(tagJob); tagJob = null
        tagAsset = ""; tagSuggestions = []; savedTags = []; tagError = ""
        if (wasOpen && restoreFocus) Qt.callLater(() => {
            if (!shown || tagAsset) return
            if (tagButton.visible && tagButton.usable) tagButton.forceActiveFocus()
            else search.input.forceActiveFocus()
        })
    }
    function writeTags(key, asset, value) {
        const entries = Object.assign({}, pinhole.state[key] || {})
        const photo = "photo:" + asset
        if (value.length) entries[photo] = value
        else delete entries[photo]
        pinhole.state[key] = Object.keys(entries).length ? entries : null
        if (key === "tags:" + sourceKey) {
            if (asset === tagAsset) savedTags = value
            if (tagQuery) findTags()
        }
    }
    function toggleTag(tag) {
        if (!tagAsset || !sourceKey || sourceKey !== pinhole.library.key) return
        const key = "tags:" + sourceKey
        const asset = tagAsset
        const before = savedTags
        const removing = before.some(item => item.id === tag.id)
        if (!removing && !before.length && Object.keys(pinhole.state[key] || {}).length >= 2000) {
            tagError = qsTr("This source has reached the limit of 2,000 tagged photos.")
            return
        }
        const after = removing ? before.filter(item => item.id !== tag.id) : before.concat([tag])
        writeTags(key, asset, after)
        pinhole.did(removing ? qsTr("removing a photo tag") : qsTr("adding a photo tag"),
            () => writeTags(key, asset, before), () => writeTags(key, asset, after))
    }
    readonly property bool browsing: shown && tagAsset === ""
    Command { name: "semantic.close"; keys: ["Escape"]; keyLayer: "plugin"; when: panel.shown; onTriggered: panel.tagAsset ? panel.closeTags(true) : panel.close() }
    Command { name: "semantic.open"; keys: ["Return"]; keyLayer: "plugin"; when: panel.browsing && grid.activeFocus; onTriggered: panel.openResult(panel.selected) }
    Command { name: "semantic.down"; keys: ["Down"]; keyLayer: "plugin"; when: panel.browsing && (search.input.activeFocus || grid.activeFocus); onTriggered: panel.move(search.input.activeFocus ? 0 : Math.max(1, Math.round(grid.width / grid.cellWidth))) }
    Command { name: "semantic.up"; keys: ["Up"]; keyLayer: "plugin"; when: panel.browsing && grid.activeFocus; onTriggered: panel.move(-Math.max(1, Math.round(grid.width / grid.cellWidth))) }
    Command { name: "semantic.right"; keys: ["Right"]; keyLayer: "plugin"; when: panel.browsing && grid.activeFocus; onTriggered: panel.move(1) }
    Command { name: "semantic.left"; keys: ["Left"]; keyLayer: "plugin"; when: panel.browsing && grid.activeFocus; onTriggered: panel.move(-1) }
    function move(delta) {
        if (!results.length) return
        selected = Math.max(0, Math.min(results.length - 1, selected + delta))
        grid.positionViewAtIndex(selected, GridView.Contain)
        grid.forceActiveFocus()
    }
    Connections {
        target: search.input
        function onTextChanged() { debounce.restart() }
    }
    Timer { id: debounce; interval: 300; onTriggered: panel.lookup() }
    Timer { interval: 1000; repeat: true; running: panel.shown && panel.ready; onTriggered: panel.poll() }
    Connections {
        target: pinhole.library
        function onChanged() {
            if (panel.sourceKey !== pinhole.library.key) {
                panel.reset(); panel.returning = false
                if (panel.shown) panel.show()
            } else if (panel.ready) panel.catalog()
            else if (panel.tagQuery) panel.findTags()
        }
    }
    Connections {
        target: pinhole
        function onDismissed() { panel.close() }
        function onHiddenChanged() { panel.filterResults() }
        function onCoveredByChanged() {
            if (pinhole.coveredBy && pinhole.coveredBy !== pinhole.id) panel.returning = false
        }
    }
    Connections {
        target: pinhole.viewer
        function onOpenChanged() {
            if (!pinhole.viewer.open && panel.returning) {
                panel.returning = false; panel.shown = true
            }
        }
    }
    Column {
        id: header
        visible: panel.browsing
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 16
        spacing: 12
        Row {
            width: parent.width
            spacing: 12
            FieldInput {
                id: search
                objectName: "semanticSearchInput"
                width: Math.max(80, parent.width - closeButton.width - 12)
                placeholder: qsTr("Describe a photo or enter #tag…")
                input.maximumLength: 2048
                onAccepted: panel.lookup()
            }
            ShellButton { id: closeButton; text: "✕"; hint: "Esc"; onTapped: panel.close() }
        }
        Flow {
            width: parent.width
            spacing: 12
            ShellButton {
                text: panel.status.running ? qsTr("Pause indexing") : qsTr("Index thumbnails on NPU")
                usable: panel.catalogReady && !panel.catalogJob
                onTapped: panel.indexing()
            }
            ShellButton {
                text: qsTr("Use the 384-photo benchmark")
                usable: panel.catalogReady && !panel.status.running
                onTapped: panel.importBenchmark()
            }
            ShellButton {
                id: tagButton
                objectName: "semanticTagButton"
                text: qsTr("Tag selected photo")
                visible: panel.selected >= 0 && panel.sourceKey !== ""
                usable: panel.tagQuery || panel.catalogReady
                onTapped: panel.openTags()
            }
        }
        UiText {
            width: parent.width
            wrapMode: Text.WordWrap
            text: panel.tagQuery ? qsTr("Showing photos with saved tags.")
                : !panel.ready ? qsTr("Starting local search…")
                : !panel.catalogReady ? qsTr("Reading the photo catalog…")
                : qsTr("Indexed: %1 of %2. Results cover indexed photos only.").arg(panel.status.indexed).arg(panel.status.total)
        }
        UiText {
            width: parent.width
            wrapMode: Text.WordWrap
            text: panel.tagQuery ? qsTr("Enter # and the full tag name. Enter # alone to see all tagged photos.")
                : qsTr("Experimental CLIP. Search stays on this computer. Similarity scores are not probabilities.")
            color: theme.darkForeground
        }
        UiText {
            width: parent.width
            visible: !panel.tagQuery && panel.failure !== ""
            wrapMode: Text.WrapAnywhere
            color: theme.red
            text: panel.failure
        }
    }
    ScrollGrid {
        id: grid
        objectName: "semanticSearchResults"
        visible: panel.browsing
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 16
        cellWidth: width / Math.max(1, Math.floor(width / 220))
        cellHeight: cellWidth * .8
        model: panel.results
        delegate: Item {
            id: cell
            required property int index
            required property var modelData
            width: grid.cellWidth
            height: grid.cellHeight
            Image {
                anchors.fill: parent
                anchors.margins: 4
                source: pinhole.library.item(modelData.id).thumbnail || ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                color: "transparent"
                border.width: panel.selected === cell.index ? 2 : 0
                border.color: theme.accent
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { objectName: "semanticResultTap"; onTapped: panel.openResult(cell.index) }
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Search result %1").arg(index + 1)
            Accessible.onPressAction: panel.openResult(index)
        }
    }
    UiText {
        anchors.centerIn: grid
        visible: panel.browsing && (panel.tagQuery || panel.catalogReady) && panel.results.length === 0
        text: panel.tagQuery ? qsTr("No visible photos with this tag.")
            : panel.queryJob ? qsTr("Searching…") : panel.status.indexed === 0
            ? qsTr("Import the benchmark or index local thumbnails to begin.")
            : search.text.trim() ? qsTr("No visible results.") : qsTr("Type a description to search.")
        width: Math.min(grid.width, 500)
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }
    Loader {
        anchors.fill: parent
        active: panel.tagAsset !== ""
        sourceComponent: SemanticTags {
            photo: pinhole.library.item(panel.tagAsset).thumbnail || ""
            pending: panel.tagJob !== null
            suggestions: panel.tagSuggestions
            saved: panel.savedTags
            errorText: panel.tagError
            onClosed: panel.closeTags(true)
            onToggled: tag => panel.toggleTag(tag)
        }
    }
}
