.pragma library

// A modal's own focus chain: include nested plugin controls and text fields,
// skip unavailable or hidden ones, and keep Tab away from the photos beneath.
function items(root) {
    const found = []
    function walk(item) {
        if (!item.visible || !item.enabled) return
        if (item.activeFocusOnTab && item.usable !== false) found.push(item)
        for (const child of item.children) walk(child)
    }
    for (const child of root.children) walk(child)
    return found.sort((a, b) => {
        const ap = a.mapToItem(root, 0, 0), bp = b.mapToItem(root, 0, 0)
        return Math.round(ap.y) - Math.round(bp.y) || Math.round(ap.x) - Math.round(bp.x)
    })
}

// Whether `item` is `root` or within it.
function within(root, item) {
    for (let at = item; at; at = at.parent)
        if (at === root) return true
    return false
}

function move(root, direction, current) {
    const all = items(root)
    if (!all.length) { root.forceActiveFocus(); return }
    const at = all.indexOf(current)
    const next = at < 0 ? (direction < 0 ? all.length - 1 : 0) : (at + direction + all.length) % all.length
    all[next].forceActiveFocus(direction < 0 ? Qt.BacktabFocusReason : Qt.TabFocusReason)
    reveal(all[next])
}

function reveal(item) {
    for (let parent = item.parent; parent; parent = parent.parent) {
        if (parent.contentItem === undefined || parent.contentY === undefined) continue
        const at = item.mapToItem(parent.contentItem, 0, 0), margin = 12
        if (at.y < parent.contentY + margin) parent.contentY = Math.max(0, at.y - margin)
        else if (at.y + item.height > parent.contentY + parent.height - margin)
            parent.contentY = Math.max(0, Math.min(parent.contentHeight - parent.height, at.y + item.height - parent.height + margin))
        if (parent.contentX === undefined) continue
        if (at.x < parent.contentX) parent.contentX = Math.max(0, at.x)
        else if (at.x + item.width > parent.contentX + parent.width)
            parent.contentX = Math.max(0, Math.min(parent.contentWidth - parent.width, at.x + item.width - parent.width))
    }
}
