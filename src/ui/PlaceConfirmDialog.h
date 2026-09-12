#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QDialog>
#include <QList>
#include <QString>

#include <optional>

class QLineEdit;
class QListWidget;
class QLabel;
class QDialogButtonBox;

namespace pl::net {
class INatClient;
}

namespace pl {

// Shows the iNaturalist places that match a free-text region query, so the
// user can confirm the right one (or explicitly build without a place
// filter) before any download starts, rather than having ProjectBuilder
// silently guess. Exec it, then read chosenPlace() if it returns Accepted.
class PlaceConfirmDialog : public QDialog
{
    Q_OBJECT

public:
    PlaceConfirmDialog(pl::net::INatClient &inat, const QString &initialQuery,
                        QWidget *parent = nullptr);

    // std::nullopt means "no place filter (worldwide)" was chosen.
    std::optional<pl::taxonomy::Place> chosenPlace() const;

private:
    void runSearch();
    void populateCandidates(const QList<pl::taxonomy::Place> &candidates);
    void setBusy(bool busy);

    pl::net::INatClient &m_inat;
    QLineEdit *m_query = nullptr;
    QListWidget *m_candidates = nullptr;
    QLabel *m_status = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QList<pl::taxonomy::Place> m_places;   // indices line up with m_candidates rows after the worldwide row
};

} // namespace pl
