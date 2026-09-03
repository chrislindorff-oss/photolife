#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace pl::taxonomy {

// A region a project is scoped to (an iNaturalist place).
struct Place
{
    qint64 inatId = 0;
    QString name;
    QString displayName;
    std::optional<int> adminLevel;
    std::optional<double> bboxSwLat, bboxSwLng, bboxNeLat, bboxNeLng;
};

// A taxonomy node plus the alternative names known for it. Decoupled from the
// iNaturalist wire format so the store and the API client can evolve apart.
struct Taxon
{
    qint64 inatId = 0;
    std::optional<qint64> parentInatId;
    QString rank;
    std::optional<int> rankLevel;
    QString name;              // accepted scientific name
    QString commonName;        // preferred vernacular, if any
    QString ancestry;          // "48460/47126/..."
    bool isActive = true;

    QStringList synonyms;      // other scientific names
    QStringList vernacular;    // common names (commonName included is fine)
};

// Conservation status for a taxon in a place.
struct StatusRecord
{
    std::optional<qint64> placeInatId;
    QString status;            // e.g. "Vulnerable", "endangered"
    QString source;            // "inat" | "VBA 2021" | ...
};

// One row of a project's cached tree, for display.
struct TreeNode
{
    qint64 inatId = 0;
    std::optional<qint64> parentInatId;
    QString rank;
    QString name;
    QString commonName;
    bool inRegion = false;
    bool isLeafRank = false;   // rank == "species" or below
};

} // namespace pl::taxonomy
