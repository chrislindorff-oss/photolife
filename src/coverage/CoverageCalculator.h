#pragma once

#include <QHash>
#include <QString>

namespace pl::coverage {

// Coverage rolled up over a taxon's subtree within one project.
struct TaxonCoverage
{
    qint64 inatId = 0;
    qint64 parentInatId = 0;      // 0 if root (no parent in the project's tree)
    QString rank;
    QString name;
    QString commonName;
    QString status;               // conservation status of this taxon, if any

    int speciesTotal = 0;         // rank=="species" nodes in the subtree
    int speciesWithPhotos = 0;    // ...that have a photo somewhere in their own subtree
    int threatenedTotal = 0;      // species with a non-empty status
    int threatenedWithPhotos = 0;
    int captureCount = 0;         // matched captures on this taxon or a descendant
    QString newestCapture;        // ISO date, max over the subtree

    bool hasOwnPhotos = false;    // this exact taxon has >= 1 matched capture
    bool subtreeHasPhotos = false;

    double photographedFraction() const
    {
        return speciesTotal > 0 ? double(speciesWithPhotos) / speciesTotal : 0.0;
    }
};

struct ProjectCoverage
{
    int speciesTotal = 0;
    int speciesWithPhotos = 0;
    int threatenedTotal = 0;
    int threatenedWithPhotos = 0;
    int captureCount = 0;
    QString newestCapture;

    // Per conservation-status tier: {"Endangered": {total, withPhotos}, ...}
    struct Tier { int total = 0; int withPhotos = 0; };
    QHash<QString, Tier> byStatus;

    // Per-taxon rollup, keyed by iNaturalist id, for the tree view.
    QHash<qint64, TaxonCoverage> byTaxon;

    double photographedFraction() const
    {
        return speciesTotal > 0 ? double(speciesWithPhotos) / speciesTotal : 0.0;
    }
};

// Joins the project's cached tree, its conservation statuses, and the
// auto/confirmed capture_match rows, and rolls counts up every rank.
// Read-only; give it an open connection name.
ProjectCoverage computeCoverage(const QString &connectionName, int projectId);

// Chooses a cover capture for every project taxon that has photos anywhere in
// its subtree (best match confidence, then newest capture) and rewrites the
// project's `representative` rows. Returns how many were written.
int pickRepresentatives(const QString &connectionName, int projectId);

struct RepresentativeImage
{
    qint64 captureId = 0;
    QString previewPath;
    QString previewHash;

    bool isValid() const { return captureId > 0; }
};

// The cover capture chosen for a taxon in a project (empty if none).
RepresentativeImage representativeFor(const QString &connectionName, int projectId,
                                     qint64 taxonInatId);

} // namespace pl::coverage
