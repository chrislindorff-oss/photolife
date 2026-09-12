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
        DisplayNameRole,   // name_text, falling back to the raw filename — independent
                          // of the configurable caption shown under the grid thumbnail
        ExtRole,           // the shown rendition's file extension, e.g. "jpg" | "cr2"
        HasGpsRole,        // true when the capture's EXIF carried GPS coordinates
        LatitudeRole,      // valid only when HasGpsRole is true
        LongitudeRole,     // valid only when HasGpsRole is true
        IsBestShotRole,    // true when the user has starred this capture as a
                          // best shot of its identified species
        ShowFileTypeBadgeRole,   // true when File Type is checked (drawn as an
                                 // on-image badge, not a Qt::DisplayRole line)
        FullCaptionRole,   // every checked field (file type included), one per
                          // line — for the full-size viewer, which has room
                          // for real text and no on-image badge of its own
        LocalityRole,      // reverse-geocoded "Town, State, Country", or empty
    };

    // Which fields the Qt::DisplayRole caption (shown under each grid thumbnail)
    // combines, one per line. Bits may be OR'd together; Name is the default.
    enum CaptionField {
        CaptionName     = 1 << 0,
        CaptionDate     = 1 << 1,
        CaptionFilename = 1 << 2,
        CaptionFileType = 1 << 3,
        CaptionTaxon    = 1 << 4,
        CaptionLocality = 1 << 5,
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

    // When > 0, results are confined to taxa that belong to this reference tree
    // (project_taxon) — on its own with no taxon scope, and intersected with the
    // subtree when a taxon scope is also set. Without it, selecting an ancestor
    // node shared by several trees (or the synthetic "Life" root a tree shows
    // above its real scope) would pull in every project's photos under that
    // ancestor, and clearing the selection would fall back to the whole library.
    void setProjectScope(int projectId);
    int projectScope() const { return m_projectScope; }

    // When true, the grid is restricted to captures the user has starred as a
    // best shot (the best_shot table). Combines with the status and taxon
    // filters.
    void setBestShotOnly(bool on);
    bool bestShotOnly() const { return m_bestShotOnly; }

    // Patches the starred state of the given captures in place (no model reset),
    // for a snappy repaint of the star overlay after the user stars/unstars
    // from the grid. Rows not currently loaded are ignored.
    void applyBestShot(const QList<int> &captureIds, bool on);

    // OR of CaptionField bits controlling the grid caption. Purely a display
    // setting — no re-query needed, so this just repaints.
    void setCaptionFields(int fields);
    int captionFields() const { return m_captionFields; }

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
        QString ext;
        QString matchStatus;
        QString matchedName;
        bool hasGps = false;
        double latitude = 0.0;
        double longitude = 0.0;
        bool isBestShot = false;
        QString locality;
    };

    void onThumbnailReady(const QString &contentHash, int longestEdge);

    pl::Database &m_db;
    pl::thumb::ThumbnailCache &m_thumbs;
    QList<Row> m_rows;
    QHash<QString, QList<int>> m_rowsByHash;   // preview hash -> row indices
    QIcon m_placeholder;
    QString m_statusFilter;
    qint64 m_taxonScope = 0;
    int m_projectScope = 0;
    bool m_bestShotOnly = false;
    int m_captionFields = CaptionName;
};

} // namespace pl::model
