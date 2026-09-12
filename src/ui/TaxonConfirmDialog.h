#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QDialog>
#include <QList>
#include <QString>

#include <optional>

class QLineEdit;
class QComboBox;
class QListWidget;
class QLabel;
class QDialogButtonBox;

namespace pl::net {
class INatClient;
}

namespace pl {

// Shows the iNaturalist taxa that match a free-text root-taxon query, so the
// user can confirm the right one before any download starts, rather than
// having ProjectBuilder silently guess. Exec it, then read chosenTaxon() if
// it returns Accepted.
class TaxonConfirmDialog : public QDialog
{
    Q_OBJECT

public:
    TaxonConfirmDialog(pl::net::INatClient &inat, const QString &initialQuery,
                       const QString &initialRank, QWidget *parent = nullptr);

    std::optional<pl::taxonomy::Taxon> chosenTaxon() const;

private:
    void runSearch();
    void populateCandidates(const QList<pl::taxonomy::Taxon> &candidates);
    void setBusy(bool busy);

    pl::net::INatClient &m_inat;
    QLineEdit *m_query = nullptr;
    QComboBox *m_rank = nullptr;
    QListWidget *m_candidates = nullptr;
    QLabel *m_status = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QList<pl::taxonomy::Taxon> m_taxa;   // indices line up with m_candidates rows
};

} // namespace pl
