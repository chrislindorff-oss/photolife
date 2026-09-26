#include "inat/SpeciesComparison.h"

#include "net/INatParse.h"
#include "taxonomy/TaxonomyStore.h"

#include <algorithm>

namespace pl::inat {

std::optional<qint64> rollUp(const taxonomy::Taxon &leaf,
                             const QHash<qint64, taxonomy::Taxon> &known, const QString &rank)
{
    if (leaf.rank == rank)
        return leaf.inatId;
    const QList<qint64> ancestors = net::inat::ancestryIds(leaf.ancestry);
    for (auto it = ancestors.crbegin(); it != ancestors.crend(); ++it) {
        const auto found = known.constFind(*it);
        if (found != known.constEnd() && found->rank == rank)
            return *it;
    }
    // A hybrid between species has no species above it, but the trees list
    // it alongside the species -- so at species rank it stands for itself.
    // (Subspecies and the like have already found their species above.)
    if (rank == QLatin1String("species") && taxonomy::TaxonomyStore::isLeafRank(leaf.rank))
        return leaf.inatId;
    return std::nullopt;
}

ComparisonResult compare(const QList<net::SpeciesCount> &inatLeaves,
                         const QList<qint64> &treeLeafIds,
                         const QHash<qint64, taxonomy::Taxon> &known, const QString &rank,
                         const LibraryPresenceFn &inLibrary)
{
    ComparisonResult result;
    QHash<qint64, taxonomy::Taxon> taxa = known;
    QHash<qint64, ComparisonRow> rows;

    auto rowFor = [&](qint64 id) -> ComparisonRow & {
        auto it = rows.find(id);
        if (it == rows.end()) {
            ComparisonRow row;
            row.inatId = id;
            const taxonomy::Taxon t = taxa.value(id);
            row.name = t.name;
            row.commonName = t.commonName;
            it = rows.insert(id, row);
        }
        return *it;
    };

    for (const net::SpeciesCount &leaf : inatLeaves)
        taxa.insert(leaf.taxon.inatId, leaf.taxon);
    for (const net::SpeciesCount &leaf : inatLeaves) {
        const std::optional<qint64> id = rollUp(leaf.taxon, taxa, rank);
        if (!id) {
            ++result.unplacedTaxa;
            result.unplacedObservations += leaf.count;
            continue;
        }
        ComparisonRow &row = rowFor(*id);
        row.onInat = true;
        row.inatObservations += leaf.count;
    }

    for (qint64 leafId : treeLeafIds) {
        const auto leaf = taxa.constFind(leafId);
        if (leaf == taxa.constEnd())
            continue;
        if (const std::optional<qint64> id = rollUp(*leaf, taxa, rank))
            rowFor(*id).inTree = true;
    }

    result.rows.reserve(rows.size());
    for (ComparisonRow &row : rows) {
        if (inLibrary)
            row.inLibrary = inLibrary(row.inatId, row.name, row.commonName);
        result.rows.append(row);
    }
    std::sort(result.rows.begin(), result.rows.end(),
              [](const ComparisonRow &a, const ComparisonRow &b) {
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });
    return result;
}

} // namespace pl::inat
