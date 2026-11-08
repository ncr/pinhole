#include "places.h"

#include <QHash>

#include <cmath>
#include <vector>

QList<int> Places::groupsAt(const QList<double> &xs, const QList<double> &ys, double zoom) const {
    const qsizetype count = std::min(xs.size(), ys.size());
    const double size = 256 * std::pow(2.0, zoom), radius = 74;
    // Its columns go round, as the world does (Places.js clusters()).
    const qint64 columns = std::max<qint64>(1, qint64(std::ceil(size / radius)));
    const auto column = [columns](qint64 i) { return ((i % columns) + columns) % columns; };
    struct Group {
        double sx, sy;
    };
    std::vector<Group> groups;
    // A cell's groups, by the cell (column, row).
    QHash<quint64, std::vector<int>> bins;
    const auto key = [](qint64 column, qint64 row) { return (quint64(quint32(qint32(row))) << 32) | quint32(qint32(column)); };
    QList<int> of(count);
    for (qsizetype n = 0; n < count; ++n) {
        const double x = xs[n] * size, y = ys[n] * size;
        const qint64 bx = column(qint64(std::floor(x / radius))), by = qint64(std::floor(y / radius));
        int near = -1;
        double distance = radius * radius;
        for (qint64 j = by - 1; j <= by + 1; ++j)
            for (qint64 i = bx - 1; i <= bx + 1; ++i) {
                const auto cell = bins.constFind(key(column(i), j));
                if (cell == bins.cend())
                    continue;
                for (const int g : *cell) {
                    double dx = groups[size_t(g)].sx - x;
                    dx -= size * std::round(dx / size);
                    const double dy = groups[size_t(g)].sy - y;
                    const double d = dx * dx + dy * dy;
                    if (d < distance) {
                        near = g;
                        distance = d;
                    }
                }
            }
        if (near < 0) {
            near = int(groups.size());
            groups.push_back({x, y});
            bins[key(bx, by)].push_back(near);
        }
        of[n] = near;
    }
    return of;
}
