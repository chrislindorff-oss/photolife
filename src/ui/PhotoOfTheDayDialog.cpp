#include "ui/PhotoOfTheDayDialog.h"

#include "app/Application.h"
#include "net/INatClient.h"
#include "raw/RawPreview.h"
#include "settings/Settings.h"
#include "thumb/ThumbnailCache.h"

#include <QCheckBox>
#include <QCursor>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace pl {

// The photo, scaled to fit whatever room there is, centred.
class PhotoArea : public QWidget
{
public:
    explicit PhotoArea(QWidget *parent) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);   // never asks for the image's size
    }
    void setImage(const QImage &image)
    {
        m_image = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_image.isNull())
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSize size = m_image.size().scaled(this->size(), Qt::KeepAspectRatio);
        p.drawImage(QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size), m_image);
    }

private:
    QImage m_image;
};

namespace {

const QColor kBackground(0x14, 0x16, 0x14);
const QColor kText(0xee, 0xee, 0xea);
const QColor kMuted(0xa8, 0xad, 0xa6);

// Scientific names are italic from genus down.
bool italic(const QString &rank)
{
    static const QSet<QString> ranks{
        QStringLiteral("genus"),      QStringLiteral("subgenus"),   QStringLiteral("section"),
        QStringLiteral("subsection"), QStringLiteral("complex"),    QStringLiteral("species"),
        QStringLiteral("hybrid"),     QStringLiteral("subspecies"), QStringLiteral("variety"),
        QStringLiteral("form"),       QStringLiteral("infrahybrid")};
    return ranks.contains(rank);
}

QString scientific(const QString &name, const QString &rank)
{
    return italic(rank) ? QStringLiteral("<i>%1</i>").arg(name.toHtmlEscaped()) : name.toHtmlEscaped();
}

QLabel *label(const QString &text, QWidget *parent, int pointSize = 0, bool bold = false, bool muted = false)
{
    auto *l = new QLabel(text, parent);
    QFont font = l->font();
    if (pointSize > 0)
        font.setPointSizeF(pointSize);
    font.setBold(bold);
    l->setFont(font);
    if (muted)
        l->setObjectName(QStringLiteral("muted"));
    l->setTextFormat(Qt::RichText);
    l->setWordWrap(true);
    l->setAlignment(Qt::AlignHCenter);
    return l;
}

} // namespace

PhotoOfTheDayDialog::PhotoOfTheDayDialog(Application &app, const catalogue::PhotoOfTheDay::Pick &pick,
                                         QWidget *parent)
    : QDialog(parent), m_app(app), m_pick(pick)
{
    setWindowTitle(tr("Nature Photo of the Day"));
    // Dark whatever the theme, like a gallery wall (a style sheet, as the
    // theme's own would override a palette).
    setObjectName(QStringLiteral("photoOfTheDay"));
    setStyleSheet(QStringLiteral("#photoOfTheDay { background: %1; }"
                                 "#photoOfTheDay QLabel, #photoOfTheDay QCheckBox { color: %2; background: transparent; }"
                                 "#photoOfTheDay QLabel#muted { color: %3; }")
                      .arg(kBackground.name(), kText.name(), kMuted.name()));

    m_photo = new PhotoArea(this);
    const QPixmap preview = m_app.thumbnails().thumbnail(pick.contentHash, pick.path, thumb::ThumbnailCache::kPreviewPx);
    if (!preview.isNull())
        m_photo->setImage(preview.toImage());
    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this, [this](const QString &key, int px) {
        if (m_full || px != thumb::ThumbnailCache::kPreviewPx
            || key != thumb::ThumbnailCache::keyFor(m_pick.contentHash, m_pick.path))
            return;
        const QPixmap p = m_app.thumbnails().thumbnail(m_pick.contentHash, m_pick.path, px);
        if (!p.isNull())
            m_photo->setImage(p.toImage());
    });

    auto *heading = label(tr("Nature Photo of the Day · %1").arg(QLocale().toString(QDate::currentDate(),
                                                                                       QLocale::LongFormat)),
                          this, 0, false, true);
    const QString common = pick.commonName.isEmpty() ? QString() : pick.commonName.toHtmlEscaped();
    auto *name = label(common.isEmpty() ? scientific(pick.name, pick.rank) : common, this, 22, true);
    QStringList second;
    if (!common.isEmpty())
        second << scientific(pick.name, pick.rank);
    if (!pick.rank.isEmpty())
        second << pick.rank;
    auto *nameDetail = label(second.join(QStringLiteral(" · ")), this, 13);
    nameDetail->setVisible(!second.isEmpty());

    m_lineageLabel = label(QString(), this, 0, false, true);
    m_lineageLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(m_lineageLabel, &QLabel::linkHovered, this, [this](const QString &link) {
        if (link.isEmpty()) {
            QToolTip::hideText();
            return;
        }
        for (const auto &step : std::as_const(m_lineage))
            if (QString::number(step.inatId) == link) {
                QString tip = step.rank.isEmpty() ? step.name : QStringLiteral("%1 (%2)").arg(step.name, step.rank);
                if (!step.commonName.isEmpty())
                    tip += QStringLiteral(" — ") + step.commonName;
                QToolTip::showText(QCursor::pos(), tip, m_lineageLabel);
            }
    });

    QStringList facts;
    const QDateTime taken = QDateTime::fromString(pick.capturedOn, Qt::ISODate);
    if (pick.capturedOn.size() > 10 && taken.isValid())
        facts << QStringLiteral("%1, %2").arg(QLocale().toString(taken.date(), QLocale::LongFormat),
                                              QLocale().toString(taken.time(), QLocale::ShortFormat));
    else if (const QDate d = QDate::fromString(pick.capturedOn.left(10), Qt::ISODate); d.isValid())
        facts << QLocale().toString(d, QLocale::LongFormat);
    if (pick.latitude && pick.longitude)
        facts << QStringLiteral("%1, %2").arg(*pick.latitude, 0, 'f', 4).arg(*pick.longitude, 0, 'f', 4);
    else if (!pick.folder.isEmpty())
        facts << QFileInfo(pick.folder).fileName().toHtmlEscaped();
    facts << QFileInfo(pick.path).fileName().toHtmlEscaped();
    if (pick.bestShot)
        facts << tr("★ Best Shot");
    auto *details = label(facts.join(QStringLiteral(" · ")), this, 0, false, true);

    auto *atStartup = new QCheckBox(tr("Show a Nature Photo of the Day when PhotoLife opens"), this);
    atStartup->setChecked(m_app.settings().photoOfDayAtStartup());
    atStartup->setToolTip(tr("You can turn it back on from View → Show Photo of the Day at Start-up."));
    connect(atStartup, &QCheckBox::toggled, this, [this](bool on) { m_app.settings().setPhotoOfDayAtStartup(on); });
    auto *library = new QPushButton(tr("Show in Library"), this);
    library->setAutoDefault(false);
    connect(library, &QPushButton::clicked, this, [this] {
        emit showInLibrary(m_pick.captureId);
        accept();
    });
    auto *close = new QPushButton(tr("Close"), this);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(atStartup);
    buttons->addStretch(1);
    buttons->addWidget(library);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->addWidget(m_photo, 1);
    layout->addSpacing(8);
    layout->addWidget(heading);
    layout->addWidget(name);
    layout->addWidget(nameDetail);
    layout->addWidget(m_lineageLabel);
    layout->addWidget(details);
    layout->addSpacing(6);
    layout->addLayout(buttons);

    // Nearly full screen, on the screen PhotoLife is on.
    QScreen *screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        resize(area.width() * 92 / 100, area.height() * 92 / 100);
        move(area.center() - rect().center());
    }

    m_lineage = catalogue::PhotoOfTheDay::lineage(m_app.database().connectionName(), pick.taxonInatId);
    showLineage();
    fetchMissingSteps();
    loadFullImage();
}

void PhotoOfTheDayDialog::loadFullImage()
{
    // Read at the screen's resolution (no bigger), off the GUI thread.
    QSize target(3840, 2160);
    if (QScreen *s = screen())
        target = s->size() * s->devicePixelRatio();
    const QString path = m_pick.path;
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher] {
        const QImage image = watcher->result();
        watcher->deleteLater();
        if (image.isNull())
            return;   // the preview stays
        m_full = true;
        m_photo->setImage(image);
    });
    watcher->setFuture(QtConcurrent::run([path, target] {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (size.isValid() && (size.width() > target.width() || size.height() > target.height()))
            reader.setScaledSize(size.scaled(target, Qt::KeepAspectRatio));
        QImage image = reader.read();
        if (image.isNull() && raw::isAvailable())
            image = raw::extractPreview(path, 0);   // a RAW: its embedded preview, full size
        return image;
    }));
}

void PhotoOfTheDayDialog::showLineage()
{
    QStringList steps;
    for (const auto &step : std::as_const(m_lineage)) {
        const QString text = step.known() ? scientific(step.name, step.rank) : QStringLiteral("…");
        steps << QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none\">%3</a>")
                     .arg(step.inatId)
                     .arg(kText.name(), text);
    }
    m_lineageLabel->setText(steps.join(QStringLiteral(" <span style=\"color:%1\">›</span> ").arg(kMuted.name())));
    m_lineageLabel->setVisible(!steps.isEmpty());
}

void PhotoOfTheDayDialog::fetchMissingSteps()
{
    QList<qint64> missing;
    for (const auto &step : std::as_const(m_lineage))
        if (!step.known())
            missing << step.inatId;
    if (missing.isEmpty())
        return;
    QPointer<PhotoOfTheDayDialog> self(this);
    m_app.inat().fetchTaxa(missing.mid(0, 30), [self](net::Outcome<QList<taxonomy::Taxon>> out) {
        if (!self || !out.ok())
            return;
        for (const taxonomy::Taxon &t : out.value)
            for (auto &step : self->m_lineage)
                if (step.inatId == t.inatId) {
                    step.name = t.name;
                    step.rank = t.rank;
                    step.commonName = t.commonName;
                }
        self->showLineage();
    });
}

void PhotoOfTheDayDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_F11) {
        isFullScreen() ? showNormal() : showFullScreen();
        return;
    }
    QDialog::keyPressEvent(event);
}

} // namespace pl
