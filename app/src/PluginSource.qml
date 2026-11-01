import QtQuick

// A kind of source, as its plugin shows it (PinholePlugin.sources,
// docs/plugins.md): what Pinhole says of a source of that kind and the
// buttons it has, where Pinhole lists sources (the side panel), offers new
// ones (the first run, Add a source) and says one needs signing in. Only a
// kind its manifest's "sources" names is taken.
//
// Each `source` given is one of pinhole.sources.list (id, kind, plugin,
// name, count, thumbs, and its kind's own fields, such as account or
// path), with `shown` and `health` for the one shown.
QtObject {
    // One of its manifest's kinds: "icloud".
    property string kind
    // A Nerd Font glyph for its sources' rows and its offers.
    property string icon: ""
    // The kind in a word, before a source's count in its row: "iCloud",
    // "Folder". Its plugin's name when left out.
    property string noun
    // The photos' place in a sentence ("Nothing in %1 yet", "Add 3 files
    // to %1"): source => "iCloud". The source's name when left out.
    property var place: null
    // What was found here that could be added (pinhole.sources.discovered),
    // best first: [{ title, detail, icon, triggered }]. The first five of
    // all the plugins' are offered on ⏎ and 2–5.
    property var offers: []
    // The ways in that are always there: [{ title, key, triggered }], each
    // a button with its key ("Sign in to iCloud", I).
    property var ways: []
    // A line in place of a source's count when it needs something, said in
    // the theme's red: source => "Sign in again"; "" when all is well.
    property var note: null
    // Its buttons in its row beside Remove: source => [{ text, key, ask,
    // triggered }], `ask` the question its row asks first (Yes, No).
    property var actions: null
    // What Remove asks of a source: source => "Forget its index? The photos
    // stay in the folder."
    property var forgets: null
    // Under "Add 3 files to %1" as files are dragged over the window:
    // source => "Copied into the folder…".
    property var adds: null
    // The line for a source that says itself when something changes,
    // in place of when it is next looked at.
    property string pushed
    // What "Sign in" does where Pinhole says a source needs it (the card
    // in the middle, I, its row): source => {…}; null for a kind that never
    // needs signing in.
    property var signIn: null
}
