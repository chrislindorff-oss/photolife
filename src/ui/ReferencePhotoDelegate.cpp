#include "ui/ReferencePhotoDelegate.h"

#include "model/ReferencePhotoModel.h"
#include "net/PhotoCache.h"
#include "ui/Theme.h"

#include <QIcon>
#include <QPainter>
#include <QPainterPath>

namespace pl {

namespace {

constexpr int kGap = 3;   // between cards, each side
constexpr int kPad = 6;   // inside a card

} // namespace

ReferencePhotoDelegate::ReferencePhotoDelegate(int photoPx, QObject *parent)
    : QStyledItemDelegate(parent), m_photoPx(photoPx)
{
}

QSize ReferencePhotoDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    const QFontMetrics fm(option.font);
    return {m_photoPx + 2 * (kPad + kGap), m_photoPx + 2 * (kPad + kGap) + 4 + 2 * fm.height()};
}

void ReferencePhotoDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const
{
    using Model = model::ReferencePhotoModel;
    const ThemeColors &colors = themeColors(currentThemeVariant());
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    // The frame.
    const QRectF card = QRectF(option.rect).adjusted(kGap + 0.5, kGap + 0.5, -kGap - 0.5, -kGap - 0.5);
    QPainterPath frame;
    frame.addRoundedRect(card, colors.radius, colors.radius);
    painter->fillPath(frame, selected ? colors.selectedRow : colors.panel);
    painter->setPen(QPen(selected ? colors.primaryAccent : hovered ? colors.mutedText : colors.border,
                         selected ? 2.0 : 1.0));
    painter->drawPath(frame);

    // The photo, fitted in its square on a matte.
    const QRect square(option.rect.left() + (option.rect.width() - m_photoPx) / 2, option.rect.top() + kGap + kPad,
                       m_photoPx, m_photoPx);
    QColor matte = colors.text;
    matte.setAlpha(18);
    painter->fillRect(square, matte);
    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    const QPixmap source = icon.pixmap(QSize(net::PhotoCache::kMaxEdge, net::PhotoCache::kMaxEdge));
    if (!source.isNull()) {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        QPixmap fitted = source.scaled(QSize(m_photoPx, m_photoPx) * dpr, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
        fitted.setDevicePixelRatio(dpr);
        const QSizeF size = fitted.deviceIndependentSize();
        painter->drawPixmap(QPointF(square.left() + (square.width() - size.width()) / 2,
                                    square.top() + (square.height() - size.height()) / 2),
                            fitted);
    }

    // The names, a line each.
    const QString name = index.data(Model::NameRole).toString();
    QString common = index.data(Model::CommonNameRole).toString();
    if (common == name)
        common.clear();
    const int textWidth = int(card.width()) - 2 * kPad;
    const int left = int(card.left()) + kPad;
    QFont italic = option.font;
    italic.setItalic(true);
    const QFontMetrics fm(option.font);
    int y = square.bottom() + 1 + 4;
    painter->setFont(italic);
    painter->setPen(colors.text);
    painter->drawText(QRect(left, y, textWidth, fm.height()), Qt::AlignHCenter | Qt::AlignVCenter,
                      QFontMetrics(italic).elidedText(name, Qt::ElideRight, textWidth));
    y += fm.height();
    if (!common.isEmpty()) {
        painter->setFont(option.font);
        painter->setPen(colors.mutedText);
        painter->drawText(QRect(left, y, textWidth, fm.height()), Qt::AlignHCenter | Qt::AlignVCenter,
                          fm.elidedText(common, Qt::ElideRight, textWidth));
    }
    painter->restore();
}

} // namespace pl
