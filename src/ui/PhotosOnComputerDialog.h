#pragma once

#include "libsync/PhotoSelection.h"

#include <QDialog>

class QListWidget;
class QRadioButton;

namespace pl {

// "Photos on This Computer": keep every photo of a server catalogue here, or
// only those of some reference trees (the rest stay online, browsable by
// thumbnail, and download when opened). Each tree shows how many photos it
// has and their size.
class PhotosOnComputerDialog : public QDialog
{
    Q_OBJECT

public:
    PhotosOnComputerDialog(const QList<libsync::PhotoSelection::TreeStats> &trees, bool allTrees,
                           const QList<int> &ticked, QWidget *parent = nullptr);

    bool allTrees() const;
    QList<int> tickedTrees() const;

private:
    void updateEnabled();

    QRadioButton *m_all;
    QRadioButton *m_some;
    QListWidget *m_list;
};

} // namespace pl
