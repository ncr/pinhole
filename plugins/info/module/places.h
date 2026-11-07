#pragma once

#include <QList>
#include <QObject>

// Places.js clusters(), the photos' grouping on the map, done here: in QML
// it took 176 ms for 28,000 photos, as the map opened and every half a
// step of a zoom. The same groups in the same order: each photo, in the
// order given, joins the nearest group begun within a marker's width of it
// (looked for in its cell of the world and the eight around), or begins
// one. Only which group each is in comes back; QML makes the groups.
class Places : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    // For each photo (its x and y on the world, 0 to 1), the number of its
    // group, numbered as they are begun, at `zoom`.
    Q_INVOKABLE QList<int> groupsAt(const QList<double> &xs, const QList<double> &ys, double zoom) const;
};
