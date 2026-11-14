#pragma once

#include <QWidget>

class QToolButton;

namespace pl {

// "To do:" chips above a photo grid: Unnamed, To upload, No location, Not in
// a tree. Each is a CaptureListModel::TodoFlag; several show the photos
// needing any of them, and they combine with the grid's search and scope.
class TodoFilterBar : public QWidget
{
    Q_OBJECT

public:
    // `withNotInTree`: offer "Not in a tree" (pointless where the grid is
    // already a tree's).
    explicit TodoFilterBar(bool withNotInTree, QWidget *parent = nullptr);

    int flags() const;
    void clear();

signals:
    void flagsChanged(int flags);

private:
    QToolButton *addChip(const QString &text, const QString &tip, int flag);

    QList<QToolButton *> m_chips;
    QToolButton *m_clear = nullptr;
};

} // namespace pl
