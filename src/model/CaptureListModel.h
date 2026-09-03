#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QString>

namespace pl {
class Database;
}
namespace pl::thumb {
class ThumbnailCache;
}

namespace pl::model {

// Flat list of every capture in the catalogue, newest first, for the grid view.
// Rows are loaded up front (the library is ~16k captures); thumbnails are pulled
// lazily from the ThumbnailCache and filled in as they arrive.
class CaptureListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        FolderPathRole,
        CapturedOnRole,
        DateSourceRole,
        PreviewPathRole,
        MatchStatusRole,   // "auto" | "pending" | "unmatched" | "confirmed"
        MatchedNameRole,
    };

    CaptureListModel(pl::Database &db, pl::thumb::ThumbnailCache &thumbs,
                     QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Re-reads every row from the catalogue.
    void reload();

    // "" = all; "auto" | "pending" | "unmatched" | "confirmed" restrict the grid.
    void setStatusFilter(const QString &status);
    QString statusFilter() const { return m_statusFilter; }

    // 0 = every capture; otherwise only captures matched to this taxon (by iNat
    // id) or any of its descendants.
    void setTaxonScope(qint64 taxonInatId);
    qint64 taxonScope() const { return m_taxonScope; }

    int captureCount() const { return int(m_rows.size()); }

private:
    struct Row
    {
        int id = 0;
        QString baseName;
        QString name;
        QString folderPath;
        QString capturedOn;
        QString dateSource;
        QString previewPath;
        QString previewHash;
        QString matchStatus;
        QString matchedName;
    };

    void onThumbnailReady(const QString &contentHash, int longestEdge);

    pl::Database &m_db;
    pl::thumb::ThumbnailCache &m_thumbs;
    QList<Row> m_rows;
    QHash<QString, QList<int>> m_rowsByHash;   // preview hash -> row indices
    QIcon m_placeholder;
    QString m_statusFilter;
    qint64 m_taxonScope = 0;
};

} // namespace pl::model
