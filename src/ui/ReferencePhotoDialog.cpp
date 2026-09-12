#include "ui/ReferencePhotoDialog.h"

#include "net/PhotoCache.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {

ReferencePhotoDialog::ReferencePhotoDialog(net::PhotoCache &photos, QWidget *parent)
    : QDialog(parent), m_photos(photos)
{
    setModal(false);

    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setBackgroundRole(QPalette::Dark);
    m_image->setMinimumSize(200, 200);

    m_caption = new QLabel(this);
    m_caption->setTextFormat(Qt::RichText);
    m_caption->setAlignment(Qt::AlignCenter);
    m_caption->setWordWrap(true);

    m_openInat = new QPushButton(tr("Open on iNaturalist"), this);
    connect(m_openInat, &QPushButton::clicked, this, [this] {
        if (m_inatId > 0)
            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://www.inaturalist.org/taxa/%1").arg(m_inatId)));
    });

    auto *nav = new QHBoxLayout;
    nav->addStretch(1);
    nav->addWidget(m_openInat);
    nav->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_image, 1);
    layout->addWidget(m_caption);
    layout->addLayout(nav);

    connect(&m_photos, &net::PhotoCache::ready, this, [this](const QString &url) {
        if (url == m_url)
            applyPhoto(m_photos.photo(url));
    });

    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QSize s = screen->availableSize();
        resize(s.width() / 2, s.height() * 2 / 3);
    } else {
        resize(700, 600);
    }
}

void ReferencePhotoDialog::showTaxon(qint64 inatId, const QString &name, const QString &commonName,
                                     const QString &photoUrl, const QString &attribution)
{
    m_inatId = inatId;
    m_url = photoUrl;
    m_openInat->setEnabled(inatId > 0);
    setWindowTitle(name.isEmpty() ? tr("Reference Photo") : name);

    QString caption = QStringLiteral("<h3 style='margin:0'>%1</h3>").arg(name.toHtmlEscaped());
    if (!commonName.isEmpty() && commonName != name)
        caption += QStringLiteral("<div>%1</div>").arg(commonName.toHtmlEscaped());
    if (!attribution.isEmpty())
        caption +=
            QStringLiteral("<div style='color:gray'>%1</div>").arg(attribution.toHtmlEscaped());
    m_caption->setText(caption);

    if (photoUrl.isEmpty()) {
        applyPhoto({});
        m_image->setText(tr("No reference photo cached for this species yet.\n"
                            "Use \"Fetch Reference Photos\" to download one."));
    } else {
        const QPixmap pm = m_photos.photo(photoUrl);
        if (pm.isNull())
            m_image->setText(tr("Loading…"));
        applyPhoto(pm);
    }
}

void ReferencePhotoDialog::applyPhoto(const QPixmap &pixmap)
{
    m_current = pixmap;
    rescale();
}

void ReferencePhotoDialog::rescale()
{
    if (m_current.isNull())
        return;
    m_image->setPixmap(
        m_current.scaled(m_image->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_image->setText(QString());
}

void ReferencePhotoDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    rescale();
}

} // namespace pl
