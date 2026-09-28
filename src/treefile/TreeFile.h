#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace pl::coverage {
struct ProjectCoverage;
}
namespace pl::taxonomy {
class TaxonomyStore;
}

namespace pl::treefile {

// A reference tree as a spreadsheet: one row per taxon (every rank), readable
// columns first (rank, names, the Kingdom..Genus a row sits in, status,
// origin) and the columns PhotoLife needs for an exact round trip last (iNat
// ids, ancestry, and every recorded status/origin as JSON). Written as .xlsx
// (a "Taxa" sheet plus a "Tree" sheet of details) or .csv (details as
// "# Key: value" lines above the header).
//
// Reading also accepts hand-made files: only a scientific-name column is
// required (the checklist header aliases -- "Name", "Species", "Taxon" --
// work too). A row without an iNat id is resolved by name on import.
//
// The export also says how much of each taxon the user has photographed
// (Photographed, Photos, Latest photo, Species photographed). Those columns
// describe one library, not the tree, so reading ignores them: whoever
// imports the tree gets coverage from their own photos.

// One recorded conservation status or origin ("introduced" / "native").
struct TreeFileFact
{
    QString value;
    QString source;
    std::optional<qint64> placeInatId;

    bool operator==(const TreeFileFact &) const = default;
};

struct TreeFileRow
{
    QString rank;
    QString name;
    QString commonName;
    std::optional<bool> inRegion;   // not given: decided on import

    std::optional<qint64> inatId;
    std::optional<qint64> parentInatId;
    QString ancestry;               // "48460/47126/..." (ancestors only)
    std::optional<int> rankLevel;
    bool isActive = true;
    bool fromChecklist = false;

    // Every recorded status / origin. A hand-made file's single Status or
    // Origin column is read into one fact here with no source or place.
    QList<TreeFileFact> statuses;
    QList<TreeFileFact> origins;

    // The user's photos of this taxon and everything below it -- written
    // only, never read back (see above).
    struct Coverage
    {
        int photoCount = 0;
        QString latestPhoto;       // ISO date
        int speciesTotal = 0;      // species below a higher-rank taxon
        int speciesWithPhotos = 0;

        bool operator==(const Coverage &) const = default;
    };
    std::optional<Coverage> coverage;

    bool operator==(const TreeFileRow &) const = default;
};

struct TreeFile
{
    static constexpr int kFormatVersion = 1;

    // kFormatVersion for a file PhotoLife exported (it says so in its
    // details); 0 for a hand-made one. An exported file is taken as the
    // exact tree; a hand-made one gets its missing ancestors filled in.
    int formatVersion = 0;
    QString name;                           // the tree's name when exported
    std::optional<qint64> rootInatId;
    QString rootName;
    std::optional<qint64> placeInatId;
    QString placeName;
    QList<qint64> excludedInatIds;          // pruned taxa, so a refresh won't re-add them
    QList<TreeFileRow> rows;                // parents before children
};

// Reads a .xlsx or .csv tree file (by extension). On failure returns
// nullopt and sets `error`.
std::optional<TreeFile> readTreeFile(const QString &path, QString *error);

// Writes `file` as .xlsx or .csv (by extension). On failure returns false and
// sets `error`.
bool writeTreeFile(const QString &path, const TreeFile &file, QString *error);

// The project's tree as a TreeFile, rows in depth-first order (children by
// name) so the file reads like the tree. With `coverage` (computeCoverage()
// for the same project), each row carries the user's photo coverage.
TreeFile exportProject(const pl::taxonomy::TaxonomyStore &store, int projectId,
                       const QString &treeName,
                       const pl::coverage::ProjectCoverage *coverage = nullptr);

} // namespace pl::treefile
