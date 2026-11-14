#pragma once

#include <QLayout>
#include <QStyle>
#include <QList>

namespace pl {

// A row of widgets that wraps onto further lines when it's narrower than
// they need: on a wide window it's one line, as a QHBoxLayout would be;
// narrow, its minimum width is only its widest item's. Spare width on each
// line goes to the items that expand horizontally (a label with the folder's
// name, say). Spacing items (addSpacing) are dropped at a line's start.
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent = nullptr, int hSpacing = -1, int vSpacing = -1);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    void addSpacing(int width);
    // Spare width on its line goes here (as QBoxLayout::addStretch()).
    void addStretch();
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;

private:
    int doLayout(const QRect &rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QList<QLayoutItem *> m_items;
    int m_hSpace;
    int m_vSpace;
};

} // namespace pl
