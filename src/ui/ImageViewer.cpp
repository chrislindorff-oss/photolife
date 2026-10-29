#include "ui/ImageViewer.h"

#include "raw/RawPreview.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {

// The photo, drawn at any zoom without ever holding a magnified copy: zoomed
// in, only the part on screen is drawn, straight from the original; zoomed
// out, one copy the size it's shown at is kept. (Scaling the whole photo up
// would need gigabytes at high zoom.)
class ImageCanvas : public QWidget
{
public:
    explicit ImageCanvas(QWidget *parent) : QWidget(parent)
    {
        setAutoFillBackground(true);
        setBackgroundRole(QPalette::Dark);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setImage(const QImage &image)
    {
        m_image = image;
        m_reduced = QImage();
        m_message.clear();
        update();
    }
    void setMessage(const QString &message)
    {
        m_image = QImage();
        m_reduced = QImage();
        m_message = message;
        update();
    }
    // Device pixels per image pixel.
    void setScale(double scale)
    {
        if (scale != m_scale)
            m_reduced = QImage();
        m_scale = scale;
        update();
    }
    // The photo's size on screen, in logical pixels.
    QSize logicalImageSize() const
    {
        if (m_image.isNull())
            return {};
        const qreal dpr = devicePixelRatioF();
        return QSize(std::max(1, int(std::lround(m_image.width() * m_scale / dpr))),
                     std::max(1, int(std::lround(m_image.height() * m_scale / dpr))));
    }
    // Where it starts in the canvas: centred when smaller than it.
    QPoint imageOffset() const
    {
        const QSize s = logicalImageSize();
        return QPoint((width() - s.width()) / 2, (height() - s.height()) / 2);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPainter p(this);
        if (m_image.isNull()) {
            p.setPen(palette().color(QPalette::BrightText));
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_message);
            return;
        }
        const QRect target(imageOffset(), logicalImageSize());
        const QRect exposed = event->rect() & target;
        if (exposed.isEmpty())
            return;
        if (m_scale < 1.0) {
            // Smaller than the photo: one smoothly reduced copy, drawn 1:1.
            const qreal dpr = devicePixelRatioF();
            const QSize device(std::max(1, int(std::lround(m_image.width() * m_scale))),
                               std::max(1, int(std::lround(m_image.height() * m_scale))));
            if (m_reduced.size() != device) {
                m_reduced = m_image.scaled(device, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                m_reduced.setDevicePixelRatio(dpr);
            }
            p.drawImage(target.topLeft(), m_reduced);
            return;
        }
        // Actual pixels or closer: just the exposed part, crisp.
        const double perLogical = m_scale / devicePixelRatioF();   // logical px per image px
        const QRectF source((exposed.left() - target.left()) / perLogical,
                            (exposed.top() - target.top()) / perLogical,
                            exposed.width() / perLogical, exposed.height() / perLogical);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(QRectF(exposed), m_image, source);
    }

private:
    QImage m_image;
    mutable QImage m_reduced;
    QString m_message;
    double m_scale = 1.0;
};

ImageViewer::ImageViewer(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Photo"));
    setModal(false);

    m_image = new ImageCanvas(this);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_image);
    // The image label is sized by rescale(): the picture, or the viewport if
    // that's bigger (so a small picture stays centred).
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_scroll->viewport()->installEventFilter(this);

    m_caption = new QLabel(this);
    m_caption->setAlignment(Qt::AlignCenter);
    m_caption->setWordWrap(true);

    m_counter = new QLabel(this);
    m_counter->setAlignment(Qt::AlignCenter);
    m_counter->setEnabled(false);

    auto *prev = new QPushButton(tr("← Previous"), this);
    auto *next = new QPushButton(tr("Next →"), this);
    connect(prev, &QPushButton::clicked, this, [this] { step(-1); });
    connect(next, &QPushButton::clicked, this, [this] { step(1); });

    m_bestShot = new QPushButton(this);
    m_bestShot->hide();
    connect(m_bestShot, &QPushButton::clicked, this, [this] {
        if (m_items.isEmpty())
            return;
        const Item &item = m_items.at(m_index);
        if (item.captureId > 0 && item.canBestShot)
            emit bestShotToggleRequested(item.captureId, !item.bestShot);
    });

    m_viewOnMap = new QPushButton(tr("View on Map"), this);
    m_viewOnMap->hide();
    connect(m_viewOnMap, &QPushButton::clicked, this, [this] {
        if (m_items.isEmpty())
            return;
        const Item &item = m_items.at(m_index);
        if (!item.hasGps)
            return;
        QDesktopServices::openUrl(QUrl(QStringLiteral(
            "https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=16/%1/%2")
            .arg(item.latitude, 0, 'f', 6)
            .arg(item.longitude, 0, 'f', 6)));
    });

    m_reject = new QPushButton(this);
    m_reject->hide();
    connect(m_reject, &QPushButton::clicked, this, &ImageViewer::toggleReject);

    auto *nav = new QHBoxLayout;
    nav->addWidget(prev);
    nav->addStretch(1);
    nav->addWidget(m_bestShot);
    nav->addWidget(m_viewOnMap);
    nav->addWidget(m_reject);
    nav->addStretch(1);
    nav->addWidget(m_counter);
    nav->addStretch(1);
    nav->addWidget(next);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_scroll, 1);
    layout->addWidget(m_caption);
    layout->addLayout(nav);

    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QSize s = screen->availableSize();
        resize(s.width() * 4 / 5, s.height() * 4 / 5);
    } else {
        resize(1000, 750);
    }
}

void ImageViewer::setItems(const QVector<Item> &items, int startIndex)
{
    m_items = items;
    m_index = qBound(0, startIndex, qMax(0, items.size() - 1));
    showCurrent();
}

void ImageViewer::step(int delta)
{
    if (m_items.isEmpty())
        return;
    const int next = m_index + delta;
    if (next < 0 || next >= m_items.size())
        return;
    m_index = next;
    showCurrent();
}

void ImageViewer::showCurrent()
{
    if (m_items.isEmpty()) {
        m_image->setMessage(tr("No image."));
        m_caption->clear();
        m_counter->clear();
        m_bestShot->hide();
        m_viewOnMap->hide();
        m_reject->hide();
        return;
    }

    const Item &item = m_items.at(m_index);
    QImageReader reader(item.path);
    reader.setAutoTransform(true);
    m_current = reader.read();

    if (m_current.isNull() && raw::isAvailable()) {
        // QImageReader can't decode camera-RAW formats; fall back to the
        // embedded preview JPEG that RAW files carry (same source the
        // thumbnail cache uses), at full size (no downscale cap).
        m_current = raw::extractPreview(item.path, 0);
    }

    if (m_current.isNull()) {
        m_image->setMessage(tr("Preview unavailable for\n%1").arg(item.path));
        m_image->resize(m_scroll->viewport()->size());
    } else {
        m_image->setImage(m_current);
        rescale();
    }

    m_caption->setText(item.caption);
    updateCounter();
    setWindowTitle(item.title.isEmpty() ? tr("Photo") : item.title);
    updateBestShotButton();
    updateMapButton();
    updateRejectButton();
}

void ImageViewer::updateRejectButton()
{
    if (m_items.isEmpty() || !m_items.at(m_index).canReject) {
        m_reject->hide();
        return;
    }
    const bool rejected = m_items.at(m_index).rejected;
    m_reject->setText(rejected ? tr("✕ Rejected — R to keep") : tr("✕ Reject (R)"));
    m_reject->setStyleSheet(rejected ? QStringLiteral("QPushButton { color: #c33; font-weight: bold; }")
                                     : QString());
    m_reject->show();
}

void ImageViewer::toggleReject()
{
    if (m_items.isEmpty() || !m_items.at(m_index).canReject)
        return;
    const int index = m_index;
    const bool reject = !m_items.at(index).rejected;
    emit rejectToggleRequested(index, reject);
    // On to the next photo, as when culling; taking one back stays on it.
    if (reject && m_items.at(index).rejected && index == m_index)
        step(1);
}

void ImageViewer::updateItem(int index, const Item &item)
{
    if (index < 0 || index >= m_items.size())
        return;
    const bool samePhoto = m_items.at(index).path == item.path;
    m_items[index] = item;
    if (index != m_index)
        return;
    if (!samePhoto) {
        showCurrent();
        return;
    }
    m_caption->setText(item.caption);
    setWindowTitle(item.title.isEmpty() ? tr("Photo") : item.title);
    updateBestShotButton();
    updateMapButton();
    updateRejectButton();
}

void ImageViewer::updateMapButton()
{
    if (m_items.isEmpty()) {
        m_viewOnMap->hide();
        return;
    }
    m_viewOnMap->setVisible(m_items.at(m_index).hasGps);
}

void ImageViewer::updateBestShotButton()
{
    if (m_items.isEmpty()) {
        m_bestShot->hide();
        return;
    }
    const Item &item = m_items.at(m_index);
    m_bestShot->setVisible(item.captureId > 0);
    m_bestShot->setEnabled(item.canBestShot);
    if (!item.canBestShot)
        m_bestShot->setText(tr("☆ Identify to add a best shot"));
    else
        m_bestShot->setText(item.bestShot ? tr("★ Remove from Best Shots")
                                          : tr("☆ Add to Best Shots"));
}

void ImageViewer::markBestShot(qint64 captureId, bool on)
{
    if (captureId <= 0)
        return;
    for (Item &item : m_items) {
        if (item.captureId == captureId)
            item.bestShot = on;
    }
    updateBestShotButton();
}

void ImageViewer::updateCounter()
{
    if (m_items.isEmpty())
        return;
    QString text = tr("%1 of %2").arg(m_index + 1).arg(m_items.size());
    if (!m_current.isNull())
        text += m_fit ? tr("  ·  fit") : tr("  ·  %1%").arg(std::lround(m_scale * 100));
    m_counter->setText(text);
}

double ImageViewer::fitScale() const
{
    if (m_current.isNull())
        return 1.0;
    const QSizeF vp = QSizeF(m_scroll->viewport()->size()) * devicePixelRatioF();
    // Never blown up past actual pixels just to fill the window.
    return std::min({1.0, vp.width() / m_current.width(), vp.height() / m_current.height()});
}

QPoint ImageViewer::viewportCenter() const
{
    return m_scroll->viewport()->rect().center();
}

// Draws the image at the current zoom, then scrolls so m_spot is in the
// middle of the view.
void ImageViewer::rescale()
{
    if (m_current.isNull())
        return;
    const double scale = m_fit ? fitScale() : m_scale;
    m_image->setScale(scale);
    const QSize logical = m_image->logicalImageSize();
    m_image->resize(logical.expandedTo(m_scroll->viewport()->size()));

    // Centre on the spot.
    const QSize vp = m_scroll->viewport()->size();
    const QPoint offset = m_image->imageOffset();
    m_scroll->horizontalScrollBar()->setValue(
        int(offset.x() + m_spot.x() * logical.width() - vp.width() / 2.0));
    m_scroll->verticalScrollBar()->setValue(
        int(offset.y() + m_spot.y() * logical.height() - vp.height() / 2.0));
    m_scroll->viewport()->setCursor(m_image->width() > vp.width() || m_image->height() > vp.height()
                                        ? Qt::OpenHandCursor
                                        : Qt::ArrowCursor);
    updateCounter();
}

void ImageViewer::zoomTo(double scale, QPoint anchor)
{
    if (m_current.isNull())
        return;
    // The image point under the anchor, as a fraction of the image.
    const QSize logical = m_image->logicalImageSize();
    if (!logical.isEmpty()) {
        const QPoint offset = m_image->imageOffset();
        const QPointF at(m_scroll->horizontalScrollBar()->value() + anchor.x() - offset.x(),
                         m_scroll->verticalScrollBar()->value() + anchor.y() - offset.y());
        const QPointF frac(std::clamp(at.x() / logical.width(), 0.0, 1.0),
                           std::clamp(at.y() / logical.height(), 0.0, 1.0));
        // Then the view is moved so that point stays under the anchor.
        const double newScale = scale <= 0 ? fitScale() : scale;
        const QSizeF newLogical = QSizeF(m_current.size()) * newScale / devicePixelRatioF();
        const QPointF vpCenter = QPointF(m_scroll->viewport()->size().width(), m_scroll->viewport()->size().height()) / 2.0;
        m_spot = QPointF(std::clamp(frac.x() + (vpCenter.x() - anchor.x()) / newLogical.width(), 0.0, 1.0),
                         std::clamp(frac.y() + (vpCenter.y() - anchor.y()) / newLogical.height(), 0.0, 1.0));
    }
    m_fit = scale <= 0;
    if (!m_fit)
        m_scale = std::clamp(scale, 0.05, 16.0);
    rescale();
}

void ImageViewer::zoomBy(double factor, QPoint anchor)
{
    const double from = m_fit ? fitScale() : m_scale;
    double to = from * factor;
    // Stop at actual pixels on the way through, and at fit on the way out.
    if ((from < 1.0 && to > 1.0) || (from > 1.0 && to < 1.0))
        to = 1.0;
    if (to <= fitScale() && factor < 1.0) {
        zoomTo(0, anchor);
        return;
    }
    zoomTo(to, anchor);
}

void ImageViewer::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    rescale();
}

bool ImageViewer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_scroll->viewport())
        return QDialog::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::Wheel: {
        auto *wheel = static_cast<QWheelEvent *>(event);
        const int delta = wheel->angleDelta().y();
        if (delta != 0)
            zoomBy(std::pow(1.25, delta / 120.0), wheel->position().toPoint());
        return true;
    }
    case QEvent::MouseButtonDblClick: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        zoomTo(m_fit ? 1.0 : 0.0, mouse->position().toPoint());
        return true;
    }
    case QEvent::MouseButtonPress: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            m_panning = true;
            m_panFrom = mouse->position().toPoint();
            if (m_scroll->viewport()->cursor().shape() == Qt::OpenHandCursor)
                m_scroll->viewport()->setCursor(Qt::ClosedHandCursor);
            return true;
        }
        break;
    }
    case QEvent::MouseMove: {
        if (!m_panning)
            break;
        auto *mouse = static_cast<QMouseEvent *>(event);
        const QPoint d = mouse->position().toPoint() - m_panFrom;
        m_panFrom = mouse->position().toPoint();
        m_scroll->horizontalScrollBar()->setValue(m_scroll->horizontalScrollBar()->value() - d.x());
        m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->value() - d.y());
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (!m_panning)
            break;
        m_panning = false;
        if (m_scroll->viewport()->cursor().shape() == Qt::ClosedHandCursor)
            m_scroll->viewport()->setCursor(Qt::OpenHandCursor);
        // Remember the spot, so the next photo opens there too.
        const QSize logical = m_image->logicalImageSize();
        if (!logical.isEmpty()) {
            const QSize vp = m_scroll->viewport()->size();
            const QPoint offset = m_image->imageOffset();
            m_spot = QPointF(std::clamp((m_scroll->horizontalScrollBar()->value() + vp.width() / 2.0 - offset.x())
                                            / logical.width(), 0.0, 1.0),
                             std::clamp((m_scroll->verticalScrollBar()->value() + vp.height() / 2.0 - offset.y())
                                            / logical.height(), 0.0, 1.0));
        }
        return true;
    }
    default:
        break;
    }
    return QDialog::eventFilter(watched, event);
}

void ImageViewer::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Up:
    case Qt::Key_PageUp:
        step(-1);
        break;
    case Qt::Key_Right:
    case Qt::Key_Down:
    case Qt::Key_PageDown:
    case Qt::Key_Space:
        step(1);
        break;
    case Qt::Key_Home:
        m_index = 0;
        showCurrent();
        break;
    case Qt::Key_End:
        m_index = qMax(0, m_items.size() - 1);
        showCurrent();
        break;
    case Qt::Key_A: {
        // Actual pixels at the spot under the pointer (or the middle), or back to fit.
        QPoint at = m_scroll->viewport()->mapFromGlobal(QCursor::pos());
        if (!m_scroll->viewport()->rect().contains(at))
            at = viewportCenter();
        zoomTo(m_fit || std::abs(m_scale - 1.0) > 1e-6 ? 1.0 : 0.0, at);
        break;
    }
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomBy(1.25, viewportCenter());
        break;
    case Qt::Key_Minus:
        zoomBy(0.8, viewportCenter());
        break;
    case Qt::Key_0:
    case Qt::Key_F:
        m_spot = QPointF(0.5, 0.5);
        zoomTo(0, viewportCenter());
        break;
    case Qt::Key_R:
        if (!m_items.isEmpty() && m_items.at(m_index).canReject)
            toggleReject();
        else
            QDialog::keyPressEvent(event);
        break;
    default:
        QDialog::keyPressEvent(event);
    }
}

} // namespace pl
