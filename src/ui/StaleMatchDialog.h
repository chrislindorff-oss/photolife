#pragma once

#include "match/MatchEngine.h"

#include <QDialog>
#include <QList>

class QTreeWidget;

namespace pl {

// Shown after Match Library when photos you identified yourself have since
// been renamed to a different taxon (MatchEngine's staleDecisions). The
// engine never overrides your decisions, so this asks: re-associate each
// checked photo with the taxon its new name points to, keep the unchecked
// ones as identified (and stop asking about them), or decide later.
class StaleMatchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit StaleMatchDialog(const QList<match::StaleDecision> &stale,
                              QWidget *parent = nullptr);

    // Valid once exec() returns Accepted.
    QList<match::StaleDecision> toReassociate() const;
    QList<match::StaleDecision> toKeep() const;

private:
    void setAllChecked(bool checked);

    QList<match::StaleDecision> m_stale;   // index = row
    QTreeWidget *m_list = nullptr;
};

} // namespace pl
