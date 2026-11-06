import QtQml

// One thing the plugin asks for at a time, the latest winning (API 13): a
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
// `start(ask)` cancels what runs and calls `ask`, which returns the
// promise. Only its answer is said: `done(result)`, or `failed(error)`.
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
    property var _ask: null
    property var _again: null

    function start(ask) {
        if (_current !== null && coalesce) {
            _again = ask
            return
        }
        cancel()
        _run(ask, true)
    }
    // Stop what runs; its answer is never said.
    function cancel() {
        _again = null
        const running = _current
        _current = null
        if (running && typeof running.cancel === "function")
            running.cancel()
    }

    function _run(ask, retry) {
        let promise
        try {
            promise = ask()
        } catch (error) {
            failed(error)
            return
        }
        if (!promise || typeof promise.then !== "function") {
            done(promise)
            return
        }
        _current = promise
        _ask = ask
        promise.then(result => {
            if (_current !== promise)
                return
            _current = null
            done(result)
            _next()
        }, error => {
            if (_current !== promise)
                return
            _current = null
            const code = error ? error.code : undefined
            if (code === "reloaded" && retry && _again === null) {
                _run(ask, false)
                return
            }
            if (code !== "cancelled" && code !== "reloaded")
                failed(error)
            _next()
        })
    }
    function _next() {
        if (_again === null)
            return
        const ask = _again
        _again = null
        _run(ask, true)
    }

    Component.onDestruction: cancel()
}
