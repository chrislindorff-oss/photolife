#pragma once

#include "inat/InatObservationFetcher.h"

#include <QAbstractListModel>
#include <QHash>
#include <QIcon>
#include <QList>

#include <optional>

namespace pl::net {
class PhotoCache;
}
namespace pl::taxonomy {
class TaxonomyStore;
}

namespace pl::inat {

// Flat, one-row-per-photo list of the candidates an InatObservationFetcher
// run found (an observation may carry several photos, and the user should be
// able to pick specific ones, not just whole observations). Thumbnails are
// pulled lazily from PhotoCache using each photo's small previewUrl -- never
// its full-resolution downloadUrl, which is only fetched later for photos
// actually selected to import. Each row's taxon name is resolved from the
// already-cached local taxonomy (TaxonomyStore::taxonPhoto) at
// setCandidates() time, not fetched from iNat again.
class InatDownloadModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        ObservationIdRole = Qt::UserRole + 1,
        PhotoIdRole,
        TaxonInatIdRole,
        ObservedOnRole,
        DownloadUrlRole,
        LikelyDuplicateRole,
        HasGpsRole,
        LatitudeRole,    // valid only when HasGpsRole is true
        LongitudeRole,   // valid only when HasGpsRole is true
        NameRole,        // resolved taxon scientific name, may be empty if not cached locally
        PlaceGuessRole,  // iNat's own free-text place description, may be empty
        PresenceCheckedRole,   // true once LibraryPresence has annotated the candidates
        InLibraryRole,         // taxon already has photos in the library (see LibraryPresence)
        InAnyTreeRole,         // taxon is a member of at least one reference tree
    };

    InatDownloadModel(pl::net::PhotoCache &photos, pl::taxonomy::TaxonomyStore &store,
                      QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replaces the whole list, flattened to one row per photo.
    void setCandidates(const QList<Candidate> &candidates);
    void clear();

    int rowCountTotal() const { return int(m_rows.size()); }

private:
    struct Row
    {
        qint64 observationId = 0;
        qint64 photoId = 0;
        qint64 taxonInatId = 0;
        QString observedOn;
        QString previewUrl;
        QString downloadUrl;
        bool likelyDuplicate = false;
        bool alreadyDownloaded = false;   // exact inat_photo_id match, not just a heuristic guess
        std::optional<double> latitude;
        std::optional<double> longitude;
        QString taxonName;
        QString taxonCommonName;
        QString placeGuess;
        bool presenceChecked = false;
        bool inLibrary = false;
        bool inAnyTree = false;
    };

    void onPhotoReady(const QString &url);

    pl::net::PhotoCache &m_photos;
    pl::taxonomy::TaxonomyStore &m_store;
    QList<Row> m_rows;
    QHash<QString, QList<int>> m_rowsByUrl;   // preview url -> row indices
    QIcon m_placeholder;
};

} // namespace pl::inat
