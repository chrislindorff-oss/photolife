#pragma once

#include <QStyledItemDelegate>

namespace pl {

// Draws the base thumbnail plus a small status pip in the corner, a best-shot
// star, and file-type/GPS badges -- the shared look for every CaptureListModel-
// backed photo grid in the app (library browse, best shots, and the export
// preview grid).
class CaptureDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    // Suppress the base class's own icon and (word-wrapping) text drawing —
    // both are drawn manually in paint() below. The icon is drawn ourselves
    // so we know its *exact* rect (Qt's own centering within decorationSize
    // isn't something we can reliably reproduce from the outside), and the
    // caption is drawn one elided line per field, so a long field value can
    // never wrap onto a second line and overflow the grid cell's fixed
    // height.
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override;

public:
    // With both icon and text cleared above, the base class's own sizeHint()
    // (which measures those two) collapses to nearly nothing — silently
    // decoupling the item's actual paint rect from the gridSize the view
    // configures, which is what left the border/caption misaligned with the
    // laid-out cell. Reporting the exact same size here (mirroring
    // MainWindow::applyCaptionFields()'s formula) keeps them locked together
    // regardless of which one the view actually uses for layout.
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
              const QModelIndex &index) const override;
};

} // namespace pl
