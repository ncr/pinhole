import QtQml

// One thing the plugin asks for at a time, the latest winning: a
// query, a run, a fetch. What a plugin otherwise writes by hand around
// each promise: cancel the one before, drop an answer that came too late,
// keep a busy flag, ask again when the library was read meanwhile, and
// stop when the plugin does.
//
//     Job {
//         id: reading
//         onDone: rows => recent.rows = rows
//         onFailed: error => recent.problem = error.message
//     }
//     reading.start(() => pinhole.query(sql, [limit]))
//
// `start(ask, answered)` cancels what runs and calls `ask`, which returns the
// promise, or a list of them asked together (a page of previews): then
// `done` has their answers in order, the first to fail fails them all, and
// a cancel cancels each. Only its answer is said: `done(result)`, or
// `failed(error)`.
// Given `answered`, that has the answer instead of done(), for an answer
// that needs what it was asked with:
//
//     motion.start(() => pinhole.fetch(photo, kind), file => play(kind, file))
//
// Cancelled is no failure and says nothing. Stopped by the library read
// again (`reloaded`), it asks again by itself, once.
QtObject {
    id: job
    // Something asked and not yet answered.
    readonly property bool busy: _current !== null
    // Asked while busy: wait for the one running, then ask once more with
    // the latest (a search typed into), instead of cancelling it.
    property bool coalesce: false
    signal done(var result)
    signal failed(var error)

    property var _current: null
    property var _again: null

    function start(ask, answered) {
        const asking = answered ? { ask: ask, answered: answered } : { ask: ask }
        if (_current !== null && coalesce) {
            _again = asking
            return
        }
        cancel()
        _run(asking, true)
    }
    // Stop what runs; its answer is never said.
    function cancel() {
        _again = null
        const running = _current
        _current = null
        if (running && typeof running.cancel === "function")
            running.cancel()
    }

    function _run(asking, retry) {
        const answer = result => asking.answered ? asking.answered(result) : done(result)
        let promise
        try {
            promise = asking.ask()
        } catch (error) {
            failed(error)
            return
        }
        if (Array.isArray(promise))
            promise = _together(promise)
        if (!promise || typeof promise.then !== "function") {
            answer(promise)
            return
        }
        _current = promise
        promise.then(result => {
            if (_current !== promise)
                return
            _current = null
            answer(result)
            _next()
        }, error => {
            if (_current !== promise)
                return
            _current = null
            const code = error ? error.code : undefined
            if (code === "reloaded" && retry && _again === null) {
                _run(asking, false)
                return
            }
            if (code !== "cancelled" && code !== "reloaded")
                failed(error)
            _next()
        })
    }
    // Several as one: answered when all are, failed with the first that
    // fails, each cancelled with it.
    function _together(promises) {
        let cancelled = false
        const together = new Promise((resolve, reject) => {
            const results = new Array(promises.length)
            let left = promises.length
            if (left === 0)
                resolve(results)
            promises.forEach((p, i) => Promise.resolve(p).then(value => {
                results[i] = value
                if (--left === 0)
                    resolve(results)
            }, reject))
        })
        together.cancel = () => {
            if (cancelled)
                return
            cancelled = true
            for (const p of promises)
                if (p && typeof p.cancel === "function")
                    p.cancel()
        }
        return together
    }
    function _next() {
        if (_again === null)
            return
        const asking = _again
        _again = null
        _run(asking, true)
    }

    Component.onDestruction: cancel()
}
