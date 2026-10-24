#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QDate>
#include <QDialog>
#include <QList>
#include <QString>

#include <optional>

class QCheckBox;
class QRadioButton;
class QLabel;
class QListWidget;
class QDialogButtonBox;

class QToolButton;

namespace pl::net {
class INatClient;
class PhotoCache;
}
namespace pl::thumb {
class ThumbnailCache;
}

namespace pl {

// For "Attempt to Match with iNat Observation to Fetch Data": searches the
// configured account's own observations around the selected photos' dates
// (every species -- the photos may not have one yet) and lets the user pick
// the one that matches, by its photo. The chosen observation's species and
// coordinates are shown, and the user ticks which to apply to the selected
// photos. Nothing is applied here: exec it, then read chosenObservation() and
// the apply*() choices if it returns Accepted.
class InatObservationMatchDialog : public QDialog
{
    Q_OBJECT

public:
    struct Selection
    {
        int photos = 0;                   // photos the data would go to
        int withGps = 0;                  // ...of which already have coordinates
        bool anyNeedsSpecies = true;      // some are unmatched or matched to something else
        qint64 currentTaxonInatId = 0;    // their species, when they share one (listed first)
        QDate from;                       // the photos' dates (inclusive)
        QDate to;
        // When they were taken, to the minute where known (clock time, held
        // as UTC so no zone rules apply) -- and the dates of those whose time
        // isn't known.
        QList<QDateTime> times;
        QList<QDate> untimedDates;
        int window = 0;                   // the Window to start with
        QString photoName;                // the name the photo was filed under, to compare
        // One of the photos, to show what renaming would make of it.
        QString sampleBaseName;           // its file name without extension
        QString sampleNameText;           // ...and the name part of it, as scanned
        QString sampleExtension;          // "JPG"
        bool preferCommonName = true;     // which name renaming offers first
        // The photos themselves, shown beside the observations to compare.
        struct Photo
        {
            QString path;          // its preview file
            QString contentHash;   // its thumbnail's key, when catalogued
            QString label;         // its file name
            QDateTime taken;       // when, if known
        };
        QList<Photo> shown;
    };

    InatObservationMatchDialog(net::INatClient &inat, net::PhotoCache &photos,
                               const QString &userLogin, const Selection &selection,
                               QWidget *parent = nullptr, thumb::ThumbnailCache *thumbs = nullptr);

    // How close in time an observation must be to one of the photos.
    enum Window { Minutes, Hours, Days };
    static constexpr int kMinutes = 10;
    static constexpr int kHours = 2;

    std::optional<taxonomy::Observation> chosenObservation() const;
    Window window() const { return m_window; }
    bool applyCoordinates() const;
    bool applySpecies() const;
    bool replaceExistingCoordinates() const;
    // Renaming the photos' files with the species' name, and which name.
    bool renameFiles() const;
    bool renameWithCommonName() const;
    QString renameName() const;

    // The order candidates are offered in: observations of `preferredTaxon`
    // first, then the nearest in time to one of `times` (else in date to
    // `centre`), newest id breaking ties.
    static QList<taxonomy::Observation> rankCandidates(QList<taxonomy::Observation> candidates,
                                                       qint64 preferredTaxon, const QDate &centre,
                                                       const QList<QDateTime> &times = {});
    // The observations within `window` of the photos: of a photo's time (to
    // the minute or hour), or of its date when its time isn't known; a day
    // either side for Days. `noTime` counts those left out of a narrow window
    // only because iNaturalist has just their date.
    static QList<taxonomy::Observation> withinWindow(const QList<taxonomy::Observation> &all,
                                                     const QList<QDateTime> &times,
                                                     const QList<QDate> &untimedDates, Window window,
                                                     int *noTime = nullptr);
    // "Common name (Scientific) · rank", or what there is of it.
    static QString speciesText(const taxonomy::Observation &obs);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    static constexpr int kMaxPages = 3;   // of 200: more than a day's observations

    void runSearch(int page);
    void populateCandidates();
    void showDetails();
    void showPhoto();
    void updateApplyButton();
    void updateRename();
    void onPhotoReady(const QString &url);
    void showOwnPhoto();
    QString largeUrl(const taxonomy::Observation &obs) const;

    net::INatClient &m_inat;
    net::PhotoCache &m_photos;
    QString m_userLogin;
    Selection m_selection;

    thumb::ThumbnailCache *m_thumbs = nullptr;
    int m_ownIndex = 0;                // which of m_selection.shown is on view
    QLabel *m_ownPhoto = nullptr;
    QLabel *m_ownCaption = nullptr;
    QToolButton *m_ownPrevious = nullptr;
    QToolButton *m_ownNext = nullptr;
    QLabel *m_status = nullptr;
    QListWidget *m_candidates = nullptr;
    QLabel *m_photo = nullptr;
    QLabel *m_species = nullptr;
    QLabel *m_coordinates = nullptr;
    QLabel *m_link = nullptr;
    QLabel *m_applyHeading = nullptr;
    QCheckBox *m_applyCoordinates = nullptr;
    QCheckBox *m_applySpecies = nullptr;
    QCheckBox *m_replace = nullptr;
    Window m_window = Minutes;
    QRadioButton *m_windowButtons[3] = {};
    bool m_searched = false;
    QCheckBox *m_rename = nullptr;
    QRadioButton *m_renameCommon = nullptr;
    QRadioButton *m_renameScientific = nullptr;
    QLabel *m_renameExample = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QList<taxonomy::Observation> m_found;          // as the pages arrive
    QList<taxonomy::Observation> m_observations;   // indices line up with m_candidates rows
};

} // namespace pl
