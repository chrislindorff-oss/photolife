#pragma once

#include <QMetaType>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

namespace pl::collection {

// What to include when exporting a reference tree's photos.
struct ExportOptions
{
    bool includeAutoApplied = false;   // besides always-included confirmed matches
    bool scaffoldEmptyFolders = true;  // create every taxon's folder, even ones with no photos

    // Empty = every taxon in the tree gets its own folder (today's behavior).
    // Non-empty = only taxa whose inat_id is in this set get their own folder;
    // anything that would otherwise attach to an excluded taxon (a descendant,
    // or a photo confirmed directly at it) resolves to its nearest *included*
    // ancestor's folder instead, mirroring the reference-tree dock's own
    // rank-hiding reparenting. Meant to be filled from
    // TaxonomyTreeModel::visibleTaxonIds() so the export matches whatever the
    // dock's current filters (photographed-only / hidden ranks / threatened)
    // are showing on screen.
    QSet<qint64> restrictToTaxonIds;

    // Append each taxon's common name to its folder name, e.g.
    // "Anura  ·  Frogs and Toads", when one is known and differs from the
    // scientific name (empty/identical common names are omitted).
    bool includeCommonName = false;

    // "Update an existing collection": destRoot *is* the collection's root
    // folder, rather than the parent a new "<tree name>" folder is created in.
    bool updateExisting = false;

    // After copying, list every file under the collection root that this run
    // didn't produce or find already in place (ExportSummary::unexpectedFiles)
    // -- photos reassigned or excluded since an earlier export, or files added
    // by hand. Reported only; nothing is ever deleted.
    bool reportUnexpectedFiles = false;
};

struct ExportSummary
{
    int foldersCreated = 0;
    int photosCopied = 0;
    int photosSkippedExisting = 0;      // already present from a prior export run
    int photosRenamedForCollision = 0;  // same name, different file -- disambiguated
    bool cancelled = false;
    QString error;
    QString collectionRoot;          // the folder the collection was written to
    QStringList unexpectedFiles;     // relative to collectionRoot; see reportUnexpectedFiles

    bool ok() const { return error.isEmpty(); }
};

// Builds a folder tree mirroring a reference tree's taxonomy under
// <destRoot>/<project's own name>/... (or directly under destRoot when
// updating an existing collection), one folder per taxon in the tree (any
// rank), and copies each in-scope capture's rendition files (raw/jpeg) into
// its matched taxon's own folder, keeping the original filename. Files
// already present from an earlier run are left alone. Source files are only
// ever read, never modified or moved. A successful run leaves a
// CollectionManifest in the collection root.
//
// The project's name is read straight from the `project` table rather than
// taken from a caller-supplied string, since the reference-tree combo box
// decorates a pruned tree's display text with " (customized)" -- that suffix
// has no business ending up in a folder name.
class ExportEngine
{
public:
    explicit ExportEngine(QString connectionName);

    using CancelFn = std::function<bool()>;
    using ProgressFn = std::function<void(int done, int total)>;

    ExportSummary run(int projectId, const QString &destRoot, const ExportOptions &options,
                       const CancelFn &cancel = {}, const ProgressFn &progress = {});

private:
    QString m_connectionName;
};

} // namespace pl::collection

Q_DECLARE_METATYPE(pl::collection::ExportSummary)
