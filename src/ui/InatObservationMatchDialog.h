#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QDate>
#include <QDialog>
#include <QList>
#include <QString>

#include <optional>

class QCheckBox;
class QLabel;
class QListWidget;
class QDialogButtonBox;

namespace pl::net {
class INatClient;
class PhotoCache;
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
    };

    InatObservationMatchDialog(net::INatClient &inat, net::PhotoCache &photos,
                               const QString &userLogin, const Selection &selection,
                               QWidget *parent = nullptr);

    std::optional<taxonomy::Observation> chosenObservation() const;
    bool applyCoordinates() const;
    bool applySpecies() const;
    bool replaceExistingCoordinates() const;

    // The order candidates are offered in: observations of `preferredTaxon`
    // first, then the nearest to `centre` in date, newest id breaking ties.
    static QList<taxonomy::Observation> rankCandidates(QList<taxonomy::Observation> candidates,
                                                       qint64 preferredTaxon, const QDate &centre);
    // "Common name (Scientific) · rank", or what there is of it.
    static QString speciesText(const taxonomy::Observation &obs);

private:
    static constexpr int kMaxPages = 3;   // of 200: more than a day's observations

    void runSearch(int page);
    void populateCandidates();
    void showDetails();
    void showPhoto();
    void updateApplyButton();
    void onPhotoReady(const QString &url);
    QString largeUrl(const taxonomy::Observation &obs) const;

    net::INatClient &m_inat;
    net::PhotoCache &m_photos;
    QString m_userLogin;
    Selection m_selection;

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
    QDialogButtonBox *m_buttons = nullptr;
    QList<taxonomy::Observation> m_found;          // as the pages arrive
    QList<taxonomy::Observation> m_observations;   // indices line up with m_candidates rows
};

} // namespace pl
