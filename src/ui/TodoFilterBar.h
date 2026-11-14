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

    // A chip for a map area narrowing the photos too ("Map area: Anakie
    // Gorge ✕"); clicking it clears the area. Empty: hidden.
    void setAreaChip(const QString &text);

signals:
    void flagsChanged(int flags);
    void areaChipCleared();

private:
    QToolButton *addChip(const QString &text, const QString &tip, int flag);

    QList<QToolButton *> m_chips;
    QToolButton *m_clear = nullptr;
    QToolButton *m_area = nullptr;
};

} // namespace pl
