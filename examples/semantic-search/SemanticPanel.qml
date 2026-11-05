import QtQuick
import Pinhole.Plugin

// The whole window, as the map is: the question at the top, the photos
// that answer it below, best first, and ⏎ opens the one chosen with ← →
// going through the answers alone.
Panel {
    id: panel
    required property Item indexer
    property bool shown: false
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
    property alias selected: grid.cursor
    property var allResults: []
    property bool tagQuery: false
    property string tagName: ""
    property bool hadIndex: false
    property bool wasIndexing: false
    // ⏎ in the field before the answer came: open the best one once it does.
    property bool openWhenReady: false
    readonly property string query: search.text.trim()
    readonly property bool browsing: shown && tagAsset === ""
    readonly property var tagLabels: {
        const entries = sourceKey ? pinhole.state["tags:" + sourceKey] || {} : {}
        const found = new Map()
        for (const [key, tags] of Object.entries(entries))
            if (key.startsWith("photo:"))
                for (const tag of tags) found.set(tag.label, (found.get(tag.label) || 0) + 1)
        return Array.from(found.entries()).sort((a, b) => b[1] - a[1]).slice(0, 12).map(entry => entry[0])
    }
    visible: shown && !pinhole.away
    onCatalogReadyChanged: if (shown && catalogReady) lookup()
    onStatusChanged: {
        const refresh = (wasIndexing && !status.running) || (!hadIndex && status.indexed > 0)
        wasIndexing = status.running
        hadIndex = status.indexed > 0
        if (shown && refresh && !tagQuery && !queryJob) lookup()
    }
    onSourceKeyChanged: {
        resetResults()
        if (shown && query.startsWith("#")) lookup()
    }

    function show() {
        shown = true
        indexer.ensureStarted()
        lookup()
        Qt.callLater(() => { search.input.forceActiveFocus(); search.input.selectAll() })
    }
    function resetResults() {
        indexer.cancel(queryJob); queryJob = null
        closeTags(false)
        results = []; allResults = []; selected = -1
        tagQuery = false; tagName = ""
        localFailure = ""
        openWhenReady = false
    }
    function close() {
        shown = false
        debounce.stop()
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
    function ask(text) {
        search.text = text
        search.input.cursorPosition = text.length
        search.input.forceActiveFocus()
        lookup()
    }
    function lookup() {
        debounce.stop()
        indexer.cancel(queryJob); queryJob = null
        localFailure = ""
        const text = query
        results = []; allResults = []; selected = -1
        tagQuery = text.startsWith("#")
        tagName = tagQuery ? text.slice(1).trim().normalize("NFC").toLowerCase() : ""
        if (tagQuery) {
            findTags()
            return
        }
        if (!shown || !catalogReady || !text) {
            openWhenReady = false
            return
        }
        const job = request({ op: "query", text: text, limit: 500 })
        queryJob = job
        job.then(result => {
            if (queryJob !== job) return
            queryJob = null
            const value = unpack(result)
            allResults = value.results
            indexer.status = value.status
            filterResults()
            openIfAsked()
        }).catch(error => {
            if (queryJob !== job) return
            queryJob = null
            openWhenReady = false
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
        matches.sort((a, b) => b.position - a.position)
        allResults = matches
        filterResults()
        openIfAsked()
    }
    function filterResults() {
        const previous = results[selected]?.id
        const visibleResults = allResults.filter(row => {
            const position = pinhole.library.indexOf(row.id)
            return position >= 0 && !pinhole.isHidden(position)
        })
        results = tagQuery ? visibleResults : visibleResults.slice(0, 240)
        const retained = results.findIndex(row => row.id === previous)
        selected = retained >= 0 ? retained : results.length ? 0 : -1
        if (tagAsset) {
            const taggedPosition = pinhole.library.indexOf(tagAsset)
            if (taggedPosition < 0 || pinhole.isHidden(taggedPosition)) closeTags(true)
        }
    }
    // ⏎ pressed before the answer came: it opens once the answer does.
    function openIfAsked() {
        if (!openWhenReady) return
        openWhenReady = false
        openResult(selected)
    }
    function accept() {
        if (debounce.running || queryJob) {
            openWhenReady = true
            if (debounce.running) lookup()
        } else {
            openResult(selected)
        }
    }
    function importBenchmark() {
        indexer.importBenchmark().then(() => { if (shown) lookup() })
    }
    function openResult(index) {
        if (index < 0 || index >= results.length) return
        const id = results[index].id
        if (pinhole.library.indexOf(id) < 0) return
        selected = index
        // Away while it shows, back on the one last looked at (onReturned).
        pinhole.openPhotos(results.map(row => row.id), id, { comeBack: true })
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
            if (results.length) grid.forceActiveFocus()
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
    function toResults() {
        if (!results.length) return
        grid.view.positionViewAtIndex(selected, GridView.Contain)
        grid.forceActiveFocus()
    }
    Command { name: "semantic.close"; keys: ["Escape"]; keyLayer: "plugin"; when: panel.shown; onTriggered: panel.tagAsset ? panel.closeTags(true) : panel.close() }
    Command { name: "semantic.down"; keys: ["Down"]; keyLayer: "plugin"; when: panel.browsing && search.input.activeFocus; onTriggered: panel.toResults() }
    Command {
        id: tagCommand
        name: "semantic.tags"
        text: qsTr("Tags")
        keys: ["T"]
        keyLayer: "plugin"
        when: panel.browsing && grid.activeFocus && panel.selected >= 0 && (panel.tagQuery || panel.catalogReady)
        onTriggered: panel.openTags()
    }
    Connections {
        target: search.input
        function onTextChanged() {
            panel.openWhenReady = false
            debounce.restart()
        }
    }
    Timer { id: debounce; interval: 250; onTriggered: panel.lookup() }
    Connections {
        target: pinhole
        function onDismissed() { panel.close() }
        function onHiddenChanged() { panel.filterResults() }
        function onReturned(photo) {
            const at = panel.results.findIndex(row => row.id === photo)
            grid.moveTo(photo)
            grid.forceActiveFocus()
        }
    }

    Item {
        id: titlebar
        visible: panel.browsing
        width: parent.width
        height: search.height + 16
        FieldInput {
            id: search
            objectName: "semanticSearchInput"
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.right: toolbar.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            height: closeButton.height
            placeholder: qsTr("Describe a photo or enter #tag…")
            input.maximumLength: 2048
            onAccepted: panel.accept()
        }
        Row {
            id: toolbar
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            ShellButton {
                id: tagButton
                objectName: "semanticTagButton"
                text: tagCommand.text
                hint: tagCommand.keyName
                visible: panel.selected >= 0 && panel.sourceKey !== ""
                usable: panel.tagQuery || panel.catalogReady
                onTapped: panel.openTags()
            }
            ShellButton { id: closeButton; objectName: "semanticSearchClose"; text: "✕"; hint: "Esc"; onTapped: panel.close() }
        }
        // How much of the library can be found yet, along its foot.
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 2
            visible: panel.catalogReady && !panel.indexer.complete && !panel.tagQuery
            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
            Rectangle {
                height: parent.height
                width: parent.width * panel.indexer.progress
                color: panel.indexer.paused ? theme.darkForeground : theme.accent
                Behavior on width { NumberAnimation { duration: 400; easing.type: Easing.OutCubic } }
            }
        }
    }
    Item {
        id: statusRow
        visible: panel.browsing && statusText.text !== ""
        anchors.top: titlebar.bottom
        width: parent.width
        height: visible ? statusText.implicitHeight + 12 : 0
        UiText {
            id: statusText
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 28
            wrapMode: Text.WordWrap
            color: panel.failure !== "" ? theme.red : theme.darkForeground
            text: panel.failure !== "" ? panel.failure
                : panel.tagQuery ? (panel.tagName ? qsTr("Photos tagged #%1").arg(panel.tagName) : qsTr("Every tagged photo"))
                : !panel.indexer.complete ? panel.indexer.summary
                : ""
        }
    }
    PhotoTiles {
        id: grid
        objectName: "semanticSearchResults"
        visible: panel.browsing && panel.results.length > 0
        anchors.top: statusRow.bottom
        anchors.topMargin: 2
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        library: pinhole.library
        photos: panel.results.map(row => row.id)
        active: panel.browsing && grid.activeFocus
        // The best match ringed while the question has the keys: ⏎ opens it.
        showCursor: panel.browsing && (grid.activeFocus || search.input.activeFocus)
        activeFocusOnTab: panel.results.length > 0
        Accessible.name: qsTr("Search results")
        onOpened: (photo, index) => panel.openResult(index)
        onPastTop: search.input.forceActiveFocus()
        Keys.onPressed: event => {
            // Typing goes on with the question.
            if (event.text.length === 1 && event.text >= " " && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                    && !(event.text === " " || event.text.toUpperCase() === "T")) {
                search.input.forceActiveFocus()
                search.input.insert(search.input.cursorPosition, event.text)
                event.accepted = true
            }
        }
    }

    // Before a question, or with none answered: what to ask.
    Column {
        id: invitation
        objectName: "semanticInvitation"
        visible: panel.browsing && panel.results.length === 0
        anchors.centerIn: parent
        anchors.verticalCenterOffset: titlebar.height / 2
        width: Math.min(parent.width - 48, theme.fontSize * 46)
        spacing: 14
        readonly property bool asking: panel.query === "" && panel.catalogReady
        readonly property var examples: [qsTr("beach at sunset"), qsTr("dog in the snow"), qsTr("birthday cake"),
            qsTr("mountains"), qsTr("city at night"), qsTr("bicycle"), qsTr("food on a plate"), qsTr("old car")]
        UiText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.pixelSize: Math.round(theme.fontSize * 1.35)
            color: theme.brightForeground
            text: !panel.catalogReady ? panel.indexer.summary
                : panel.tagQuery ? qsTr("No visible photos with this tag.")
                : panel.queryJob || debounce.running ? qsTr("Searching…")
                : panel.status.indexed === 0 ? qsTr("Search will be available as thumbnails are indexed.")
                : panel.query !== "" ? qsTr("Nothing here looks like that.")
                : qsTr("Find photos by what is in them")
        }
        UiText {
            width: parent.width
            visible: invitation.asking && panel.status.indexed > 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("Describe it in a few words, in English or Polish, or try one of these:")
        }
        Column {
            id: examples
            visible: invitation.asking && panel.status.indexed > 0
            width: parent.width
            spacing: 6
            // Rows of four, each centred under the words above.
            readonly property var words: invitation.examples.concat(panel.tagLabels.map(label => "#" + label))
            Repeater {
                model: Math.ceil(examples.words.length / 4)
                delegate: Row {
                    required property int index
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 6
                    Repeater {
                        model: examples.words.slice(index * 4, index * 4 + 4)
                        delegate: ShellButton {
                            required property string modelData
                            objectName: "semanticExample"
                            text: modelData
                            hint: ""
                            onTapped: panel.ask(modelData)
                        }
                    }
                }
            }
        }
    }

    Loader {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width, theme.fontSize * 52)
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
