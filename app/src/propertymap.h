#pragma once

#include <QQmlPropertyMap>
#include <QVariantHash>

// Many keys into a QQmlPropertyMap at once. One at a time, each key it has
// not had makes its type again with every property so far: People's 9627
// names took 34 seconds as Pinhole started. All at once, it is made once.
//
// Qt's insert of many takes none of them if one is a name it allows no
// property by ("keys", say): those known go alone, said and skipped, and
// should another be refused, each half is tried, so the rest still go in.
inline void insertAll(QQmlPropertyMap *map, QVariantHash values) {
    // Those Qt is known to refuse, said once each, before the rest.
    for (const char *refused : {"keys", "valueChanged", "QObject", "destroyed", "deleteLater"})
        if (const auto it = values.constFind(QLatin1String(refused)); it != values.cend()) {
            map->insert(it.key(), it.value());
            values.erase(it);
        }
    if (values.isEmpty())
        return;
    if (values.size() == 1) {
        map->insert(values.begin().key(), values.begin().value());
        return;
    }
    map->insert(values);
    if (map->contains(values.begin().key()))
        return;
    QVariantHash first, second;
    int i = 0;
    for (auto it = values.begin(); it != values.end(); ++it, ++i)
        (i < values.size() / 2 ? first : second).insert(it.key(), it.value());
    insertAll(map, first);
    insertAll(map, second);
}
