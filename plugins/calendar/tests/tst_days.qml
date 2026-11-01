import QtQuick
import QtTest
import "../CalendarDays.js" as Days

// The counting and the days, without Pinhole: `run` beside this file.
TestCase {
    name: "Days"

    // A library of these local dates, in time order, ids by position, as
    // its columns: library.field("day") has each one's day, before 1970
    // too, and 0 for none.
    function library(dates) {
        return {
            days: dates.map(d => d === 0 ? 0 : d.getFullYear() * 10000 + (d.getMonth() + 1) * 100 + d.getDate()),
            ids: dates.map((d, i) => "p" + i)
        }
    }
    function counted(photos, hidden) {
        return { done: Days.finish(Days.count(photos.days, photos.ids, 1, hidden)) }
    }

    function test_dates() {
        compare(Days.dateOf(50, 0, 1).getFullYear(), 50)
        compare(Days.daysIn(2024), 366)
        compare(Days.daysIn(1900), 365)
        compare(Days.daysIn(2000), 366)
        compare(Days.dayOf(new Date(2024, 11, 31)), 365)
        compare(Days.dayOf(new Date(2025, 0, 1)), 0)
        compare(Days.dayOf(new Date(1965, 2, 1, 23, 30)), 59)
    }

    // Home and End: the first and the last day with photos, not 1 January
    // and 31 December.
    function test_edges() {
        const c = counted(library([new Date(2001, 2, 5, 10), new Date(2001, 2, 5, 11), new Date(2026, 9, 29, 18)])).done
        compare(Days.edge(c.years, -1), { year: 2001, day: Days.dayOf(new Date(2001, 2, 5)) })
        compare(Days.edge(c.years, 1), { year: 2026, day: Days.dayOf(new Date(2026, 9, 29)) })
        const ends = counted(library([new Date(2010, 0, 1, 9), new Date(2010, 11, 31, 23)])).done
        compare(Days.edge(ends.years, -1).day, 0)
        compare(Days.edge(ends.years, 1).day, 364)
        compare(Days.edge([], 1), null)
    }

    // Old scans keep their years; a photo without a date is not counted.
    function test_before1970() {
        const c = counted(library([0, new Date(1965, 5, 1, 12), new Date(1969, 11, 31, 20), new Date(1970, 0, 1, 0, 30), new Date(2020, 0, 1)])).done
        compare(c.years.map(y => y.year), [1965, 1969, 1970, 2020])
        compare(c.total, 4)
        compare(c.rowOf[1969], 1)
        compare(c.years[1].counts[364], 1)
    }

    // A day's first photo is kept by id, the earliest of the day.
    function test_firstById() {
        const c = counted(library([new Date(2022, 3, 2, 8), new Date(2022, 3, 2, 9), new Date(2022, 3, 3, 9)])).done
        const y = c.years[0]
        compare(y.counts[Days.dayOf(new Date(2022, 3, 2))], 2)
        compare(y.first[Days.dayOf(new Date(2022, 3, 2))], "p0")
        compare(y.first[Days.dayOf(new Date(2022, 3, 3))], "p2")
        compare(y.first[0], "")
    }

    // Every day of a leap year and of the year after, from its yyyymmdd.
    function test_dayOfYear() {
        const dates = []
        for (let d = 0; d < 366 + 365; ++d)
            dates.push(new Date(2024, 0, 1 + d, 12))
        const c = counted(library(dates)).done
        compare(c.total, 731)
        compare(c.dayCount, 731)
        compare(c.years[0].first[59], "p59")
        compare(c.years[0].first[365], "p365")
        compare(c.years[1].first[0], "p366")
        compare(c.years[1].first[364], "p730")
    }

    // A photo hidden from the grid is not counted, nor its day's first.
    function test_hidden() {
        const c = counted(library([new Date(2022, 3, 2, 8), new Date(2022, 3, 2, 9), new Date(2022, 3, 3, 9)]),
                          i => i === 0 || i === 2).done
        const day = Days.dayOf(new Date(2022, 3, 2))
        compare(c.total, 1)
        compare(c.years[0].counts[day], 1)
        compare(c.years[0].first[day], "p1")
        compare(c.dayCount, 1)
        compare(counted(library([new Date(2022, 3, 2, 8)]), i => true).done.years.length, 0)
    }

    function test_nearest() {
        const c = counted(library([new Date(2003, 0, 1), new Date(2010, 0, 1), new Date(2020, 0, 1)])).done
        compare(Days.nearest(c.years, 2009), 1)
        compare(Days.nearest(c.years, 1990), 0)
        compare(Days.nearest(c.years, 2030), 2)
        compare(Days.nearest([], 2030), -1)
    }
}
