#include "coverage/CoverageCalculator.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>

#include <functional>
#include <optional>

namespace pl::coverage {
namespace {

struct Node
{
    qint64 inatId = 0;
    std::optional<qint64> parentInatId;
    QString rank;
    QString name;
    QString commonName;
    QString status;
    int directCaptures = 0;
    QString directNewest;
};

bool isSpeciesUnit(const QString &rank)
{
    return rank.compare(QLatin1String("species"), Qt::CaseInsensitive) == 0;
}

QString maxDate(const QString &a, const QString &b)
{
    if (a.isEmpty())
        return b;
    if (b.isEmpty())
        return a;
    return a >= b ? a : b;
}

} // namespace

ProjectCoverage computeCoverage(const QString &connectionName, int projectId)
{
    ProjectCoverage coverage;
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isOpen())
        return coverage;

    QHash<qint64, Node> nodes;

    // Project taxa.
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare(QStringLiteral(
            "SELECT t.inat_id, t.parent_inat_id, t.rank, t.name, t.common_name "
            "FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id WHERE pt.project_id = ?"));
        q.addBindValue(projectId);
        if (!q.exec())
            return coverage;
        while (q.next()) {
            Node n;
            n.inatId = q.value(0).toLongLong();
            if (!q.value(1).isNull())
                n.parentInatId = q.value(1).toLongLong();
            n.rank = q.value(2).toString();
            n.name = q.value(3).toString();
            n.commonName = q.value(4).toString();
            nodes.insert(n.inatId, n);
        }
    }
    if (nodes.isEmpty())
        return coverage;

    // Conservation status (any place; prefer a listed status over a blank one).
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.exec(QStringLiteral(
            "SELECT t.inat_id, ts.status FROM taxon_status ts JOIN taxon t ON t.id = ts.taxon_id "
            "WHERE ts.status <> '' ORDER BY ts.id"));
        while (q.next()) {
            const qint64 id = q.value(0).toLongLong();
            if (auto it = nodes.find(id); it != nodes.end() && it->status.isEmpty())
                it->status = q.value(1).toString();
        }
    }

    // Direct matched captures per taxon (auto or user-confirmed).
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.exec(QStringLiteral(
            "SELECT t.inat_id, COUNT(*), MAX(c.captured_on) "
            "FROM capture_match m JOIN capture c ON c.id = m.capture_id "
            "JOIN taxon t ON t.id = m.taxon_id "
            "WHERE m.status IN ('auto', 'confirmed') GROUP BY t.inat_id"));
        while (q.next()) {
            const qint64 id = q.value(0).toLongLong();
            if (auto it = nodes.find(id); it != nodes.end()) {
                it->directCaptures = q.value(1).toInt();
                it->directNewest = q.value(2).toString();
            }
        }
    }

    // Children index.
    QHash<qint64, QList<qint64>> children;
    QList<qint64> roots;
    for (const Node &n : nodes) {
        if (n.parentInatId && nodes.contains(*n.parentInatId))
            children[*n.parentInatId].append(n.inatId);
        else
            roots.append(n.inatId);
    }

    // Post-order rollup.
    QSet<qint64> visiting;
    std::function<TaxonCoverage(qint64)> roll = [&](qint64 id) -> TaxonCoverage {
        const Node &n = nodes.value(id);
        TaxonCoverage tc;
        tc.inatId = id;
        tc.rank = n.rank;
        tc.name = n.name;
        tc.commonName = n.commonName;
        tc.status = n.status;
        tc.hasOwnPhotos = n.directCaptures > 0;
        tc.captureCount = n.directCaptures;
        tc.newestCapture = n.directNewest;
        tc.subtreeHasPhotos = tc.hasOwnPhotos;

        if (!visiting.contains(id)) {
            visiting.insert(id);
            for (qint64 childId : children.value(id)) {
                const TaxonCoverage child = roll(childId);
                tc.speciesTotal += child.speciesTotal;
                tc.speciesWithPhotos += child.speciesWithPhotos;
                tc.threatenedTotal += child.threatenedTotal;
                tc.threatenedWithPhotos += child.threatenedWithPhotos;
                tc.captureCount += child.captureCount;
                tc.newestCapture = maxDate(tc.newestCapture, child.newestCapture);
                tc.subtreeHasPhotos = tc.subtreeHasPhotos || child.subtreeHasPhotos;
            }
        }

        if (isSpeciesUnit(n.rank)) {
            tc.speciesTotal += 1;
            if (tc.subtreeHasPhotos)
                tc.speciesWithPhotos += 1;
            if (!n.status.isEmpty()) {
                tc.threatenedTotal += 1;
                if (tc.subtreeHasPhotos)
                    tc.threatenedWithPhotos += 1;

                ProjectCoverage::Tier &tier = coverage.byStatus[n.status];
                tier.total += 1;
                if (tc.subtreeHasPhotos)
                    tier.withPhotos += 1;
            }
        }

        coverage.byTaxon.insert(id, tc);
        return tc;
    };

    for (qint64 rootId : roots) {
        const TaxonCoverage rc = roll(rootId);
        coverage.speciesTotal += rc.speciesTotal;
        coverage.speciesWithPhotos += rc.speciesWithPhotos;
        coverage.threatenedTotal += rc.threatenedTotal;
        coverage.threatenedWithPhotos += rc.threatenedWithPhotos;
        coverage.captureCount += rc.captureCount;
        coverage.newestCapture = maxDate(coverage.newestCapture, rc.newestCapture);
    }

    return coverage;
}

int pickRepresentatives(const QString &connectionName, int projectId)
{
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isOpen())
        return 0;

    struct Best
    {
        qint64 captureId = 0;
        double confidence = -1.0;
        QString capturedOn;

        bool betterThan(const Best &o) const
        {
            if (confidence != o.confidence)
                return confidence > o.confidence;
            return capturedOn > o.capturedOn;
        }
    };

    QHash<qint64, std::optional<qint64>> parentOf;   // inat_id -> parent inat_id
    QHash<qint64, Best> direct;

    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare(QStringLiteral(
            "SELECT t.inat_id, t.parent_inat_id FROM project_taxon pt "
            "JOIN taxon t ON t.id = pt.taxon_id WHERE pt.project_id = ?"));
        q.addBindValue(projectId);
        if (!q.exec())
            return 0;
        while (q.next()) {
            const qint64 id = q.value(0).toLongLong();
            parentOf.insert(id, q.value(1).isNull() ? std::optional<qint64>()
                                                    : std::optional<qint64>(q.value(1).toLongLong()));
        }
    }
    if (parentOf.isEmpty())
        return 0;

    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.exec(QStringLiteral(
            "SELECT t.inat_id, m.capture_id, m.confidence, c.captured_on "
            "FROM capture_match m JOIN capture c ON c.id = m.capture_id "
            "JOIN taxon t ON t.id = m.taxon_id "
            "WHERE m.status IN ('auto', 'confirmed')"));
        while (q.next()) {
            const qint64 id = q.value(0).toLongLong();
            if (!parentOf.contains(id))
                continue;
            Best b{q.value(1).toLongLong(), q.value(2).toDouble(), q.value(3).toString()};
            auto it = direct.find(id);
            if (it == direct.end() || b.betterThan(*it))
                direct.insert(id, b);
        }
    }

    QHash<qint64, QList<qint64>> children;
    QList<qint64> roots;
    for (auto it = parentOf.constBegin(); it != parentOf.constEnd(); ++it) {
        if (it.value() && parentOf.contains(*it.value()))
            children[*it.value()].append(it.key());
        else
            roots.append(it.key());
    }

    QHash<qint64, Best> rollup;
    QSet<qint64> visiting;
    std::function<Best(qint64)> roll = [&](qint64 id) -> Best {
        Best best = direct.value(id);
        if (!visiting.contains(id)) {
            visiting.insert(id);
            for (qint64 child : children.value(id)) {
                const Best cb = roll(child);
                if (cb.captureId > 0 && cb.betterThan(best))
                    best = cb;
            }
        }
        if (best.captureId > 0)
            rollup.insert(id, best);
        return best;
    };
    for (qint64 r : roots)
        roll(r);

    if (!db.transaction())
        return 0;
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM representative WHERE project_id = ?"));
    del.addBindValue(projectId);
    del.exec();

    int written = 0;
    for (auto it = rollup.constBegin(); it != rollup.constEnd(); ++it) {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO representative (project_id, taxon_id, capture_id) "
            "VALUES (?, (SELECT id FROM taxon WHERE inat_id = ?), ?)"));
        ins.addBindValue(projectId);
        ins.addBindValue(qlonglong(it.key()));
        ins.addBindValue(qlonglong(it.value().captureId));
        if (ins.exec())
            ++written;
    }
    db.commit();
    return written;
}

RepresentativeImage representativeFor(const QString &connectionName, int projectId,
                                      qint64 taxonInatId)
{
    RepresentativeImage image;
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT c.id, "
        "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id "
        "     ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
        "  (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id "
        "     ORDER BY (r.kind = 'raw'), r.id LIMIT 1) "
        "FROM representative rep JOIN capture c ON c.id = rep.capture_id "
        "JOIN taxon t ON t.id = rep.taxon_id "
        "WHERE rep.project_id = ? AND t.inat_id = ?"));
    q.addBindValue(projectId);
    q.addBindValue(qlonglong(taxonInatId));
    if (q.exec() && q.next()) {
        image.captureId = q.value(0).toLongLong();
        image.previewPath = q.value(1).toString();
        image.previewHash = q.value(2).toString();
    }
    return image;
}

} // namespace pl::coverage
