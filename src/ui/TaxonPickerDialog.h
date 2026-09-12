#pragma once

#include "match/CandidateFinder.h"

#include <QDialog>
#include <QList>
#include <QString>

class QLineEdit;
class QListWidget;

namespace pl {

// A small modal "search for a taxon" picker, backed by the same free-text
// taxon search the review queue uses (pl::match::CandidateFinder::search).
// Exec it, then read chosenTaxonInatId() if it returns Accepted.
class TaxonPickerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TaxonPickerDialog(QString connectionName, QWidget *parent = nullptr);

    // 0 if nothing was chosen.
    qint64 chosenTaxonInatId() const;

private:
    void runSearch(const QString &text);
    void populateCandidates(const QList<pl::match::TaxonCandidate> &candidates);

    pl::match::CandidateFinder m_finder;
    QLineEdit *m_search = nullptr;
    QListWidget *m_candidates = nullptr;
};

} // namespace pl
