import QtQuick

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
    property var catalogJob: null
    property var statusJob: null
    property var indexingJob: null
    property int generation: 0
    property int catalogRevision: 0
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
        ++catalogRevision
        cancel(catalogJob); catalogJob = null
        cancel(statusJob); statusJob = null
        cancel(indexingJob); indexingJob = null
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
        if (!ready || managed) return
        const revision = ++catalogRevision
        const current = generation
        cancel(catalogJob)
        catalogReady = false
        const job = pinhole.query("SELECT id, thumb_path AS path FROM assets WHERE is_video=0 AND thumb_path IS NOT NULL AND thumb_path<>'' ORDER BY id LIMIT 100001")
        catalogJob = job
        job.then(assets => {
            if (current !== generation || revision !== catalogRevision || catalogJob !== job) return
            const sync = request({ op: "catalog", assets: assets, auto_index: !paused })
            catalogJob = sync
            return sync.then(result => {
                if (current !== generation || revision !== catalogRevision || catalogJob !== sync) return
                catalogJob = null
                status = unpack(result)
                if (status.error) failure = friendly(status.error)
                catalogReady = true
            })
        }).catch(error => {
            if (current !== generation || revision !== catalogRevision) return
            catalogJob = null
            report(error)
        })
    }
    function poll() {
        if ((!managed && (!ready || !catalogReady)) || statusJob) return
        const current = generation
        const job = request({ op: "status" })
        statusJob = job
        job.then(result => {
            if (current !== generation || statusJob !== job) return
            statusJob = null
            status = unpack(result)
            if (managed) {
                missedStatus = 0
                ready = true
                catalogReady = status.catalog_ready
                failure = status.error ? friendly(status.error) : ""
            } else if (status.error) failure = friendly(status.error)
        }).catch(error => {
            if (current !== generation || statusJob !== job) return
            statusJob = null
            if (managed) {
                ready = false; catalogReady = false
                if (++missedStatus >= 2)
                    failure = qsTr("Waiting for Pinhole's background search service.")
            } else report(error)
        })
    }
    // A finished benchmark run's vectors, for the Apple Neural Engine
    // profile: the index without indexing.
    function importBenchmark() {
        const benchmark = profile?.benchmark
        if (!benchmark) return Promise.resolve()
        failure = ""
        const source = sourceKey
        return request({ op: "import", run: benchmark.run, corpus: benchmark.corpus }).then(result => {
            if (source === sourceKey) status = unpack(result)
        }).catch(error => { if (source === sourceKey) report(error) })
    }
    function toggleIndexing() {
        if (!catalogReady || indexingJob) return
        failure = ""
        const resume = paused || status.error !== ""
        pinhole.settings.indexingPaused = !resume
        const current = generation
        const job = request(resume ? { op: "start", missing_only: true } : { op: "stop" })
        indexingJob = job
        job.then(result => {
            if (current !== generation || indexingJob !== job) return
            indexingJob = null
            status = unpack(result)
        }).catch(error => {
            if (current !== generation || indexingJob !== job) return
            indexingJob = null
            if (resume) pinhole.settings.indexingPaused = true
            report(error)
        })
    }

    // Opening search can start the worker sooner. The normal launch path
    // leaves the first frame and settling of the photo grid to Pinhole.
    Timer { id: startup; interval: 4000; running: true; onTriggered: indexer.ensureStarted() }
    Timer { id: changes; interval: 900; onTriggered: indexer.catalog() }
    // Its own helper, asked every two seconds while it indexes; Pinhole's
    // worker says when it got further (workerSaid), asked besides every
    // ten seconds, to notice one that is gone.
    Timer { interval: 2000; repeat: true; running: !indexer.managed && indexer.ready && indexer.status.running; onTriggered: indexer.poll() }
    Timer { interval: 10000; repeat: true; running: indexer.managed; onTriggered: indexer.poll() }
    Connections {
        target: pinhole
        function onWorkerSaid(name, said) { if (name === "index" && indexer.managed) indexer.poll() }
        function onWorkersChanged() { if (indexer.managed) indexer.poll() }
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
