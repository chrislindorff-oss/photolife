#pragma once

#include "catalogue/DuplicateFinder.h"

#include <QDialog>

class QLabel;
class QTreeWidget;

namespace pl {

// Find Duplicate Photos: the files in the library that have an identical
// copy elsewhere in it, grouped, the biggest waste first. Each copy can be
// shown in the file manager; nothing is deleted here -- removing a copy is
// done in the file manager, and a rescan then catches up.
class DuplicatesDialog : public QDialog
{
    Q_OBJECT

public:
    // `stale`: copies the catalogue lists that aren't on disk any more.
    DuplicatesDialog(const QList<catalogue::DuplicateGroup> &groups, int stale, QWidget *parent = nullptr);

private:
    void showSelected();
    QTreeWidget *m_tree = nullptr;
};

} // namespace pl
