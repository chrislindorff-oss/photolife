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

// The review queue: every capture whose match is still pending or unresolved,
// with the engine's current guess, ordered by folder then file name.
class ReviewQueueModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        CaptureIdRole = Qt::UserRole + 1,
        FolderIdRole,
        FolderPathRole,
        BaseNameRole,
        NameTextRole,
        PreviewPathRole,
        GuessNameRole,
        GuessInatIdRole,
        ConfidenceRole,
        NoteRole,
        QualifierRole,
        SearchTextRole,   // name + folder + guess, joined, for the filter box
    };

    ReviewQueueModel(pl::Database &db, pl::thumb::ThumbnailCache &thumbs,
                     QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    void reload();
    void dropCapture(qint64 captureId);

    int queueCount() const { return int(m_rows.size()); }
    int pendingInFolder(int folderId) const;

private:
    struct Row
    {
        qint64 id = 0;
        int folderId = 0;
        QString folderPath;
        QString baseName;
        QString nameText;
        QString previewPath;
        QString previewHash;
        QString guessName;
        qint64 guessInatId = 0;
        double confidence = 0.0;
        QString note;
        QString qualifier;
    };

    void onThumbnailReady(const QString &contentHash, int longestEdge);

    pl::Database &m_db;
    pl::thumb::ThumbnailCache &m_thumbs;
    QList<Row> m_rows;
    QHash<QString, QList<int>> m_rowsByHash;
    QIcon m_placeholder;
};

} // namespace pl::model
