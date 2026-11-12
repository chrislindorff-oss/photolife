#include "ui/CompareViewer.h"

#include "raw/RawPreview.h"
#include "ui/Theme.h"

#include <QFutureWatcher>
#include <QGridLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

namespace pl {

// One photo of the comparison: draws its picture at the shared zoom and spot.
class ComparePane : public QWidget
{
public:
    ComparePane(CompareViewer *viewer, const QString &path) : QWidget(viewer), m_viewer(viewer)
    {
        setMouseTracking(true);
        setMinimumSize(200, 150);
        setCursor(Qt::OpenHandCursor);
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher] {
            m_image = watcher->result();
            m_loaded = true;
            watcher->deleteLater();
            update();
        });
        watcher->setFuture(QtConcurrent::run([path] {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            QImage image = reader.read();
            if (image.isNull() && raw::isAvailable())
                image = raw::extractPreview(path, 0);
            return image;
        }));
    }

    bool hasImage() const { return !m_image.isNull(); }

    // Device pixels per image pixel that fit the picture in the pane.
    double fitScale() const
    {
        if (m_image.isNull())
            return 1.0;
        const double dpr = devicePixelRatioF();
        return std::min(width() * dpr / m_image.width(), height() * dpr / m_image.height());
    }
    double scale() const { return m_viewer->m_fit ? fitScale() : m_viewer->m_scale; }

    // A point in the pane as a fraction of the picture.
    QPointF toPicture(QPointF at) const
    {
        if (m_image.isNull())
            return {0.5, 0.5};
        const double s = scale() / devicePixelRatioF();   // widget pixels per image pixel
        const QPointF centre(width() / 2.0, height() / 2.0);
        const QPointF spot = m_viewer->m_fit ? QPointF(0.5, 0.5) : m_viewer->m_spot;
        return {spot.x() + (at.x() - centre.x()) / (s * m_image.width()),
                spot.y() + (at.y() - centre.y()) / (s * m_image.height())};
    }
    QSizeF imageSize() const { return m_image.size(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(20, 20, 20));
        if (m_image.isNull()) {
            p.setPen(QColor(200, 200, 200));
            p.drawText(rect(), Qt::AlignCenter, m_loaded ? tr("Preview unavailable") : tr("Loading…"));
            return;
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform, scale() < 1.0);
        const double s = scale() / devicePixelRatioF();
        const QPointF spot = m_viewer->m_fit ? QPointF(0.5, 0.5) : m_viewer->m_spot;
        const QSizeF shown(m_image.width() * s, m_image.height() * s);
        const QPointF topLeft(width() / 2.0 - spot.x() * shown.width(), height() / 2.0 - spot.y() * shown.height());
        p.drawImage(QRectF(topLeft, shown), m_image);
        if (m_viewer->paneUnderPointer() == this && m_viewer->m_panes.size() > 1) {
            p.setPen(QPen(themeColors(currentThemeVariant()).primaryAccent, 3));
            p.setBrush(Qt::NoBrush);
            p.drawRect(rect().adjusted(1, 1, -2, -2));
        }
    }

    void enterEvent(QEnterEvent *) override
    {
        for (ComparePane *pane : m_viewer->m_panes)
            pane->update();   // the frame follows the pointer
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const double steps = event->angleDelta().y() / 120.0;
        if (steps != 0)
            m_viewer->zoomAt(this, std::pow(1.25, steps), event->position());
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragFrom = event->position();
            setCursor(Qt::ClosedHandCursor);
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton) || m_viewer->m_fit)
            return;
        m_viewer->panBy(this, event->position() - m_dragFrom);
        m_dragFrom = event->position();
    }
    void mouseReleaseEvent(QMouseEvent *) override { setCursor(Qt::OpenHandCursor); }
    void mouseDoubleClickEvent(QMouseEvent *event) override { m_viewer->toggleOneToOne(this, event->position()); }

private:
    CompareViewer *m_viewer;
    QImage m_image;
    bool m_loaded = false;
    QPointF m_dragFrom;
};

CompareViewer::CompareViewer(const QVector<ImageViewer::Item> &items, const QList<int> &indexes, QWidget *parent)
    : QDialog(parent), m_indexes(indexes), m_items(items)
{
    setWindowTitle(tr("Compare %n Photo(s)", nullptr, int(items.size())));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *grid = new QGridLayout;
    grid->setSpacing(6);
    // Two or three in a row; four as two by two.
    const int columns = items.size() == 4 ? 2 : int(items.size());
    for (int i = 0; i < items.size(); ++i) {
        auto *pane = new ComparePane(this, items.at(i).path);
        auto *caption = new QLabel(this);
        caption->setAlignment(Qt::AlignCenter);
        caption->setWordWrap(true);
        auto *column = new QVBoxLayout;
        column->addWidget(pane, 1);
        column->addWidget(caption);
        grid->addLayout(column, i / columns, i % columns);
        m_panes << pane;
        m_captions << caption;
        m_notes << QString();
    }
    auto *hint = new QLabel(tr("The wheel zooms them together · drag to pan · A or a double-click: 1:1 at the pointer · "
                               "R rejects, B stars, N names the one under the pointer · Esc closes"),
                            this);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(grid, 1);
    layout->addWidget(hint);
    refreshCaptions();
    resize(1500, 900);
}

void CompareViewer::refreshCaptions()
{
    for (int i = 0; i < m_items.size(); ++i) {
        const ImageViewer::Item &it = m_items.at(i);
        QStringList marks;
        if (it.rejected)
            marks << tr("✕ reject");
        if (it.bestShot)
            marks << tr("★ best shot");
        if (!m_notes.at(i).isEmpty())
            marks << m_notes.at(i);
        m_captions[i]->setText(QStringLiteral("<b>%1</b>%2").arg(it.title.toHtmlEscaped(),
                                                               marks.isEmpty() ? QString()
                                                                               : QStringLiteral(" · ")
                                                                                     + marks.join(QStringLiteral(" · "))
                                                                                           .toHtmlEscaped()));
    }
}

void CompareViewer::updateItem(int index, const ImageViewer::Item &item, const QString &note)
{
    const int at = m_indexes.indexOf(index);
    if (at < 0)
        return;
    m_items[at] = item;
    if (!note.isNull())
        m_notes[at] = note;
    refreshCaptions();
}

void CompareViewer::setNote(int index, const QString &note)
{
    const int at = m_indexes.indexOf(index);
    if (at < 0)
        return;
    m_notes[at] = note;
    refreshCaptions();
}

void CompareViewer::setView(bool fit, double scale, QPointF spot)
{
    m_fit = fit;
    m_scale = std::clamp(scale, 0.02, 16.0);
    m_spot = QPointF(std::clamp(spot.x(), 0.0, 1.0), std::clamp(spot.y(), 0.0, 1.0));
    for (ComparePane *pane : std::as_const(m_panes))
        pane->update();
}

ComparePane *CompareViewer::paneUnderPointer() const
{
    for (ComparePane *pane : m_panes) {
        if (pane->rect().contains(pane->mapFromGlobal(QCursor::pos())))
            return pane;
    }
    return nullptr;
}

void CompareViewer::zoomAt(ComparePane *pane, double factor, QPointF at)
{
    if (!pane->hasImage())
        return;
    // The picture point under the pointer stays put.
    const QPointF before = pane->toPicture(at);
    const double scale = pane->scale() * factor;
    if (scale <= pane->fitScale()) {
        setView(true, m_scale, {0.5, 0.5});
        return;
    }
    m_fit = false;
    m_scale = std::min(scale, 16.0);
    const double s = m_scale / pane->devicePixelRatioF();
    const QPointF centre(pane->width() / 2.0, pane->height() / 2.0);
    const QSizeF size = pane->imageSize();
    setView(false, m_scale, {before.x() - (at.x() - centre.x()) / (s * size.width()),
                             before.y() - (at.y() - centre.y()) / (s * size.height())});
}

void CompareViewer::toggleOneToOne(ComparePane *pane, QPointF at)
{
    if (!pane || !pane->hasImage())
        return;
    if (!m_fit) {
        setView(true, m_scale, {0.5, 0.5});
        return;
    }
    const QPointF spot = pane->toPicture(at);
    setView(false, 1.0, spot);
}

void CompareViewer::panBy(ComparePane *pane, QPointF delta)
{
    if (!pane->hasImage())
        return;
    const double s = m_scale / pane->devicePixelRatioF();
    const QSizeF size = pane->imageSize();
    setView(false, m_scale, {m_spot.x() - delta.x() / (s * size.width()), m_spot.y() - delta.y() / (s * size.height())});
}

void CompareViewer::keyPressEvent(QKeyEvent *event)
{
    ComparePane *pane = paneUnderPointer();
    if (!pane && !m_panes.isEmpty())
        pane = m_panes.first();
    const int at = int(m_panes.indexOf(pane));
    switch (event->key()) {
    case Qt::Key_A:
        if (pane)
            toggleOneToOne(pane, pane->mapFromGlobal(QCursor::pos()));
        return;
    case Qt::Key_0:
    case Qt::Key_F:
        setView(true, m_scale, {0.5, 0.5});
        return;
    case Qt::Key_R:
        if (at >= 0 && m_items.at(at).canReject)
            emit rejectToggleRequested(m_indexes.at(at), !m_items.at(at).rejected);
        return;
    case Qt::Key_B:
        if (at >= 0)
            emit bestToggleRequested(m_indexes.at(at), !m_items.at(at).bestShot);
        return;
    case Qt::Key_N:
        if (at >= 0 && m_items.at(at).canName)
            emit nameRequested(m_indexes.at(at));
        return;
    default:
        QDialog::keyPressEvent(event);
    }
}

} // namespace pl
