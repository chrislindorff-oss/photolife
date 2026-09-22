#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace pl::lightroom {

// One Lightroom-catalogued photo: its reconstructed absolute path and the
// deduped keyword strings tagged on it.
struct LightroomPhotoKeywords
{
    QString absolutePath;
    QStringList keywords;
};

// Reads keyword tags out of a Lightroom Classic catalog (.lrcat, itself a
// SQLite database) on a throwaway, read-only connection of its own -- never
// touches the app's own catalogue connection.
//
// Known risk: the table/column names below (Adobe_images, AgLibraryFile,
// AgLibraryFolder, AgLibraryRootFolder, AgLibraryKeyword,
// AgLibraryKeywordImage) come from general knowledge of the Lightroom
// catalog schema, not a verified real .lrcat file. read() defensively probes
// for them first (PRAGMA table_info) so a schema mismatch surfaces as a
// clear error() instead of a crash or a silently wrong result.
class LightroomCatalogReader
{
public:
    // Opens `lrcatPath` read-only, probes it for the tables/columns this
    // reader needs, and runs the join query. On failure, error() explains
    // why and the returned list is empty.
    QList<LightroomPhotoKeywords> read(const QString &lrcatPath);

    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }

private:
    QString m_error;
};

} // namespace pl::lightroom
