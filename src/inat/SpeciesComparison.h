#pragma once

#include "net/INatClient.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QHash>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace pl::inat {

// Compares one iNaturalist observer's taxa with the reference trees' taxa at
// a single rank, for the iNaturalist view's Compare Species tab. Pure: all
// data comes in as arguments (see InatSpeciesListFetcher for gathering it).

// The iNat id of `leaf`'s taxon at `rank`: the leaf itself when it has that
// rank, otherwise the nearest ancestor in its ancestry with that rank. The
// ancestors' ranks are read from `known`. nullopt when there is no such
// taxon -- the leaf is identified only above `rank`, or its lineage skips
// that rank, or an ancestor's rank isn't known. At species rank a hybrid
// (which has no species above it) stands for itself.
std::optional<qint64> rollUp(const taxonomy::Taxon &leaf,
                             const QHash<qint64, taxonomy::Taxon> &known, const QString &rank);

struct ComparisonRow
{
    qint64 inatId = 0;
    QString name;
    QString commonName;
    int inatObservations = 0;   // summed over the observer's leaf taxa beneath it
    bool onInat = false;
    bool inTree = false;
    bool inLibrary = false;
};

struct ComparisonResult
{
    QList<ComparisonRow> rows;   // name-sorted
    // The observer's leaf taxa (and their observations) that couldn't be
    // placed at the rank -- see rollUp().
    int unplacedTaxa = 0;
    int unplacedObservations = 0;
};

// (inat id, scientific name, common name) -> "do I have photos of it?"
using LibraryPresenceFn = std::function<bool(qint64, const QString &, const QString &)>;

// Rolls both sides up to `rank` and merges them into one row per taxon.
// `known` must hold the tree leaves and every ancestor of both sides' leaves
// that the roll-up needs; the observer's own leaf taxa are taken from
// `inatLeaves`.
ComparisonResult compare(const QList<net::SpeciesCount> &inatLeaves,
                         const QList<qint64> &treeLeafIds,
                         const QHash<qint64, taxonomy::Taxon> &known, const QString &rank,
                         const LibraryPresenceFn &inLibrary);

} // namespace pl::inat
