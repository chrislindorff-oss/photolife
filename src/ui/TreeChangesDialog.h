#pragma once

#include "taxonomy/TreeChangeFinder.h"

#include <QDialog>
#include <QList>

class QComboBox;
class QTreeWidget;

namespace pl {

// What a refresh found gone from a reference tree on iNaturalist (renamed,
// split, retired, no longer recorded in the place, or a group left empty),
// one row each, with what to do about it: ask again next time, keep it,
// prune it, or -- for a new name -- move its photos there. Renames default
// to moving; everything else to asking again. choices() is what to do.
class TreeChangesDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Action { Later, Keep, Prune, Move };
    struct Choice
    {
        taxonomy::TreeChange change;
        Action action = Action::Later;
        taxonomy::Taxon moveTo;   // Move: the new name
    };

    TreeChangesDialog(const QList<taxonomy::TreeChange> &changes, const QString &treeName,
                      const QString &placeName, QWidget *parent = nullptr);

    QList<Choice> choices() const;

private:
    QTreeWidget *m_tree = nullptr;
    QList<taxonomy::TreeChange> m_changes;
    QList<QComboBox *> m_actions;   // one per change, in order
};

} // namespace pl
