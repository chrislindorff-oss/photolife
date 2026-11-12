#include "inat/IdentificationCheck.h"

#include <QCoreApplication>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>

namespace pl::inat {

QList<LinkedObservation> linkedObservations(const QString &connectionName)
{
    QList<LinkedObservation> out;
    QHash<qint64, int> byObservation;
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.setForwardOnly(true);
    // The photo's taxon as the grids show it: the user's decision first, else
    // the most confident match.
    if (!q.exec(QStringLiteral(
            "SELECT c.inat_observation_id, c.id, t.inat_id, t.name, t.common_name, t.ancestry, t.rank, "
            "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
            "  (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id ORDER BY (r.kind = 'raw'), r.id LIMIT 1) "
            "FROM capture c "
            "LEFT JOIN capture_match m ON m.id = (SELECT id FROM capture_match WHERE capture_id = c.id "
            "  ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
            "LEFT JOIN taxon t ON t.id = m.taxon_id "
            "WHERE c.inat_observation_id IS NOT NULL "
            "ORDER BY c.inat_observation_id, c.captured_on, c.id")))
        return out;
    while (q.next()) {
        const qint64 obs = q.value(0).toLongLong();
        auto it = byObservation.find(obs);
        if (it == byObservation.end()) {
            it = byObservation.insert(obs, int(out.size()));
            LinkedObservation l;
            l.observationId = obs;
            l.mineInatId = q.value(2).toLongLong();
            l.mineName = q.value(3).toString();
            l.mineCommonName = q.value(4).toString();
            for (const QString &part : q.value(5).toString().split(QLatin1Char('/'), Qt::SkipEmptyParts))
                l.mineAncestry << part.toLongLong();
            l.mineRank = q.value(6).toString();
            l.previewPath = q.value(7).toString();
            l.previewHash = q.value(8).toString();
            out << l;
        }
        out[*it].captureIds << q.value(1).toInt();
    }
    return out;
}

IdVerdict compareIds(const LinkedObservation &mine, const taxonomy::Observation &theirs)
{
    if (theirs.taxonInatId <= 0)
        return IdVerdict::NoName;
    if (mine.mineInatId <= 0)
        return IdVerdict::YouHadNone;
    if (mine.mineInatId == theirs.taxonInatId)
        return IdVerdict::Same;
    if (theirs.taxonAncestorIds.contains(mine.mineInatId))
        return IdVerdict::Finer;
    if (mine.mineAncestry.contains(theirs.taxonInatId))
        return IdVerdict::Coarser;
    return IdVerdict::Different;
}

QString verdictText(IdVerdict verdict)
{
    switch (verdict) {
    case IdVerdict::Same:
        return QCoreApplication::translate("pl::inat", "Agrees");
    case IdVerdict::Finer:
        return QCoreApplication::translate("pl::inat", "Narrowed down");
    case IdVerdict::Coarser:
        return QCoreApplication::translate("pl::inat", "Taken up a level");
    case IdVerdict::Different:
        return QCoreApplication::translate("pl::inat", "Different");
    case IdVerdict::YouHadNone:
        return QCoreApplication::translate("pl::inat", "Named on iNaturalist");
    case IdVerdict::NoName:
        return QCoreApplication::translate("pl::inat", "Not named yet");
    }
    return {};
}

} // namespace pl::inat
