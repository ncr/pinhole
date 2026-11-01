.pragma library

// A key sequence as the buttons and the help show it, in Omarchy's way:
// "Escape" as Esc, Shift as ⇧, "Ctrl+Shift+E" as "Ctrl ⇧ E", the arrows
// and Return as ← → ↑ ↓ and ⏎ ("Back ←", never "Back Left").
const symbols = { "Left": "←", "Right": "→", "Up": "↑", "Down": "↓", "Return": "⏎", "Enter": "⏎", "PgDown": "PgDn" }
function nameOf(sequence) {
    return sequence.replace(/(^|\+)(Left|Right|Up|Down|Return|Enter|PgDown)$/, (all, before, key) => before + symbols[key])
        .replace("Escape", "Esc").replace(/Shift\+/g, "⇧ ").replace(/Ctrl\+/g, "Ctrl ")
}
