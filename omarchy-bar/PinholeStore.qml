pragma Singleton

import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons

// What the bar knows of Pinhole's library: the photos added most lately,
// which of them are new since the panel was last looked at, and the files
// they are dragged out as. One for the whole shell: the bar has a widget on
// every monitor, and they all show this.
//
// Pinhole does the work: `pinhole recent --follow` says the newest photos
// each time the library changes (read from its index, no service held
// open), and `pinhole export` makes the file a drag hands over, as a drag
// out of Pinhole's own window does (a HEIC as a JPEG, an edit as it looks).
Singleton {
  id: root

  // As many as the panel's grid holds.
  readonly property int capacity: 16
  readonly property int columns: 4

  // Newest first: { id, name, added, taken, thumb (url), ratio, color,
  // video, duration }.
  property var items: []
  // The library has said something at least once.
  property bool loaded: false
  // pinhole could not be run at all.
  property bool missing: false

  // The newest addition looked at in the panel (ms since 1970); -1 until
  // read from its file. Kept per computer.
  property real seenAdded: -1
  // While the panel is open: what was seen before it opened, so the photos
  // new to this look keep their mark until it closes.
  property real seenBefore: -1
  readonly property int freshCount: {
    if (seenAdded < 0) return 0
    var n = 0
    for (var i = 0; i < items.length; i++) if (items[i].added > seenAdded) n++
    return n
  }

  // id -> { path, at }: the file made for it, and when.
  property var prepared: ({})
  // id -> true while its file is being made; id -> message when it failed.
  property var preparing: ({})
  property var failed: ({})
  // Chosen for a drag of several: id -> true.
  property var selected: ({})
  readonly property int selectedCount: Object.keys(selected).length
  // A drag is under way.
  property bool dragging: false

  property bool open: false
  property Item anchorItem: null
  property QtObject anchorBar: null
  property QtObject anchorOwner: null
  property string fontFamily: Style.font.family

  // Files of earlier days are cleared away by Pinhole (its staging folder
  // keeps a day): made again after half of that.
  readonly property real freshFor: 12 * 3600 * 1000

  function isFresh(item) { return seenBefore >= 0 && item.added > seenBefore }
  function isReady(id) {
    var p = prepared[id]
    return !!p && Date.now() - p.at < freshFor
  }

  function showOn(anchor, bar, owner) {
    anchorItem = anchor
    anchorBar = bar
    anchorOwner = owner
    seenBefore = seenAdded
    open = true
    markSeen()
    prepare(items.map(function(item) { return item.id }))
  }
  function hide() { open = false }

  onOpenChanged: {
    if (open) return
    selected = ({})
    markSeen()
    seenBefore = seenAdded
  }

  function markSeen() {
    var newest = items.length > 0 ? items[0].added : 0
    if (newest > seenAdded) {
      seenAdded = newest
      stateFile.setText(JSON.stringify({ seenAdded: newest }) + "\n")
    }
  }

  function toggleSelected(id) {
    var next = Object.assign({}, selected)
    if (next[id]) delete next[id]
    else next[id] = true
    selected = next
  }

  // What a drag from this photo carries: the photos chosen if it is one of
  // them, else itself; in the grid's order.
  function dragIds(id) {
    if (!selected[id]) return [id]
    return items.filter(function(item) { return !!selected[item.id] }).map(function(item) { return item.id })
  }
  function uriList(ids) {
    var lines = []
    for (var i = 0; i < ids.length; i++) {
      var p = prepared[ids[i]]
      if (p) lines.push(fileUrl(p.path))
    }
    return lines
  }

  function fileUrl(path) {
    return "file://" + String(path).split("/").map(encodeURIComponent).join("/")
  }

  // Opened as the file it is dragged as, in the desktop's own viewer.
  function openPhoto(id) {
    var p = prepared[id]
    if (p && String(p.path).charAt(0) === "/") Quickshell.execDetached(["xdg-open", p.path])
    else { pendingOpen = id; prepare([id]) }
  }
  property string pendingOpen: ""

  // Pinhole's window: brought forward if it is open (by its class, which
  // only it has: a title could be any terminal's in ~/dev/pinhole), else
  // opened as the launcher opens it.
  function openPinhole() {
    open = false
    Quickshell.execDetached(["sh", "-c",
      'a=$(hyprctl clients -j | jq -r \'[.[] | select(.class == "pinhole")] | sort_by(.focusHistoryID) | .[0].address // empty\'); '
      + 'if [ -n "$a" ]; then hyprctl dispatch "hl.dsp.focus({ window = \\"address:$a\\" })" >/dev/null 2>&1 || hyprctl dispatch focuswindow "address:$a"; '
      + 'elif command -v uwsm-app >/dev/null; then exec setsid uwsm-app -- pinhole.desktop; '
      + 'else PATH="$HOME/.local/bin:$PATH" exec setsid pinhole; fi'])
  }

  // Pinhole's window where the pointer is, the bar's strip apart: opened
  // there, or brought there if it is open (open-here).
  function openHere(axis, from, to) {
    open = false
    var script = String(Qt.resolvedUrl("open-here")).replace(/^file:\/\//, "")
    Quickshell.execDetached(["sh", decodeURIComponent(script), axis, String(Math.floor(from)), String(Math.ceil(to))])
  }

  // ---- The library, followed --------------------------------------------

  // A launcher in ~/.local/bin (a source install) comes before the
  // package's: the shell's PATH may not have it.
  readonly property string runner: 'PATH="$HOME/.local/bin:$PATH" exec pinhole "$@"'

  Process {
    id: follower
    command: ["sh", "-c", root.runner, "pinhole", "recent", "--count", String(root.capacity), "--follow"]
    running: true
    stdout: SplitParser { onRead: function(line) { root.take(line) } }
    onExited: function(code) {
      // 127: no pinhole on this computer. Looked for again now and then.
      root.missing = code === 127
      again.interval = root.missing ? 60000 : 5000
      again.restart()
    }
  }
  Timer { id: again; onTriggered: follower.running = true }

  function take(line) {
    var said
    try { said = JSON.parse(line) } catch (e) { return }
    if (!said || !Array.isArray(said.items)) return
    var list = []
    for (var i = 0; i < said.items.length && list.length < capacity; i++) {
      var item = clean(said.items[i])
      if (item) list.push(item)
    }
    items = list
    loaded = true
    missing = false
    if (seenAdded >= 0 && open) markSeen()
    else if (seenAdded >= 0 && ownOnly(list)) markSeen()
    // What has just come is what will be dragged: its file is made now,
    // so the drag is under way the moment it starts.
    var fresh = []
    for (var j = 0; j < list.length; j++)
      if (open || (seenAdded >= 0 && list[j].added > seenAdded)) fresh.push(list[j].id)
    prepare(fresh)
    var known = {}
    for (var k = 0; k < list.length; k++) known[list[k].id] = true
    var kept = {}
    for (var id in selected) if (known[id]) kept[id] = true
    selected = kept
  }

  // Everything new is what was just dropped on the bar from here.
  function ownOnly(list) {
    var now = Date.now(), fresh = 0
    for (var i = 0; i < list.length; i++) {
      if (list[i].added <= seenAdded) continue
      var until = ownNames[stem(list[i].name)]
      if (!until || until < now) return false
      fresh++
    }
    return fresh > 0
  }

  // The index is Pinhole's, the names in it the camera's or whoever made
  // the file: checked, bounded, and shown only as plain text.
  function clean(raw) {
    if (!raw || typeof raw !== "object") return null
    var id = raw.id
    if (typeof id !== "string" || id.length === 0 || id.length > 1024 || /[\u0000\n\r]/.test(id)) return null
    var added = Number(raw.added), taken = Number(raw.taken)
    if (!isFinite(added) || !isFinite(taken)) return null
    var thumb = typeof raw.thumb === "string" && raw.thumb.charAt(0) === "/" && raw.thumb.length < 4096 ? fileUrl(raw.thumb) : ""
    var w = Number(raw.width), h = Number(raw.height)
    var color = typeof raw.color === "string" && /^#[0-9a-fA-F]{6}$/.test(raw.color) ? raw.color : "#303030"
    var duration = Number(raw.duration)
    return {
      id: id,
      name: typeof raw.name === "string" ? raw.name.slice(0, 255) : "",
      added: added,
      taken: taken,
      thumb: thumb,
      ratio: w > 0 && h > 0 ? w / h : 1,
      color: color,
      video: raw.video === true,
      duration: isFinite(duration) && duration > 0 ? duration : 0
    }
  }

  // ---- Files to drag ----------------------------------------------------

  property var queue: []

  function prepare(ids) {
    var wanted = queue.slice()
    for (var i = 0; i < ids.length; i++) {
      var id = ids[i]
      if (isReady(id) || preparing[id] || wanted.indexOf(id) >= 0) continue
      wanted.push(id)
    }
    if (wanted.length === queue.length) return
    queue = wanted
    exportNext()
  }

  function exportNext() {
    if (exporter.running || queue.length === 0) return
    var batch = queue.slice(0, 8)
    queue = queue.slice(8)
    var now = Object.assign({}, preparing)
    var failedNow = Object.assign({}, failed)
    for (var i = 0; i < batch.length; i++) { now[batch[i]] = true; delete failedNow[batch[i]] }
    preparing = now
    failed = failedNow
    exporter.batch = batch
    exporter.command = ["sh", "-c", root.runner, "pinhole", "export", "--json"].concat(batch)
    exporter.running = true
  }

  Process {
    id: exporter
    property var batch: []
    stdout: SplitParser { onRead: function(line) { root.exported(line) } }
    onExited: {
      // Whatever it did not answer for failed.
      var left = Object.assign({}, root.preparing)
      var failedNow = Object.assign({}, root.failed)
      for (var i = 0; i < batch.length; i++) {
        if (left[batch[i]]) { delete left[batch[i]]; failedNow[batch[i]] = "failed" }
      }
      root.preparing = left
      root.failed = failedNow
      batch = []
      Qt.callLater(root.exportNext)
    }
  }

  function exported(line) {
    var said
    try { said = JSON.parse(line) } catch (e) { return }
    if (!said || typeof said.id !== "string") return
    var left = Object.assign({}, preparing)
    delete left[said.id]
    preparing = left
    var path = typeof said.path === "string" ? said.path : ""
    // Only what Pinhole made for handing over.
    if (path.charAt(0) === "/" && path.indexOf("/pinhole/outgoing/") > 0 && path.indexOf("/../") < 0) {
      var next = Object.assign({}, prepared)
      next[said.id] = { path: path, at: Date.now() }
      prepared = next
      if (pendingOpen === said.id) { pendingOpen = ""; openPhoto(said.id) }
    } else {
      var failedNow = Object.assign({}, failed)
      failedNow[said.id] = "failed"
      failed = failedNow
    }
  }

  // ---- Files dropped on the bar, added ----------------------------------

  // As files dropped on Pinhole's window are: `pinhole add`, which asks
  // the service, for the source shown, one drop after another.
  property bool adding: false
  // How far, 0 to 1, of all the files of the drop being added.
  property real addProgress: 0
  // How the last drop went, a moment after: "added", "problem" or "".
  property string addResult: ""
  // Stems of the files just added: their photos coming into the library
  // are this computer's own doing, not news for the bar to light up for.
  property var ownNames: ({})
  property var addQueue: []

  // The local files and folders of a drop, as paths; none of the files
  // Pinhole makes to hand over, already in its library.
  function droppable(urls) {
    var paths = []
    for (var i = 0; i < urls.length && paths.length < 500; i++) {
      var url = String(urls[i])
      if (url.indexOf("file://") !== 0) continue
      var path
      try { path = decodeURIComponent(url.slice(7).replace(/^localhost\//, "/")) } catch (e) { continue }
      if (path.charAt(0) !== "/" || /[\u0000\n\r]/.test(path)) continue
      if (path.indexOf("/pinhole/outgoing/") >= 0) continue
      paths.push(path)
    }
    return paths
  }

  function add(paths) {
    if (paths.length === 0) return
    addQueue = addQueue.concat([paths])
    addNext()
  }

  function addNext() {
    if (adder.running || addQueue.length === 0) return
    var paths = addQueue[0]
    addQueue = addQueue.slice(1)
    adding = true
    addProgress = 0
    addResult = ""
    resultShown.stop()
    adder.answer = null
    adder.paths = paths
    // The paths are arguments, never part of the command line's text.
    adder.command = ["sh", "-c", root.runner, "pinhole", "add", "--json"].concat(paths)
    adder.running = true
  }

  Process {
    id: adder
    property var paths: []
    property var answer: null
    stdout: SplitParser {
      onRead: function(line) {
        var said
        try { said = JSON.parse(line) } catch (e) { return }
        if (!said || typeof said !== "object") return
        var p = Number(said.progress)
        if (isFinite(p)) root.addProgress = Math.max(root.addProgress, Math.min(1, Math.max(0, p)))
        else adder.answer = said
      }
    }
    onExited: function(code) {
      root.added(adder.answer, adder.paths, code)
      Qt.callLater(root.addNext)
    }
  }

  Timer { id: resultShown; interval: 3200; onTriggered: root.addResult = "" }

  function stem(name) {
    var base = String(name).split("/").pop()
    var dot = base.lastIndexOf(".")
    return (dot > 0 ? base.slice(0, dot) : base).toLowerCase()
  }

  // How the drop went: a tick on the bar, and what was not added (or added
  // with a note) told the way the window tells it, as a notification.
  function added(answer, paths, code) {
    adding = addQueue.length > 0
    addProgress = 1
    var problems = []
    var failed = 0, addedCount = 0
    var place = answer && typeof answer.place === "string" && answer.place.length > 0 ? answer.place.slice(0, 80) : "Pinhole"
    if (answer && Array.isArray(answer.files)) {
      var own = Object.assign({}, ownNames)
      var until = Date.now() + 10 * 60 * 1000
      for (var i = 0; i < answer.files.length && i < 500; i++) {
        var f = answer.files[i] || {}
        var name = String(f.file || "").split("/").pop().slice(0, 255)
        var count = Array.isArray(f.files) ? Math.max(1, f.files.length) : 1
        if (f.state === "added") {
          addedCount += count
          var made = Array.isArray(f.files) ? f.files : [f.file]
          for (var j = 0; j < made.length && j < 500; j++) own[stem(made[j])] = until
          if (f.note) problems.push(strings.t("add.stillOnly").arg(name))
        } else if (f.state === "duplicate") {
          problems.push(strings.t("add.duplicate").arg(name).arg(place))
        } else {
          failed += count
          var error = f.error || {}
          problems.push(strings.t("add.failed").arg(name).arg(reason(String(error.code || ""), place)))
        }
      }
      ownNames = own
    } else {
      // No answer at all: the service could not be asked.
      failed = Math.max(1, paths.length)
      var whole = answer && typeof answer.error === "string" ? reason(answer.error, place) : strings.t("add.reason.unavailable").arg(place)
      problems.push(whole)
    }
    addResult = failed > 0 || (addedCount === 0 && problems.length > 0) ? "problem" : "added"
    resultShown.restart()
    if (problems.length > 0) {
      var title = failed > 0 ? strings.tn("add.notAdded", failed).arg(failed)
                : addedCount > 0 ? strings.t("add.withNote") : strings.t("add.already").arg(place)
      notify(title, problems.slice(0, 12).join("\n"))
    }
  }

  function reason(code, place) {
    var known = ["unsupported", "too_large", "over_quota", "protected", "needs_sign_in", "unavailable", "read_only", "no_service", "not_found"]
    return known.indexOf(code) >= 0 ? strings.t("add.reason." + code).arg(place) : strings.t("add.reason.other")
  }

  // The notification's text is the files' names: no markup gets through.
  function notify(title, body) {
    var plain = function(text) { return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;") }
    Quickshell.execDetached(["notify-send", "--app-name=Pinhole", "--icon=pinhole", plain(title), plain(body)])
  }

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  // ---- What was seen, kept --------------------------------------------

  // The theme's cyan, the colour Pinhole's video mark is lit in
  // (plugins.cpp's pinhole/video): from the theme's colors.toml, as
  // Pinhole's own window reads it; Tokyo Night's where it says none.
  property color cyan: "#449dab"
  FileView {
    path: (Quickshell.env("XDG_STATE_HOME") || (Quickshell.env("HOME") + "/.local/state")) + "/omarchy/current/theme/colors.toml"
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: {
      var found = String(text()).match(/^\s*cyan\s*=\s*["'](#[0-9A-Fa-f]{6})["']/m)
      root.cyan = found ? found[1] : "#449dab"
    }
  }

  readonly property string stateDir: (Quickshell.env("XDG_STATE_HOME") || (Quickshell.env("HOME") + "/.local/state")) + "/pinhole"

  FileView {
    id: stateFile
    path: root.stateDir + "/bar.json"
    printErrors: false
    onLoaded: {
      var seen = -1
      try { seen = Number(JSON.parse(text()).seenAdded) } catch (e) {}
      if (isFinite(seen) && seen > 0) root.seenAdded = seen
      else firstLook.start()
    }
    // First run: nothing is new until something comes.
    onLoadFailed: firstLook.start()
  }
  // The first look waits for the library to have said what it has.
  Timer {
    id: firstLook
    interval: 500
    repeat: true
    onTriggered: if (root.loaded) { stop(); root.markSeen() }
  }

  // ---- The panel ------------------------------------------------------

  RecentPopup {
    store: root
    open: root.open
    dragging: root.dragging
    anchorItem: root.anchorItem
    bar: root.anchorBar
    owner: root.anchorOwner
    onDismissed: root.hide()
  }
}
