import QtQuick
import Pinhole.Plugin

// The shared library uses pinholed's worker; isolated libraries own theirs.
Item {
    id: indexer
    visible: false
    readonly property string helper: pinhole.dir + "/pinhole-semantic"
    readonly property bool paused: pinhole.settings.indexingPaused === true
    property bool ready: false
    property bool catalogReady: false
    property bool managed: false
    property int missedStatus: 0
    property string sourceKey: ""
    property string indexPath: ""
    property string failure: ""
    property var profile: null
    property var status: ({ indexed: 0, total: 0, running: false, waiting: false, errors: 0 })
    property var server: null
    // Indexing turned on or off, asked of the helper and not yet answered.
    readonly property bool toggling: toggler.busy
    // The model loading, so a question asked now waits for it.
    readonly property bool loading: warmer.busy
    property int generation: 0
    readonly property bool complete: catalogReady && status.total > 0 && status.indexed >= status.total
    readonly property real progress: status.total > 0 ? Math.min(1, status.indexed / status.total) : 0
    function count(n) { return Number(n || 0).toLocaleString(Qt.locale(), "f", 0) }
    // What can be found now, in a sentence: the search view's and the side
    // panel's.
    readonly property string summary: failure !== "" ? failure
        : !ready ? qsTr("Starting search…")
        : !catalogReady ? qsTr("Reading the library…")
        : complete ? qsTr("All %1 photos can be found.").arg(count(status.total))
        : paused ? qsTr("Indexing is paused: %1 of %2 photos can be found.").arg(count(status.indexed)).arg(count(status.total))
        : status.yielding ? qsTr("Indexing while you are away from the screen, so the desktop stays smooth: %1 of %2 photos can be found.").arg(count(status.indexed)).arg(count(status.total))
        : status.waiting ? qsTr("Waiting for the graphics card: %1 of %2 photos can be found.").arg(count(status.indexed)).arg(count(status.total))
        : qsTr("Looking through your photos: %1 of %2 can be found so far.").arg(count(status.indexed)).arg(count(status.total))

    function cancel(job) { if (job) job.cancel() }
    function reset() {
        ++generation
        cataloguer.cancel()
        poller.cancel()
        toggler.cancel()
        cancel(server); server = null
        managed = false; missedStatus = 0
        ready = false; catalogReady = false; failure = ""; indexPath = ""
        status = { indexed: 0, total: 0, running: false, waiting: false, errors: 0 }
    }
    function friendly(message) {
        return message.includes("reboot")
            ? qsTr("The NPU stopped responding. Restart the computer and repeat its smoke test before indexing again. Existing results still work.")
            : message
    }
    function report(error) {
        if (error.code === "cancelled" || error.code === "reloaded") return
        let message = error.message || String(error)
        if (error.stdout) {
            try { message = JSON.parse(error.stdout).error || message } catch (_) {}
        }
        if (error.stderr?.includes("No local photo-search engine"))
            message = qsTr("Set up a local photo search engine for this computer.")
        failure = friendly(message)
        pinhole.warn(error)
    }
    function ensureStarted() {
        if (sourceKey !== pinhole.library.key) {
            reset()
            sourceKey = pinhole.library.key
        }
        if (server || managed || !sourceKey || !pinhole.library.count) return
        const current = generation
        const job = pinhole.run(helper, ["config", "--library", pinhole.library.dir, "--library-key", sourceKey])
        server = job
        job.then(result => {
            if (server !== job || current !== generation) return
            profile = JSON.parse(result.stdout)
            indexPath = pinhole.cacheDir + "/" + profile.indexFile
            if (profile.managed) {
                server = null
                managed = true
                poll()
            } else {
                launchWorker()
            }
        }).catch(error => {
            if (server !== job || current !== generation) return
            server = null; report(error)
        })
    }
    function launchWorker() {
        const current = generation
        const args = [profile.worker, "serve", "--background", "--index", indexPath,
            "--library", pinhole.library.dir, "--library-key", sourceKey]
        const job = pinhole.run(profile.python, args.concat(profile.args), {
            timeout: 0,
            output: line => {
                if (current !== generation) return
                let event
                try { event = JSON.parse(line) } catch (_) { return }
                if (event.event === "ready") {
                    ready = true
                    status = event.status
                    catalog()
                } else if (event.event === "error") failure = friendly(event.error)
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
    // A question to the worker, a promise of its answer: pinholed's
    // worker asked in place (pinhole.ask, no program started each time),
    // this plugin's own through its socket.
    function request(message) {
        if (managed)
            return pinhole.ask("index", message, { timeout: 120000 })
        return pinhole.run(helper, ["request", "--index", indexPath], {
            input: JSON.stringify(message), timeout: 120000
        }).then(result => {
            const value = JSON.parse(result.stdout)
            if (!value.ok) throw new Error(value.error)
            return value.result
        })
    }
    function catalog() {
        if (!ready || managed) return
        catalogReady = false
        cataloguer.start(() => pinhole.query("SELECT id, thumb_path AS path FROM assets WHERE is_video=0 AND thumb_path IS NOT NULL AND thumb_path<>'' ORDER BY id LIMIT 100001")
            .then(assets => request({ op: "catalog", assets: assets, auto_index: !paused })))
    }
    Job {
        id: cataloguer
        onDone: value => {
            indexer.status = value
            if (value.error) indexer.failure = indexer.friendly(value.error)
            indexer.catalogReady = true
        }
        onFailed: error => indexer.report(error)
    }
    // Asked while one is asked: that one's answer will do.
    function poll() {
        if ((!managed && (!ready || !catalogReady)) || poller.busy) return
        poller.start(() => request({ op: "status" }))
    }
    Job {
        id: poller
        onDone: value => {
            indexer.status = value
            if (indexer.managed) {
                indexer.missedStatus = 0
                indexer.ready = true
                indexer.catalogReady = value.catalog_ready
                indexer.failure = value.error ? indexer.friendly(value.error) : ""
            } else if (value.error) indexer.failure = indexer.friendly(value.error)
        }
        onFailed: error => {
            if (indexer.managed) {
                indexer.ready = false; indexer.catalogReady = false
                if (++indexer.missedStatus >= 2)
                    indexer.failure = qsTr("Waiting for Pinhole's background search service.")
            } else indexer.report(error)
        }
    }
    // Search ready from the start: the model loaded as soon as the library
    // is read and held while Pinhole is open, whatever else wants the
    // memory (asked again each `keeper` interval, which also starts it
    // again if it stopped; the worker holds it 90 seconds past each ask).
    // What it answers is no matter; a search says the rest.
    function warm() {
        if (!catalogReady || warmer.busy) return
        warmer.start(() => request({ op: "warm" }))
    }
    Job { id: warmer }
    onCatalogReadyChanged: if (catalogReady) warm()
    Timer { id: keeper; interval: 20000; repeat: true; running: indexer.catalogReady; onTriggered: indexer.warm() }
    // Pinhole's worker, as pinholed says it runs: asked how far it is, or,
    // gone (or pinholed with it), waited for.
    function watch() {
        const worker = pinhole.workers.find(each => each.name === "index")
        if (worker?.running && pinhole.sources.health !== "no_service") {
            poll()
            return
        }
        poller.cancel()
        ready = false; catalogReady = false
        failure = qsTr("Waiting for Pinhole's background search service.")
    }
    // A finished benchmark run's vectors, for the Apple Neural Engine
    // profile: the index without indexing.
    function importBenchmark() {
        const benchmark = profile?.benchmark
        if (!benchmark) return Promise.resolve()
        failure = ""
        const source = sourceKey
        return request({ op: "import", run: benchmark.run, corpus: benchmark.corpus }).then(result => {
            if (source === sourceKey) status = result
        }).catch(error => { if (source === sourceKey) report(error) })
    }
    function toggleIndexing() {
        if (!catalogReady || toggling) return
        failure = ""
        const resume = paused || status.error !== ""
        pinhole.settings.indexingPaused = !resume
        toggler.start(() => request(resume ? { op: "start", missing_only: true } : { op: "stop" }))
    }
    Job {
        id: toggler
        onDone: value => indexer.status = value
        onFailed: error => {
            // Not started after all: paused, as it was.
            if (!indexer.paused) pinhole.settings.indexingPaused = true
            indexer.report(error)
        }
    }

    // As Pinhole starts, just after its first frame: the worker loads the
    // model at idle priority, away from the window.
    Timer { id: startup; interval: 250; running: true; onTriggered: indexer.ensureStarted() }
    Timer { id: changes; interval: 900; onTriggered: indexer.catalog() }
    // Its own helper, asked every two seconds while it indexes; Pinhole's
    // worker says when it got further (workerSaid), and pinholed when it
    // starts or ends (workersChanged) or is gone itself (health).
    Timer { interval: 2000; repeat: true; running: !indexer.managed && indexer.ready && indexer.status.running; onTriggered: indexer.poll() }
    Connections {
        target: pinhole
        function onWorkerSaid(name, said) { if (name === "index" && indexer.managed) indexer.poll() }
        function onWorkersChanged() { if (indexer.managed) indexer.watch() }
    }
    Connections {
        target: pinhole.sources
        function onHealthChanged() { if (indexer.managed) indexer.watch() }
    }
    Connections {
        target: pinhole.library
        function onChanged() {
            if (indexer.sourceKey !== pinhole.library.key) {
                indexer.reset()
                indexer.sourceKey = pinhole.library.key
                startup.restart()
            } else if (indexer.ready && !indexer.managed) {
                if (!changes.running) changes.start()
            } else if (!indexer.managed && !indexer.server && !indexer.failure && !startup.running) {
                startup.start()
            }
        }
    }
}
