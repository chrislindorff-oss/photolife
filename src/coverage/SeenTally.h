#pragma once

#include "coverage/CoverageCalculator.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QHash>
#include <QList>
#include <QSet>

namespace pl::coverage {

// One taxon in the Reference Tree's Seen column.
struct SeenState
{
    bool photographed = false;   // it, or something below it, has a photo
    bool marked = false;         // ticked as seen by hand
    bool seen = false;           // a species or lower: photographed, marked, or a lower taxon of it seen
    qint64 seenVia = 0;          // seen only through this lower taxon (0 otherwise)
    int speciesSeen = 0;         // species in the subtree (itself included) that are seen
};

// What the Seen column adds up to over some of the tree's taxa.
struct SeenTotals
{
    int speciesShown = 0;
    int speciesSeen = 0;
    int speciesPhotographed = 0;   // of speciesSeen
    int speciesSeenOnly = 0;       // of speciesSeen: seen, not photographed
    int belowSeen = 0;             // subspecies, varieties, hybrids ... seen
};

// Rolls the seen ticks up a project's tree. `flat` lists its taxa parents
// first (TaxonomyStore::projectTree); `coverage` says which are
// photographed; `marked` holds the hand ticks (iNat ids). Only species and
// lower ranks are ever seen themselves; a seen subspecies makes its species
// seen too. Higher taxa get speciesSeen only.
QHash<qint64, SeenState> rollUpSeen(const QList<taxonomy::TreeNode> &flat, const ProjectCoverage &coverage,
                                    const QSet<qint64> &marked);

// Adds up the taxa `shown` (from rollUpSeen's `states`).
SeenTotals tallySeen(const QList<taxonomy::TreeNode> &shown, const QHash<qint64, SeenState> &states);

} // namespace pl::coverage
