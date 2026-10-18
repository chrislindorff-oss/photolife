#include "match/NameIndex.h"

#include "taxonomy/TaxonomyStore.h"

#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>

namespace pl::match {

namespace {

void addUnique(QList<NameIndex::Entry> &list, const NameIndex::Entry &e)
{
    for (const NameIndex::Entry &x : std::as_const(list)) {
        if (x.taxonId == e.taxonId)
            return;
    }
    list.append(e);
}

} // namespace

std::shared_ptr<const NameIndex> NameIndex::build(const QString &connectionName)
{
    auto index = std::make_shared<NameIndex>();
    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT tn.name, tn.name_folded, t.id, t.inat_id, t.parent_inat_id, t.name, t.rank, "
            "t.common_name FROM taxon_name tn JOIN taxon t ON t.id = tn.taxon_id "
            "WHERE tn.kind IN ('vernacular', 'user-vernacular')")))
        return index;
    while (q.next()) {
        Entry e;
        e.vernacular = q.value(0).toString();
        const QString folded = q.value(1).toString();
        e.taxonId = q.value(2).toLongLong();
        e.inatId = q.value(3).toLongLong();
        e.parentInatId = q.value(4).toLongLong();
        e.name = q.value(5).toString();
        e.rank = q.value(6).toString();
        e.commonName = q.value(7).toString();
        addUnique(index->m_byFolded[folded], e);
        const QString loose = looseKey(folded);
        if (!loose.isEmpty())
            addUnique(index->m_byLoose[loose], e);
    }
    return index;
}

QString NameIndex::looseKey(const QString &text)
{
    const QString folded = taxonomy::TaxonomyStore::foldName(text);
    QString out;
    out.reserve(folded.size());
    for (const QChar c : folded) {
        if (c.isLetterOrNumber())
            out.append(c);
    }
    return out;
}

QList<NameIndex::Entry> NameIndex::lookup(const QString &text, bool *loose) const
{
    if (loose)
        *loose = false;
    const QString folded = taxonomy::TaxonomyStore::foldName(text);
    if (const auto it = m_byFolded.constFind(folded); it != m_byFolded.constEnd())
        return *it;
    const QString key = looseKey(folded);
    if (key.size() < 4)
        return {};   // too little left to compare loosely
    if (const auto it = m_byLoose.constFind(key); it != m_byLoose.constEnd()) {
        if (loose)
            *loose = true;
        return *it;
    }
    return {};
}

std::optional<NameIndex::Entry> NameIndex::unique(const QList<Entry> &hits)
{
    if (hits.isEmpty())
        return std::nullopt;
    if (hits.size() == 1)
        return hits.first();
    // A species and its own infraspecific taxa: the species.
    for (const Entry &species : hits) {
        if (species.rank != QLatin1String("species"))
            continue;
        bool allItsOwn = true;
        for (const Entry &other : hits) {
            if (other.taxonId != species.taxonId && other.parentInatId != species.inatId) {
                allItsOwn = false;
                break;
            }
        }
        if (allItsOwn)
            return species;
    }
    return std::nullopt;
}

bool NameIndex::isSpeciesOrBelow(const QString &rank)
{
    static const QSet<QString> ranks = {
        QStringLiteral("species"), QStringLiteral("subspecies"), QStringLiteral("variety"),
        QStringLiteral("form"), QStringLiteral("hybrid"), QStringLiteral("infrahybrid")};
    return ranks.contains(rank);
}

} // namespace pl::match
