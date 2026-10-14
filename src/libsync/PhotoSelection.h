#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <optional>

namespace pl::libsync {

// Which photos a computer keeps when only some reference trees are ticked,
// read from the catalogue. A photo belongs to a tree through its capture's
// auto or confirmed match to a taxon in it (project_taxon); one capture can be
// in several trees, or in none.
//
// A capture's photos are left online only when it's matched into at least
// one tree but none of the ticked ones. So unmatched photos (still to be
// reviewed) and photos matched only to taxa in no tree are always kept, and a
// RAW and its JPEG always go together.
class PhotoSelection
{
public:
    // The rendition paths (absolute, as the catalogue holds them) to leave
    // online only, given the ticked trees. `connection` is an open catalogue
    // connection on the calling thread. nullopt on a database error.
    static std::optional<QSet<QString>> notWanted(const QString &connection,
                                                  const QList<int> &tickedTrees,
                                                  QString *error = nullptr);

    // Every rendition's content hash (its file's SHA-256), by path.
    static QHash<QString, QString> contentHashes(const QString &connection);

    struct TreeStats
    {
        int id = 0;
        QString name;
        int photos = 0;      // captures matched into the tree
        qint64 bytes = 0;    // their files' total size
    };
    // Each reference tree's photos, by name.
    static QList<TreeStats> treeStats(const QString &connection);
};

} // namespace pl::libsync
