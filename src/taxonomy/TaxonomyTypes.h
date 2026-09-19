#pragma once

#include <QList>
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

    QString photoUrl;          // iNaturalist default_photo, medium size (empty if none)
    QString photoAttribution;  // licence / credit line for photoUrl

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

// Resume point for an interrupted build/refresh's species-pagination phase
// (see ProjectBuilder, project_build_checkpoint). Tied to the exact query
// parameters a run resolved -- a later run only honours it if its own
// resolved root taxon, place and page size all match.
struct BuildCheckpoint
{
    qint64 rootTaxonInatId = 0;
    std::optional<qint64> placeInatId;
    int perPage = 0;
    int nextPage = 0;
    int speciesSeen = 0;
    int speciesTotal = 0;
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

// One photo attached to an observation. Two URLs, deliberately: `previewUrl`
// is a small size, cheap to fetch for a review-grid thumbnail; `downloadUrl`
// is the largest size the parser could resolve, for actually saving the
// photo -- never used just to build a thumbnail, since that would mean
// downloading a full-resolution image only to shrink it. See
// net::inat::parseObservation().
struct ObservationPhoto
{
    qint64 id = 0;
    QString previewUrl;
    QString downloadUrl;
};

// One of a user's iNaturalist observations, for the "Download from
// iNaturalist" feature. Coordinates are the ones the API actually returned:
// true coordinates when the request was authenticated as the observation's
// owner, geoprivacy-obscured (or absent) otherwise -- see HttpClient's
// bearer-token handling.
struct Observation
{
    qint64 id = 0;
    qint64 taxonInatId = 0;
    QString observedOn;               // "YYYY-MM-DD", may be empty
    std::optional<double> latitude;
    std::optional<double> longitude;
    QString placeGuess;               // iNat's own free-text place description, may be empty
    QList<ObservationPhoto> photos;
};

} // namespace pl::taxonomy
