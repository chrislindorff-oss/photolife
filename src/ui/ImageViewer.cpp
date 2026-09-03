#include "ui/ImageViewer.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

namespace pl {

ImageViewer::ImageViewer(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Photo"));
    setModal(false);

    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setBackgroundRole(QPalette::Dark);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_image);
    m_scroll->setWidgetResizable(true);
    m_scroll->setAlignment(Qt::AlignCenter);

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

    auto *nav = new QHBoxLayout;
    nav->addWidget(prev);
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
        m_image->setText(tr("No image."));
        m_caption->clear();
        m_counter->clear();
        return;
    }

    const Item &item = m_items.at(m_index);
    QImageReader reader(item.path);
    reader.setAutoTransform(true);
    m_current = reader.read();

    if (m_current.isNull()) {
        m_image->setPixmap({});
        m_image->setText(tr("Preview unavailable for\n%1").arg(item.path));
    } else {
        rescale();
    }

    m_caption->setText(item.caption);
    m_counter->setText(tr("%1 of %2").arg(m_index + 1).arg(m_items.size()));
    setWindowTitle(item.caption.isEmpty() ? tr("Photo") : item.caption);
}

void ImageViewer::rescale()
{
    if (m_current.isNull())
        return;
    const QSize target = m_scroll->viewport()->size();
    const QImage scaled = (m_current.width() > target.width()
                           || m_current.height() > target.height())
                              ? m_current.scaled(target, Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation)
                              : m_current;
    m_image->setPixmap(QPixmap::fromImage(scaled));
    m_image->setText(QString());
}

void ImageViewer::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    rescale();
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
    default:
        QDialog::keyPressEvent(event);
    }
}

} // namespace pl
