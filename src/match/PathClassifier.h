#pragma once

#include <QString>

namespace pl::match {

// What a folder in the library is.
enum class FolderKind
{
    Unknown,
    Group,      // an informal grouping: "3. MONOCOTYLEDONS", "FERNS & FERN ALLIES"
    Taxon,      // a family / genus / species / infraspecies folder
    Locality,   // a place folder nested under a taxon
    Staging,    // "To Sort and Upload", "Duplicates", ...
};

struct FolderClass
{
    FolderKind kind = FolderKind::Unknown;
    QString rank;           // for Taxon: "family" | "genus" | "species" | "infraspecies"
    QString inferredName;   // for Taxon: the cleaned taxon name

    bool operator==(const FolderClass &) const = default;
};

// Classifies a single folder name by its shape alone (no tree context).
// Strips a leading "3. " / "3) " numbering. Family folders are recognised by the
// -aceae suffix; the matcher still trusts an inner genus/species over a possibly
// pre-APG family folder.
FolderClass classifyFolderName(const QString &folderName);

// Runs classifyFolderName over every row of the `folder` table on the given
// open connection, then uses parent/child context to resolve the leftovers
// (e.g. a bare capitalised word directly under a group is a genus; an
// unparseable folder under a species is a locality). Writes kind / inferred_rank
// / inferred_name back. Returns the number of rows updated.
int classifyFolders(const QString &connectionName);

} // namespace pl::match
