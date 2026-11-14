#include "ui/FlowLayout.h"

#include <QSpacerItem>
#include <QStyle>
#include <QWidget>

namespace pl {

FlowLayout::FlowLayout(QWidget *parent, int hSpacing, int vSpacing)
    : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
    m_items.append(item);
}

void FlowLayout::addSpacing(int width)
{
    addItem(new QSpacerItem(width, 0, QSizePolicy::Fixed, QSizePolicy::Minimum));
}

void FlowLayout::addStretch()
{
    addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));
}

QLayoutItem *FlowLayout::takeAt(int index)
{
    return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
}

int FlowLayout::horizontalSpacing() const
{
    return m_hSpace >= 0 ? m_hSpace : smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}

int FlowLayout::verticalSpacing() const
{
    return m_vSpace >= 0 ? m_vSpace : smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const
{
    QObject *parent = this->parent();
    if (!parent)
        return 6;
    if (parent->isWidgetType()) {
        auto *pw = static_cast<QWidget *>(parent);
        return pw->style()->pixelMetric(pm, nullptr, pw);
    }
    return static_cast<QLayout *>(parent)->spacing();
}

int FlowLayout::heightForWidth(int width) const
{
    return doLayout(QRect(0, 0, width, 0), true);
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem *item : m_items) {
        if (!item->isEmpty())
            size = size.expandedTo(item->minimumSize());
    }
    const QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::sizeHint() const
{
    // All on one line.
    int width = 0, height = 0, n = 0;
    for (const QLayoutItem *item : m_items) {
        if (item->isEmpty())
            continue;
        width += item->sizeHint().width() + (n++ > 0 ? horizontalSpacing() : 0);
        height = std::max(height, item->sizeHint().height());
    }
    const QMargins m = contentsMargins();
    return QSize(width + m.left() + m.right(), height + m.top() + m.bottom());
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
    const QMargins m = contentsMargins();
    const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    const int hSpace = horizontalSpacing();
    const int vSpace = verticalSpacing();

    // Break into lines, each item at its preferred width (never under its minimum).
    QList<QList<QLayoutItem *>> lines;
    QList<QLayoutItem *> line;
    int lineWidth = 0;
    // An item whose width policy ignores its size hint (a label eliding a
    // long path) starts at its minimum and takes spare space instead.
    const auto ignoresHint = [](const QLayoutItem *item) {
        const QWidget *w = item->widget();
        return w && (w->sizePolicy().horizontalPolicy() & QSizePolicy::IgnoreFlag);
    };
    const auto stretches = [&](const QLayoutItem *item) {
        return (item->expandingDirections() & Qt::Horizontal) || ignoresHint(item);
    };
    const auto widthOf = [&](const QLayoutItem *item) {
        const QWidget *w = item->widget();
        const int min = w ? std::max(w->minimumWidth(), item->minimumSize().width()) : item->minimumSize().width();
        return ignoresHint(item) ? min : std::max(item->sizeHint().width(), min);
    };
    for (QLayoutItem *item : m_items) {
        if (item->isEmpty())
            continue;
        const bool spacer = item->spacerItem() != nullptr;
        if (line.isEmpty() && spacer)
            continue;   // no indent at a line's start
        const int w = std::min(widthOf(item), area.width());
        const int needed = line.isEmpty() ? w : lineWidth + hSpace + w;
        if (!line.isEmpty() && needed > area.width()) {
            lines << line;
            line.clear();
            lineWidth = 0;
            if (spacer)
                continue;
            lineWidth = w;
        } else {
            lineWidth = needed;
        }
        line << item;
    }
    if (!line.isEmpty())
        lines << line;

    int y = area.y();
    for (const QList<QLayoutItem *> &l : std::as_const(lines)) {
        int used = 0, height = 0, expanding = 0;
        for (int i = 0; i < l.size(); ++i) {
            used += std::min(widthOf(l[i]), area.width()) + (i > 0 ? hSpace : 0);
            height = std::max(height, l[i]->sizeHint().height());
            if (stretches(l[i]))
                ++expanding;
        }
        const int spare = std::max(0, area.width() - used);
        int x = area.x();
        for (int i = 0; i < l.size(); ++i) {
            int w = std::min(widthOf(l[i]), area.width());
            if (expanding > 0 && stretches(l[i]))
                w += spare / expanding;
            const int h = std::min(height, std::max(l[i]->sizeHint().height(), l[i]->minimumSize().height()));
            if (!testOnly)
                l[i]->setGeometry(QRect(x, y + (height - h) / 2, w, h));
            x += w + hSpace;
        }
        y += height + vSpace;
    }
    if (!lines.isEmpty())
        y -= vSpace;
    return y - rect.y() + m.bottom();
}

} // namespace pl
