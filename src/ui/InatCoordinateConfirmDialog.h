#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QDate>
#include <QDialog>
#include <QList>
#include <QString>

#include <optional>

class QLabel;
class QListWidget;
class QDialogButtonBox;

namespace pl::net {
class INatClient;
class PhotoCache;
}

namespace pl {

// For the "Attempt to fetch coordinates from iNat" action: searches the
// configured account's own observations for a likely match to one local,
// GPS-less capture (same taxon, within a day of its captured-on date), and
// lets the user confirm which -- by its photo thumbnail -- before its
// coordinates are used, rather than guessing silently. Only observations
// with actual coordinates are offered. Exec it, then read chosenObservation()
// if it returns Accepted.
class InatCoordinateConfirmDialog : public QDialog
{
    Q_OBJECT

public:
    InatCoordinateConfirmDialog(net::INatClient &inat, net::PhotoCache &photos,
                                qint64 taxonInatId, const QString &userLogin,
                                const QDate &capturedOn, QWidget *parent = nullptr);

    std::optional<taxonomy::Observation> chosenObservation() const;

private:
    void runSearch();
    void populateCandidates(const QList<taxonomy::Observation> &candidates);
    void setBusy(bool busy);
    void onPhotoReady(const QString &url);

    net::INatClient &m_inat;
    net::PhotoCache &m_photos;
    qint64 m_taxonInatId = 0;
    QString m_userLogin;
    QDate m_capturedOn;

    QLabel *m_status = nullptr;
    QListWidget *m_candidates = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QList<taxonomy::Observation> m_observations;   // indices line up with m_candidates rows
};

} // namespace pl
