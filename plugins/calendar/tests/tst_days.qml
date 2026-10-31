import QtQuick
import QtTest
import "../Days.js" as Days

// The counting and the days, without Pinhole: `run` beside this file.
TestCase {
    name: "Days"

    // A library of these local dates, in time order, ids by position.
    function library(dates) {
        return dates.map((d, i) => ({ date: d === 0 ? 0 : d.getTime(), id: "p" + i }))
    }
    function counted(photos, budget) {
        const state = Days.start(photos.length, 1)
        let rounds = 0
        while (!Days.count(state, i => photos[i], budget))
            ++rounds
        return { done: Days.finish(state), rounds: rounds }
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
        const c = counted(library([new Date(2001, 2, 5, 10), new Date(2001, 2, 5, 11), new Date(2026, 9, 29, 18)]), 4).done
        compare(Days.edge(c.years, -1), { year: 2001, day: Days.dayOf(new Date(2001, 2, 5)) })
        compare(Days.edge(c.years, 1), { year: 2026, day: Days.dayOf(new Date(2026, 9, 29)) })
        const ends = counted(library([new Date(2010, 0, 1, 9), new Date(2010, 11, 31, 23)]), 4).done
        compare(Days.edge(ends.years, -1).day, 0)
        compare(Days.edge(ends.years, 1).day, 364)
        compare(Days.edge([], 1), null)
    }

    // Old scans keep their years; a photo without a date is not counted.
    function test_before1970() {
        const c = counted(library([0, new Date(1965, 5, 1, 12), new Date(1969, 11, 31, 20), new Date(1970, 0, 1, 0, 30), new Date(2020, 0, 1)]), 4).done
        compare(c.years.map(y => y.year), [1965, 1969, 1970, 2020])
        compare(c.total, 4)
        compare(c.rowOf[1969], 1)
        compare(c.years[1].counts[364], 1)
    }

    // A day's first photo is kept by id, the earliest of the day.
    function test_firstById() {
        const c = counted(library([new Date(2022, 3, 2, 8), new Date(2022, 3, 2, 9), new Date(2022, 3, 3, 9)]), 4).done
        const y = c.years[0]
        compare(y.counts[Days.dayOf(new Date(2022, 3, 2))], 2)
        compare(y.first[Days.dayOf(new Date(2022, 3, 2))], "p0")
        compare(y.first[Days.dayOf(new Date(2022, 3, 3))], "p2")
        compare(y.first[0], "")
    }

    // With no time to spare, still on, a little each round, to the end.
    function test_budget() {
        const dates = []
        for (let i = 0; i < 1000; ++i)
            dates.push(new Date(2015, 0, 1 + Math.floor(i / 3), 12))
        const run = counted(library(dates), 0)
        compare(run.done.total, 1000)
        verify(run.rounds >= 1000 / 64 - 1, "rounds: " + run.rounds)
    }

    function test_nearest() {
        const c = counted(library([new Date(2003, 0, 1), new Date(2010, 0, 1), new Date(2020, 0, 1)]), 4).done
        compare(Days.nearest(c.years, 2009), 1)
        compare(Days.nearest(c.years, 1990), 0)
        compare(Days.nearest(c.years, 2030), 2)
        compare(Days.nearest([], 2030), -1)
    }
}
