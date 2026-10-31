.pragma library

// The library counted into days, for the calendar: plain JavaScript, so
// tests/tst_days.qml checks it without Pinhole.

// A local date by its year, month and day, any year: Date's own takes 0 to
// 99 as 1900 to 1999.
function dateOf(year, month, day) {
    const date = new Date(2000, 0, 1)
    date.setFullYear(year, month, day)
    return date
}
// A day of its year, from 0.
function dayOf(date) {
    const y = date.getFullYear()
    const utc = (m, d) => {
        const t = new Date(0)
        t.setUTCFullYear(y, m, d)
        return t.getTime()
    }
    return Math.round((utc(date.getMonth(), date.getDate()) - utc(0, 1)) / 86400000)
}
function daysIn(year) {
    return (year % 4 === 0 && year % 100 !== 0) || year % 400 === 0 ? 366 : 365
}
// A day's row in its week, the week starting on `firstWeekday` (0 Sunday).
function weekdayRow(date, firstWeekday) {
    return (date.getDay() - firstWeekday + 7) % 7
}

// Counting `n` photos: given to count() a few at a time.
function start(n, firstWeekday) {
    return { at: 0, n: n, firstWeekday: firstWeekday, byYear: {} }
}
// Counts photos from where `state` is, `item(i)` giving each one's date (ms)
// and id, for up to `budget` ms; true once every photo is counted. A date
// of 0 is none (the library's NULL) and not counted; any other, before 1970
// too, is.
function count(state, item, budget) {
    const began = Date.now()
    let i = state.at
    for (; i < state.n; ++i) {
        if ((i & 63) === 0 && i > state.at && Date.now() - began >= budget)
            break
        const photo = item(i)
        const ms = photo.date
        if (ms === 0 || !isFinite(ms))
            continue
        const date = new Date(ms)
        const y = date.getFullYear()
        let year = state.byYear[y]
        if (!year) {
            year = state.byYear[y] = { year: y, days: daysIn(y), offset: weekdayRow(dateOf(y, 0, 1), state.firstWeekday),
                                       counts: new Array(366).fill(0), first: new Array(366).fill(""), total: 0 }
        }
        const day = dayOf(date)
        // The library is in time order: a day's first is its earliest. By
        // its id, as positions move when the library is read again.
        if (year.counts[day]++ === 0)
            year.first[day] = photo.id
        ++year.total
    }
    state.at = i
    return i >= state.n
}
// What was counted: every year with a photo, oldest first ({year, days,
// offset (the weekday row of 1 January), counts and first (each day's
// photos and its first photo's id, "" for none), total}), each year's row
// by the year, and how many photos light a square fully and make it glow.
function finish(state) {
    const years = Object.keys(state.byYear).map(y => state.byYear[y]).sort((a, b) => a.year - b.year)
    const rowOf = {}
    const busy = []
    let total = 0
    years.forEach((year, row) => {
        rowOf[year.year] = row
        total += year.total
        for (let d = 0; d < year.days; ++d)
            if (year.counts[d] > 0)
                busy.push(year.counts[d])
    })
    busy.sort((a, b) => a - b)
    const at = q => busy.length > 0 ? busy[Math.min(busy.length - 1, Math.floor(busy.length * q))] : 1
    const full = Math.max(2, at(0.97))
    return { years: years, rowOf: rowOf, total: total, dayCount: busy.length,
             full: full, glowing: Math.max(full + 1, at(0.995)) }
}

// The first day with photos (`way` -1) or the last (1): {year, day}, or
// null without any.
function edge(years, way) {
    if (years.length === 0)
        return null
    const y = years[way < 0 ? 0 : years.length - 1]
    let d = way < 0 ? 0 : y.days - 1
    while (d >= 0 && d < y.days && y.counts[d] === 0)
        d -= way
    return { year: y.year, day: Math.max(0, Math.min(y.days - 1, d)) }
}
// The row of the year nearest to `year`, -1 without any.
function nearest(years, year) {
    let best = -1
    years.forEach((y, row) => {
        if (best < 0 || Math.abs(y.year - year) < Math.abs(years[best].year - year))
            best = row
    })
    return best
}
