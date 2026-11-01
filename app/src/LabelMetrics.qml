import QtQuick

// How wide a button's words are drawn (UiText's font), for a bar to know
// what fits before laying it out: their advance, or for an icon drawn
// wider than its cell (Nerd Font's heart), its ink, as a Text's width
// takes it in: the ink's whole pixels, and one more for the part of one it
// rounds away.
FontMetrics {
    font.family: theme.font
    font.pixelSize: theme.fontSize
    function of(text) {
        const advance = advanceWidth(text)
        const ink = boundingRect(text)
        return ink.x + ink.width > advance ? Math.ceil(ink.x + ink.width) + 1 : advance
    }
}
