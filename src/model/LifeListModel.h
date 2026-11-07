#pragma once

#include "coverage/LifeList.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QIcon>
#include <QList>

#include <optional>

namespace pl::thumb {
class ThumbnailCache;
}

namespace pl::model {

// The Photographic Life List as a table: number, first photo (when
// thumbnails are on), date, names, place and how many photos.
class LifeListModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { NumberColumn, PhotoColumn, DateColumn, CommonColumn, ScientificColumn, PlaceColumn, CameraColumn,
                  FocalColumn, PhotosColumn, ColumnCount };
    enum Roles {
        CaptureIdRole = Qt::UserRole + 1,
        InatIdRole,
        PreviewPathRole,
        SortRole,
    };

    explicit LifeListModel(thumb::ThumbnailCache &thumbs, QObject *parent = nullptr);

    void setEntries(const QList<coverage::LifeListEntry> &entries);
    const QList<coverage::LifeListEntry> &entries() const { return m_entries; }
    const coverage::LifeListEntry &entry(int row) const { return m_entries.at(row); }

    void setShowThumbnails(bool on);
    // What the photo's EXIF said, once read: fills its Camera and Focal length.
    void setCameraInfo(int captureId, const QString &camera, std::optional<double> focalLength);

    // "14 Mar 2011", or "No date".
    static QString dateText(const QString &capturedOn);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    void onThumbnailReady(const QString &key, int longestEdge);

    thumb::ThumbnailCache &m_thumbs;
    QList<coverage::LifeListEntry> m_entries;
    QHash<QString, QList<int>> m_rowsByKey;   // ThumbnailCache::keyFor -> rows
    QHash<int, QList<int>> m_rowsByCapture;   // first photo -> rows
    bool m_thumbnails = false;
    QIcon m_placeholder;
};

} // namespace pl::model
