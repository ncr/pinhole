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

// The days before each month, in a year that is not a leap year.
const monthStarts = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334]

// Counts the library into days, at once: `days` is each photo's local day
// as yyyymmdd (library.field("day"), before 1970 too; 0 for one with no
// date, not counted), `ids` its id. `hidden(i)`, if given, is true for a
// photo hidden from the grid, which is not counted, as the grid does not
// show it.
function count(days, ids, firstWeekday, hidden) {
    const byYear = {}
    let year = null
    for (let i = 0; i < days.length; ++i) {
        const key = days[i]
        if (key === 0)
            continue
        const y = Math.floor(key / 10000)
        const m = Math.floor(key / 100) % 100
        const day = monthStarts[m - 1] + key % 100 - 1 + (m > 2 && daysIn(y) === 366 ? 1 : 0)
        if (hidden && hidden(i))
            continue
        if (!year || year.year !== y) {
            year = byYear[y]
            if (!year)
                year = byYear[y] = { year: y, days: daysIn(y), offset: weekdayRow(dateOf(y, 0, 1), firstWeekday),
                                     counts: new Array(366).fill(0), first: new Array(366).fill(""), total: 0 }
        }
        // The library is in time order: a day's first is its earliest. By
        // its id, as positions move when the library is read again.
        if (year.counts[day]++ === 0)
            year.first[day] = ids[i]
        ++year.total
    }
    return { byYear: byYear }
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
