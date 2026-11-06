#pragma once

#include <QStyledItemDelegate>

namespace pl {

// A ReferencePhotoModel taxon as a framed card: its photo fitted in a square
// at the top, then its scientific name (italic) and common name, each on its
// own line and elided to fit -- so nothing runs over the photo or is cut off.
class ReferencePhotoDelegate : public QStyledItemDelegate
{
public:
    // `photoPx`: the square the photo is fitted into.
    explicit ReferencePhotoDelegate(int photoPx, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    int m_photoPx;
};

} // namespace pl
