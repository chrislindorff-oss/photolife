#include "catalogue/DuplicateFinder.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>

namespace pl::catalogue {

QList<DuplicateGroup> findDuplicates(const QString &connectionName, QString *error,
                                     const std::function<bool(const QString &)> &present, int *stale)
{
    if (stale)
        *stale = 0;
    const auto there = present ? present : [](const QString &path) { return QFileInfo::exists(path); };
    QList<DuplicateGroup> out;
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT r.content_hash, r.path, COALESCE(r.file_size, 0), r.capture_id FROM rendition r "
            "WHERE r.content_hash IN (SELECT content_hash FROM rendition WHERE content_hash IS NOT NULL "
            "                         GROUP BY content_hash HAVING COUNT(*) > 1) "
            "ORDER BY r.content_hash, r.path"))) {
        if (error)
            *error = q.lastError().text();
        return out;
    }
    while (q.next()) {
        const QString hash = q.value(0).toString();
        if (!there(q.value(1).toString())) {
            if (stale)
                ++*stale;
            continue;
        }
        if (out.isEmpty() || out.last().contentHash != hash)
            out.append({hash, 0, {}, {}});
        DuplicateGroup &g = out.last();
        g.size = std::max(g.size, q.value(2).toLongLong());
        g.paths << q.value(1).toString();
        g.captureIds << q.value(3).toInt();
    }
    // A file whose other copies are gone has no duplicate any more.
    out.erase(std::remove_if(out.begin(), out.end(), [](const DuplicateGroup &g) { return g.paths.size() < 2; }),
              out.end());
    std::stable_sort(out.begin(), out.end(),
                     [](const DuplicateGroup &a, const DuplicateGroup &b) { return a.spareBytes() > b.spareBytes(); });
    return out;
}

} // namespace pl::catalogue
