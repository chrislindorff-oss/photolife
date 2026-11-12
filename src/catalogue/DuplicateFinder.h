#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace pl::catalogue {

// Files in the library that are byte-for-byte the same (the same content
// hash) in more than one place: the same photo copied into two folders, say.
// Only finds them; PhotoLife never deletes photos.
struct DuplicateGroup
{
    QString contentHash;
    qint64 size = 0;          // of each copy
    QStringList paths;        // every copy, by path
    QList<int> captureIds;    // the photos they belong to, in the same order
    qint64 spareBytes() const { return size * (paths.size() - 1); }
};

// Every group, the most space taken by spare copies first. Reads only.
// Only copies `present` says are still there count (by default, those on
// disk): the catalogue can still list a file renamed or deleted outside
// PhotoLife, and that's no duplicate. How many such stale copies were left
// out goes in `*stale`.
QList<DuplicateGroup> findDuplicates(const QString &connectionName, QString *error = nullptr,
                                     const std::function<bool(const QString &path)> &present = {},
                                     int *stale = nullptr);

} // namespace pl::catalogue
