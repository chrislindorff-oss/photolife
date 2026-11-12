#include "coverage/LifeList.h"

#include "taxonomy/TaxonomyStore.h"

#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace pl::coverage {

namespace {

constexpr int kMaxBindParams = 900;   // under SQLite's old 999 limit

bool isSpecies(const QString &rank)
{
    return rank.compare(QLatin1String("species"), Qt::CaseInsensitive) == 0;
}

struct Tally
{
    QString firstOn;
    int captureId = 0;   // the first dated photo
    int lowestCapture = 0;
    QSet<int> captures;
};

void count(Tally &t, const LifeListSighting &s, bool usable)
{
    t.captures.insert(s.captureId);
    if (t.lowestCapture == 0 || s.captureId < t.lowestCapture)
        t.lowestCapture = s.captureId;
    if (!usable)
        return;
    if (t.captureId == 0 || s.capturedOn < t.firstOn || (s.capturedOn == t.firstOn && s.captureId < t.captureId)) {
        t.firstOn = s.capturedOn;
        t.captureId = s.captureId;
    }
}

} // namespace

bool isUsableLifeListDate(const QString &capturedOn, const QDate &today)
{
    if (capturedOn.isEmpty())
        return false;
    const QDate d = QDate::fromString(capturedOn.left(10), Qt::ISODate);
    return d.isValid() && d <= today;
}

QList<LifeListEntry> buildLifeList(const QList<LifeListNode> &nodes, const QList<LifeListSighting> &sightings,
                                   const QDate &today)
{
    QHash<qint64, LifeListNode> byId;
    for (const LifeListNode &n : nodes)
        byId.insert(n.inatId, n);

    // The species a taxon counts toward: itself, or its nearest ancestor of
    // rank species (0 when neither -- a genus, or a hybrid of genera).
    QHash<qint64, qint64> speciesOf;
    const auto speciesFor = [&](qint64 id) {
        if (auto it = speciesOf.constFind(id); it != speciesOf.constEnd())
            return *it;
        qint64 found = 0;
        QSet<qint64> seen;
        for (qint64 at = id; at > 0 && byId.contains(at) && !seen.contains(at); at = byId[at].parentInatId) {
            seen.insert(at);
            if (isSpecies(byId[at].rank)) {
                found = at;
                break;
            }
        }
        speciesOf.insert(id, found);
        return found;
    };

    QHash<qint64, Tally> tallies;
    for (const LifeListSighting &s : sightings) {
        const auto node = byId.constFind(s.taxonInatId);
        if (node == byId.constEnd())
            continue;
        const bool usable = isUsableLifeListDate(s.capturedOn, today);
        if (taxonomy::TaxonomyStore::isBelowSpeciesRank(node->rank))
            count(tallies[s.taxonInatId], s, usable);
        const qint64 species = speciesFor(s.taxonInatId);
        if (species > 0)
            count(tallies[species], s, usable);
    }

    QList<LifeListEntry> entries;
    entries.reserve(tallies.size());
    for (auto it = tallies.cbegin(); it != tallies.cend(); ++it) {
        const LifeListNode &n = byId[it.key()];
        LifeListEntry e;
        e.inatId = n.inatId;
        e.name = n.name;
        e.commonName = n.commonName;
        e.rank = n.rank;
        e.belowSpecies = taxonomy::TaxonomyStore::isBelowSpeciesRank(n.rank);
        e.firstOn = it->firstOn;
        e.captureId = it->captureId > 0 ? it->captureId : it->lowestCapture;
        e.photoCount = int(it->captures.size());
        entries << e;
    }
    std::sort(entries.begin(), entries.end(), [](const LifeListEntry &a, const LifeListEntry &b) {
        const bool aDated = !a.firstOn.isEmpty(), bDated = !b.firstOn.isEmpty();
        if (aDated != bDated)
            return aDated;
        if (aDated) {
            if (a.firstOn != b.firstOn)
                return a.firstOn < b.firstOn;
            if (a.captureId != b.captureId)
                return a.captureId < b.captureId;
            // The same photo first for a species and its subspecies: the species first.
            if (a.belowSpecies != b.belowSpecies)
                return !a.belowSpecies;
        }
        const int byName = a.name.compare(b.name, Qt::CaseInsensitive);
        if (byName != 0)
            return byName < 0;
        return a.inatId < b.inatId;
    });
    return entries;
}

QList<LifeListEntry> numberedLifeList(const QList<LifeListEntry> &all, bool includeBelow)
{
    QList<LifeListEntry> shown;
    int n = 0;
    for (LifeListEntry e : all) {
        if (e.belowSpecies && !includeBelow)
            continue;
        e.number = e.firstOn.isEmpty() ? 0 : ++n;
        shown << e;
    }
    return shown;
}

namespace {
bool matchesPlace(const QString &place, const QStringList &words)
{
    for (const QString &w : words) {
        if (!place.contains(w, Qt::CaseInsensitive))
            return false;
    }
    return true;
}
} // namespace

QList<LifeListEntry> loadLifeList(const QString &connectionName, int projectId, const QDate &today,
                                  const LifeListFilter &filter)
{
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (!db.isOpen() || projectId <= 0)
        return {};

    QList<LifeListNode> nodes;
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare(QStringLiteral(
            "SELECT t.inat_id, t.parent_inat_id, t.rank, t.name, t.common_name, t.ancestry "
            "FROM project_taxon pt JOIN taxon t ON t.id = pt.taxon_id WHERE pt.project_id = ?"));
        q.addBindValue(projectId);
        if (!q.exec())
            return {};
        QStringList ancestries;
        while (q.next()) {
            LifeListNode n;
            n.inatId = q.value(0).toLongLong();
            n.parentInatId = q.value(1).isNull() ? 0 : q.value(1).toLongLong();
            n.rank = q.value(2).toString();
            n.name = q.value(3).toString();
            n.commonName = q.value(4).toString();
            nodes << n;
            ancestries << q.value(5).toString();
        }
        // A parent not in the tree: its nearest ancestor that is (see parentWithin).
        QSet<qint64> present;
        for (const LifeListNode &n : nodes)
            present.insert(n.inatId);
        for (int i = 0; i < nodes.size(); ++i) {
            const auto parent = taxonomy::TaxonomyStore::parentWithin(
                nodes[i].parentInatId > 0 ? std::optional<qint64>(nodes[i].parentInatId) : std::nullopt,
                ancestries.at(i), present);
            nodes[i].parentInatId = parent.value_or(0);
        }
    }
    QList<LifeListSighting> sightings;
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        const bool byPlace = !filter.place.trimmed().isEmpty();
        q.prepare(QStringLiteral(
                      "SELECT t.inat_id, m.capture_id, c.captured_on%1 "
                      "FROM capture_match m JOIN capture c ON c.id = m.capture_id "
                      "JOIN taxon t ON t.id = m.taxon_id "
                      "JOIN project_taxon pt ON pt.taxon_id = m.taxon_id AND pt.project_id = ? %2"
                      "WHERE m.status IN ('auto', 'confirmed')")
                      .arg(byPlace ? QStringLiteral(", COALESCE(g.locality, ''), COALESCE(c.locality_text, '')") : QString(),
                           byPlace ? QStringLiteral("LEFT JOIN geocode_cache g ON g.lat_round = ROUND(CAST(c.latitude AS NUMERIC), 3) "
                                                    "AND g.lon_round = ROUND(CAST(c.longitude AS NUMERIC), 3) ")
                                   : QString()));
        q.addBindValue(projectId);
        if (!q.exec())
            return {};
        const QStringList words = filter.place.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        while (q.next()) {
            const QString on = q.value(2).toString();
            if (filter.from.isValid()) {
                // In the period, by its date (an unusable one never is).
                if (!isUsableLifeListDate(on, today))
                    continue;
                const QDate day = QDate::fromString(on.left(10), Qt::ISODate);
                if (!day.isValid() || day < filter.from || (filter.to.isValid() && day > filter.to))
                    continue;
            }
            if (byPlace && !matchesPlace(q.value(3).toString() + QLatin1Char(' ') + q.value(4).toString(), words))
                continue;
            sightings.append({q.value(0).toLongLong(), q.value(1).toInt(), on});
        }
    }

    QList<LifeListEntry> entries = buildLifeList(nodes, sightings, today);

    // The first photos' previews and places, a chunk at a time.
    QHash<int, QList<int>> rowsByCapture;
    for (int i = 0; i < entries.size(); ++i)
        rowsByCapture[entries[i].captureId] << i;
    const QList<int> ids = rowsByCapture.keys();
    for (qsizetype from = 0; from < ids.size(); from += kMaxBindParams) {
        const QList<int> chunk = ids.mid(from, kMaxBindParams);
        QStringList marks;
        marks.fill(QStringLiteral("?"), chunk.size());
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare(QStringLiteral(
                      "SELECT c.id, "
                      "  (SELECT r.path FROM rendition r WHERE r.capture_id = c.id "
                      "     ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
                      "  (SELECT r.content_hash FROM rendition r WHERE r.capture_id = c.id "
                      "     ORDER BY (r.kind = 'raw'), r.id LIMIT 1), "
                      "  g.locality, c.locality_text, cc.capture_id, cc.camera, cc.focal_length "
                      "FROM capture c "
                      "LEFT JOIN geocode_cache g ON g.lat_round = ROUND(CAST(c.latitude AS NUMERIC), 3) "
                      "  AND g.lon_round = ROUND(CAST(c.longitude AS NUMERIC), 3) "
                      "LEFT JOIN capture_camera cc ON cc.capture_id = c.id "
                      "WHERE c.id IN (%1)")
                      .arg(marks.join(QLatin1Char(','))));
        for (int id : chunk)
            q.addBindValue(id);
        if (!q.exec())
            continue;
        while (q.next()) {
            const QString geocoded = q.value(3).toString();
            const QString place = geocoded.isEmpty() ? q.value(4).toString() : geocoded;
            for (int row : rowsByCapture.value(q.value(0).toInt())) {
                entries[row].previewPath = q.value(1).toString();
                entries[row].previewHash = q.value(2).toString();
                entries[row].locality = place;
                entries[row].cameraChecked = !q.value(5).isNull();
                entries[row].camera = q.value(6).toString();
                if (!q.value(7).isNull())
                    entries[row].focalLength = q.value(7).toDouble();
            }
        }
    }
    return entries;
}

QList<int> speciesByMonth(const QString &connectionName, int projectId)
{
    QList<int> months(12, 0);
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.setForwardOnly(true);
    // A subspecies counts for its species: its parent, when that's a species.
    q.prepare(QStringLiteral(
        "SELECT DISTINCT CASE WHEN t.rank = 'species' THEN t.inat_id ELSE COALESCE(p.inat_id, t.inat_id) END, "
        "       SUBSTR(c.captured_on, 6, 2) "
        "FROM capture_match m JOIN capture c ON c.id = m.capture_id "
        "JOIN taxon t ON t.id = m.taxon_id "
        "JOIN project_taxon pt ON pt.taxon_id = m.taxon_id AND pt.project_id = ? "
        "LEFT JOIN taxon p ON p.inat_id = t.parent_inat_id AND p.rank = 'species' "
        "WHERE m.status IN ('auto', 'confirmed') AND LENGTH(c.captured_on) >= 7 "
        "  AND t.rank IN ('species', 'subspecies', 'variety', 'form', 'hybrid')"));
    q.addBindValue(projectId);
    if (!q.exec())
        return months;
    while (q.next()) {
        const int m = q.value(1).toString().toInt();
        if (m >= 1 && m <= 12)
            ++months[m - 1];
    }
    return months;
}

} // namespace pl::coverage
