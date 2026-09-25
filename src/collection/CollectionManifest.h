#pragma once

#include <QDateTime>
#include <QString>

#include <optional>

namespace pl::collection {

// The small marker file ExportEngine leaves in an exported collection's root
// folder. "Update an existing collection" reads it to confirm the folder
// belongs to the right reference tree (even after the user renames it) and
// to reuse the folder-naming choice the collection was built with --
// changing "include common name" between runs would otherwise rename every
// taxon folder and duplicate the whole tree.
inline constexpr char kManifestFileName[] = ".photolife-collection.json";

struct CollectionManifest
{
    int projectId = 0;
    QString projectName;
    bool includeCommonName = false;
    QDateTime createdAt;
    QDateTime updatedAt;

    // nullopt when the file is missing, unreadable, or not a manifest.
    static std::optional<CollectionManifest> read(const QString &collectionRoot);

    // Writes atomically (QSaveFile). Returns false on any I/O error.
    bool write(const QString &collectionRoot) const;
};

} // namespace pl::collection
