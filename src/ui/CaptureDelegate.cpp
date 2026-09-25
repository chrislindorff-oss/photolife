#include "ui/CaptureDelegate.h"

#include "model/CaptureListModel.h"
#include "ui/Theme.h"

#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QPolygonF>

#include <cmath>

namespace pl {
namespace {

// A five-pointed star polygon inscribed in `box`, point-up.
QPolygonF makeStar(const QRectF &box)
{
    constexpr double kPi = 3.14159265358979323846;
    const QPointF c = box.center();
    const qreal outer = qMin(box.width(), box.height()) / 2.0;
    const qreal inner = outer * 0.42;
    QPolygonF star;
    for (int i = 0; i < 10; ++i) {
        const qreal r = (i % 2 == 0) ? outer : inner;
        const qreal a = -kPi / 2.0 + i * kPi / 5.0;
        star << QPointF(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
    }
    return star;
}

} // namespace

void CaptureDelegate::initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    option->text.clear();
    option->icon = QIcon();
}

QSize CaptureDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    int fields = 0;
    if (const auto *m = qobject_cast<const model::CaptureListModel *>(index.model()))
        fields = m->captionFields();
    int lines = 0;
    for (int b = fields & ~model::CaptureListModel::CaptionFileType; b; b &= (b - 1))
        ++lines;
    lines = std::max(lines, 1);
    return QSize(option.decorationSize.width() + 20,
                option.decorationSize.height() + 20 + lines * 16);
}

void CaptureDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    // Deliberately not calling the base class's paint() at all: even with
    // icon/text cleared, it still computes its own internal "text rect"
    // purely to paint a selection-highlight fill behind it, and that
    // rect collapses to a small stray sliver landing wherever Qt's
    // layout math happens to put it — visible as a small coloured
    // artifact wherever it overlaps our own caption text. We draw the
    // background and every bit of content ourselves instead; the
    // rounded-and-filled selection highlight below already makes
    // selection unambiguous.
    const pl::ThemeColors &theme = pl::themeColors(pl::currentThemeVariant());
    const bool selected = option.state & QStyle::State_Selected;

    painter->fillRect(option.rect, option.palette.color(QPalette::Base));
    if (selected) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme.selectedRow);
        painter->drawRoundedRect(option.rect.adjusted(1, 1, -2, -2), theme.radius, theme.radius);
        painter->restore();
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(selected ? theme.primaryAccent : theme.border, selected ? 2 : 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(option.rect.adjusted(1, 1, -2, -2), theme.radius, theme.radius);
    painter->restore();

    // Fit the thumbnail into the decoration box ourselves (KeepAspectRatio,
    // centered) so we know its exact rect — needed to anchor the pills and
    // caption to the photo's real edge rather than the (usually taller or
    // wider) bounding box reserved for it.
    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    const QRect decoBox(option.rect.left() + (option.rect.width() - option.decorationSize.width()) / 2,
                        option.rect.top(), option.decorationSize.width(),
                        option.decorationSize.height());
    QRect imageRect = decoBox;
    if (!icon.isNull()) {
        const QSize native = icon.availableSizes().value(0, option.decorationSize);
        const QSize fitted = native.scaled(option.decorationSize, Qt::KeepAspectRatio);
        imageRect = QRect(QPoint(0, 0), fitted);
        imageRect.moveCenter(decoBox.center());
        painter->drawPixmap(imageRect, icon.pixmap(native));
    }
    const int imageBottom = imageRect.bottom();

    const QString status = index.data(model::CaptureListModel::MatchStatusRole).toString();
    QColor colour;
    if (status == QLatin1String("auto"))
        colour = theme.statusAuto;
    else if (status == QLatin1String("confirmed"))
        colour = theme.statusConfirmed;
    else if (status == QLatin1String("pending"))
        colour = theme.statusPending;
    else
        colour = theme.statusUnmatched;

    const int d = 10;
    const QRect r = option.rect.adjusted(6, 6, 0, 0);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(colour);
    painter->drawEllipse(QRect(r.left(), r.top(), d, d));
    painter->restore();

    // A gold star just right of the status dot marks a capture the user has
    // starred as a best shot of its species.
    if (index.data(model::CaptureListModel::IsBestShotRole).toBool()) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(QColor(0, 0, 0, 90), 0.8));
        painter->setBrush(theme.bestShotGold);
        painter->drawPolygon(makeStar(QRectF(r.left() + d + 4, r.top() - 1, 13, 13)));
        painter->restore();
    }

    // File type and GPS are both drawn as a row of pills across the
    // bottom-center of the thumbnail, in the same style, side by side.
    QStringList badges;
    if (index.data(model::CaptureListModel::ShowFileTypeBadgeRole).toBool()) {
        const QString ext = index.data(model::CaptureListModel::ExtRole).toString().toUpper();
        if (!ext.isEmpty())
            badges << ext;
    }
    if (index.data(model::CaptureListModel::HasGpsRole).toBool())
        badges << QStringLiteral("GEO");

    if (!badges.isEmpty()) {
        QFont font = painter->font();
        font.setPointSize(7);
        font.setBold(true);
        QFontMetrics fm(font);

        const int padH = 6, pillH = 16, gap = 4;
        QList<int> widths;
        int totalW = -gap;
        for (const QString &b : std::as_const(badges)) {
            const int w = fm.horizontalAdvance(b) + padH * 2;
            widths << w;
            totalW += w + gap;
        }

        int x = option.rect.left() + (option.rect.width() - totalW) / 2;
        const int y = imageBottom - pillH - 3;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setFont(font);
        for (int i = 0; i < badges.size(); ++i) {
            const QRect pillRect(x, y, widths.at(i), pillH);
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme.badgeBackground);
            painter->drawRoundedRect(pillRect, pillH / 2.0, pillH / 2.0);
            painter->setPen(theme.badgeText);
            painter->drawText(pillRect, Qt::AlignCenter, badges.at(i));
            x += widths.at(i) + gap;
        }
        painter->restore();
    }

    const QString caption = index.data(Qt::DisplayRole).toString();
    if (!caption.isEmpty()) {
        QFont font = painter->font();
        if (font.pointSize() > 0)
            font.setPointSize(qMax(1, font.pointSize() - 1));
        else
            font.setPixelSize(qMax(1, font.pixelSize() - 1));
        QFontMetrics fm(font);
        const int lineH = 16;
        const int top = imageBottom + 4;
        const QRect textArea(option.rect.left() + 4, top, option.rect.width() - 8,
                             option.rect.bottom() - top);

        // Always the plain text colour: the caption sits below the icon,
        // outside any selection-highlight fill, so HighlightedText here
        // would render (near-)invisible against the ordinary background.
        // The border above already makes selection obvious.
        painter->save();
        painter->setFont(font);
        painter->setPen(option.palette.color(QPalette::Text));
        int y = textArea.top();
        for (const QString &line : caption.split(QLatin1Char('\n'))) {
            const QString elided = fm.elidedText(line, Qt::ElideRight, textArea.width());
            painter->drawText(QRect(textArea.left(), y, textArea.width(), lineH),
                              Qt::AlignHCenter | Qt::AlignVCenter, elided);
            y += lineH;
        }
        painter->restore();
    }
}

} // namespace pl
