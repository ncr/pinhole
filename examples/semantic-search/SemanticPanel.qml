import QtQuick
import Pinhole.Plugin

Panel {
    id: panel
    required property Item indexer
    property bool shown: false
    property bool returning: false
    readonly property string sourceKey: indexer.sourceKey
    readonly property bool ready: indexer.ready
    readonly property bool catalogReady: indexer.catalogReady
    readonly property var status: indexer.status
    readonly property string failure: localFailure || indexer.failure
    property string localFailure: ""
    property var queryJob: null
    property var tagJob: null
    property string tagAsset: ""
    property var tagSuggestions: []
    property var savedTags: []
    property string tagError: ""
    property var results: []
    property int selected: -1
    property var allResults: []
    property bool tagQuery: false
    property string tagName: ""
    property bool hadIndex: false
    property bool wasIndexing: false
    property string hoveredAsset: ""
    readonly property string selectedAsset: browsing ? hoveredAsset || results[selected]?.id || "" : ""
    property var previewJob: null
    property var previewTags: []
    readonly property var approvedPreview: selectedAsset
        ? (pinhole.state["tags:" + sourceKey] || {})["photo:" + selectedAsset] || [] : []
    readonly property var suggestedPreview: previewTags.filter(tag => !approvedPreview.some(saved => saved.id === tag.id))
    visible: shown
    color: Qt.rgba(0, 0, 0, 0.36)
    onClicked: close()
    onSelectedAssetChanged: schedulePreview()
    onCatalogReadyChanged: {
        if (shown && catalogReady) {
            lookup()
            schedulePreview()
        }
    }
    onStatusChanged: {
        const refresh = (wasIndexing && !status.running) || (!hadIndex && status.indexed > 0)
        wasIndexing = status.running
        hadIndex = status.indexed > 0
        if (shown && refresh && !tagQuery && !queryJob) lookup()
    }
    onSourceKeyChanged: {
        resetResults()
        returning = false
        if (shown && search.text.trim().startsWith("#")) lookup()
    }

    function show() {
        shown = true
        returning = false
        indexer.ensureStarted()
        lookup()
        Qt.callLater(() => search.input.forceActiveFocus())
    }
    function resetResults() {
        indexer.cancel(queryJob); queryJob = null
        indexer.cancel(previewJob); previewJob = null
        closeTags(false)
        results = []; allResults = []; selected = -1; previewTags = []; hoveredAsset = ""
        tagQuery = false; tagName = ""
        localFailure = ""
    }
    function close() {
        shown = false; returning = false
        debounce.stop()
        previewDelay.stop()
        resetResults()
    }
    function report(error) {
        if (error.code === "cancelled") return
        localFailure = error.message || String(error)
        if (error.stdout) {
            try { localFailure = JSON.parse(error.stdout).error || localFailure } catch (_) {}
        }
        pinhole.warn(error)
    }
    function request(message) { return indexer.request(message) }
    function unpack(result) { return indexer.unpack(result) }
    function schedulePreview() {
        indexer.cancel(previewJob); previewJob = null
        previewTags = []
        previewDelay.stop()
        if (selectedAsset && catalogReady) previewDelay.restart()
    }
    function preview() {
        const asset = selectedAsset
        if (!asset || !catalogReady) return
        const job = request({ op: "suggest_tags", id: asset })
        previewJob = job
        job.then(result => {
            if (previewJob !== job || selectedAsset !== asset) return
            previewJob = null
            previewTags = unpack(result)
        }).catch(() => { if (previewJob === job) previewJob = null })
    }
    function lookup() {
        debounce.stop()
        indexer.cancel(queryJob); queryJob = null
        localFailure = ""
        const text = search.text.trim()
        hoveredAsset = ""
        results = []; allResults = []; selected = -1
        tagQuery = text.startsWith("#")
        tagName = tagQuery ? text.slice(1).trim().normalize("NFC").toLowerCase() : ""
        if (tagQuery) {
            findTags()
            return
        }
        if (!shown || !catalogReady || !text) return
        const job = request({ op: "query", text: text, limit: 500 })
        queryJob = job
        job.then(result => {
            if (queryJob !== job) return
            queryJob = null
            const value = unpack(result)
            allResults = value.results
            indexer.status = value.status
            filterResults()
        }).catch(error => {
            if (queryJob !== job) return
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
    function importBenchmark() {
        const benchmark = indexer.profile?.benchmark
        if (!benchmark) return
        localFailure = ""
        const source = sourceKey
        request({ op: "import", run: benchmark.run, corpus: benchmark.corpus }).then(result => {
            if (source !== sourceKey) return
            indexer.status = unpack(result)
            if (shown) lookup()
        }).catch(error => { if (source === sourceKey && shown) report(error) })
    }
    function openResult(index) {
        if (index < 0 || index >= results.length) return
        const id = results[index].id
        if (pinhole.library.indexOf(id) < 0) return
        selected = index
        returning = true
        pinhole.openPhotoDirect(id)
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
        indexer.cancel(tagJob); tagJob = null
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
    Timer { id: previewDelay; interval: 220; onTriggered: panel.preview() }
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
            if (!panel.returning) return
            panel.shown = !pinhole.viewer.open
            if (panel.shown) {
                panel.returning = false
                grid.forceActiveFocus()
                panel.schedulePreview()
            } else {
                panel.indexer.cancel(panel.previewJob)
                panel.previewJob = null
                previewDelay.stop()
            }
        }
    }
    Card {
        id: card
        objectName: "semanticSearchCard"
        anchors.centerIn: parent
        width: Math.max(0, Math.min(840, parent.width - 24))
        height: Math.max(0, Math.min(630, parent.height - 32))

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
                    width: Math.max(0, parent.width - closeButton.width - parent.spacing)
                    height: closeButton.height
                    placeholder: qsTr("Describe a photo or enter #tag…")
                    input.maximumLength: 2048
                    onAccepted: panel.lookup()
                }
                ShellButton { id: closeButton; objectName: "semanticSearchClose"; text: "✕"; hint: "Esc"; onTapped: panel.close() }
            }
            Flow {
                width: parent.width
                spacing: 12
                ShellButton {
                    text: panel.indexer.paused ? qsTr("Resume indexing")
                        : panel.status.error ? qsTr("Retry indexing") : qsTr("Pause indexing")
                    usable: panel.catalogReady && !panel.indexer.indexingJob
                    onTapped: panel.indexer.toggleIndexing()
                }
                ShellButton {
                    text: qsTr("Use the 384-photo benchmark")
                    visible: Boolean(panel.indexer.profile?.benchmark)
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
                    : panel.status.waiting ? qsTr("Waiting for available GPU memory.")
                    : panel.indexer.profile?.name || ""
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
            anchors.bottom: tagsPreview.visible ? tagsPreview.top : parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 12
            cellWidth: width / Math.max(1, Math.floor(width / 155))
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
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    onHoveredChanged: {
                        if (hovered) panel.hoveredAsset = cell.modelData.id
                        else if (panel.hoveredAsset === cell.modelData.id) panel.hoveredAsset = ""
                    }
                }
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
                ? (panel.indexer.paused ? qsTr("Indexing is paused.") : qsTr("Search will be available as thumbnails are indexed."))
                : search.text.trim() ? qsTr("No visible results.") : qsTr("Type a description to search.")
            width: Math.min(grid.width, 500)
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
        Column {
            id: tagsPreview
            visible: panel.browsing && panel.selectedAsset !== ""
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12
            spacing: 4
            UiText {
                width: parent.width
                elide: Text.ElideRight
                text: panel.approvedPreview.length
                    ? qsTr("Saved tags: %1").arg(panel.approvedPreview.map(tag => tag.label).join(", "))
                    : qsTr("No saved tags")
            }
            UiText {
                width: parent.width
                visible: panel.suggestedPreview.length > 0
                elide: Text.ElideRight
                color: theme.darkForeground
                text: qsTr("Suggested tags: %1").arg(panel.suggestedPreview.map(tag => tag.label).join(", "))
            }
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
}
