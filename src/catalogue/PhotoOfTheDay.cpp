#include "catalogue/PhotoOfTheDay.h"

#include "net/INatParse.h"

#include <QFileInfo>
#include <QHash>
#include <QRandomGenerator>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>

namespace pl::catalogue {

namespace {

// Identified photos with a preview file: the capture, its best match
// (confirmed first, then the user's, then the most confident) and file.
const QString kPhotos = QStringLiteral(
    "SELECT c.id, r.path, r.content_hash, (b.capture_id IS NOT NULL), t.inat_id, t.name, t.common_name, t.rank, "
    "  c.captured_on, c.latitude, c.longitude, f.path "
    "FROM capture c "
    "JOIN capture_match m ON m.id = (SELECT id FROM capture_match WHERE capture_id = c.id "
    "  AND status <> 'rejected' AND taxon_id IS NOT NULL "
    "  ORDER BY (status = 'confirmed') DESC, (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
    "JOIN taxon t ON t.id = m.taxon_id "
    "JOIN rendition r ON r.id = (SELECT id FROM rendition WHERE capture_id = c.id ORDER BY (kind = 'raw'), id LIMIT 1) "
    "LEFT JOIN folder f ON f.id = c.folder_id "
    "LEFT JOIN best_shot b ON b.capture_id = c.id "
    "WHERE m.status IN ('confirmed', 'auto')");

PhotoOfTheDay::Pick read(const QSqlQuery &q)
{
    PhotoOfTheDay::Pick p;
    p.captureId = q.value(0).toInt();
    p.path = q.value(1).toString();
    p.contentHash = q.value(2).toString();
    p.bestShot = q.value(3).toBool();
    p.taxonInatId = q.value(4).toLongLong();
    p.name = q.value(5).toString();
    p.commonName = q.value(6).toString();
    p.rank = q.value(7).toString();
    p.capturedOn = q.value(8).toString();
    if (!q.value(9).isNull() && !q.value(10).isNull()) {
        p.latitude = q.value(9).toDouble();
        p.longitude = q.value(10).toDouble();
    }
    p.folder = q.value(11).toString();
    return p;
}

} // namespace

std::optional<PhotoOfTheDay::Pick> PhotoOfTheDay::choose(const QString &connectionName, const QDate &today,
                                                         const State &state, const Available &available)
{
    const auto usable = [&](const QString &path) {
        return !path.isEmpty() && (available ? available(path) : QFileInfo::exists(path));
    };
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);

    // Still today's.
    if (state.day == today && state.captureId > 0) {
        QSqlQuery q(db);
        q.prepare(kPhotos + QStringLiteral(" AND c.id = ?"));
        q.addBindValue(state.captureId);
        if (q.exec() && q.next()) {
            Pick p = read(q);
            if (usable(p.path)) {
                p.state = state;
                return p;
            }
        }
    }

    // Everything that qualifies, by id: best shots, then the rest.
    QList<int> best, rest;
    QSqlQuery all(db);
    all.setForwardOnly(true);
    if (!all.exec(kPhotos))
        return std::nullopt;
    while (all.next())
        (all.value(3).toBool() ? best : rest) << all.value(0).toInt();
    if (best.isEmpty() && rest.isEmpty())
        return std::nullopt;

    QList<int> shown = state.shown;
    if (state.captureId > 0 && !shown.contains(state.captureId))
        shown << state.captureId;   // yesterday's has had its day
    const QSet<int> had(shown.cbegin(), shown.cend());
    const auto fresh = [&](const QList<int> &ids) {
        QList<int> out;
        for (int id : ids)
            if (!had.contains(id))
                out << id;
        return out;
    };

    // Unshown best shots, then other unshown photos -- and when none of
    // those can be shown (all had their day, or their files aren't here),
    // start again: every best shot, then every photo.
    QSqlQuery one(db);
    one.prepare(kPhotos + QStringLiteral(" AND c.id = ?"));
    const QList<QList<QList<int>>> rounds{{fresh(best), fresh(rest)}, {best, rest}};
    for (int round = 0; round < rounds.size(); ++round) {
        if (round == 1)
            shown.clear();
        for (QList<int> pool : rounds.at(round)) {
            // A few at random, in case some files aren't here.
            for (int tries = 0; tries < 20 && !pool.isEmpty(); ++tries) {
                const int id = pool.takeAt(int(QRandomGenerator::global()->bounded(qsizetype(pool.size()))));
                one.addBindValue(id);
                if (!one.exec() || !one.next())
                    continue;
                Pick p = read(one);
                if (!usable(p.path))
                    continue;
                shown.removeAll(id);
                p.state = {today, id, shown};
                return p;
            }
        }
    }
    return std::nullopt;
}

QList<PhotoOfTheDay::LineageStep> PhotoOfTheDay::lineage(const QString &connectionName, qint64 taxonInatId)
{
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT ancestry FROM taxon WHERE inat_id = ?"));
    q.addBindValue(taxonInatId);
    if (!q.exec() || !q.next())
        return {};
    QList<qint64> ids = net::inat::ancestryIds(q.value(0).toString());
    ids.removeAll(taxonInatId);
    ids << taxonInatId;

    QStringList list;
    for (qint64 id : std::as_const(ids))
        list << QString::number(id);
    QHash<qint64, LineageStep> known;
    QSqlQuery names(db);
    names.setForwardOnly(true);
    if (names.exec(QStringLiteral("SELECT inat_id, rank, name, common_name FROM taxon WHERE inat_id IN (%1)")
                       .arg(list.join(QLatin1Char(','))))) {
        while (names.next())
            known.insert(names.value(0).toLongLong(), {names.value(0).toLongLong(), names.value(1).toString(),
                                                       names.value(2).toString(), names.value(3).toString()});
    }
    QList<LineageStep> steps;
    for (qint64 id : std::as_const(ids)) {
        LineageStep step = known.value(id);
        step.inatId = id;
        steps << step;
    }
    return steps;
}

} // namespace pl::catalogue
