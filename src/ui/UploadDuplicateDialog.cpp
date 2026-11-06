#include "ui/UploadDuplicateDialog.h"

#include "app/Application.h"
#include "net/PhotoCache.h"
#include "thumb/ThumbnailCache.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace pl {

namespace {

constexpr int kIconPx = 128;
constexpr int kUrlRole = Qt::UserRole;

QListWidget *iconList(QWidget *parent)
{
    auto *list = new QListWidget(parent);
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(kIconPx, kIconPx));
    list->setGridSize(QSize(kIconPx + 16, kIconPx + 36));
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setWordWrap(true);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    return list;
}

constexpr int kIsInatRole = Qt::UserRole + 3;   // an iNaturalist photo (not one from the library)

// An iNaturalist photo's tile: the photo, else a word on where it's got to.
void showInatPhoto(QListWidgetItem *item, net::PhotoCache &cache)
{
    const QString url = item->data(kUrlRole).toString();
    const QPixmap pixmap = cache.photo(url, net::PhotoCache::Urgency::Now);
    if (!pixmap.isNull()) {
        item->setIcon(QIcon(pixmap));
        item->setText(QString());
        item->setToolTip(QString());
    } else if (cache.hasFailed(url)) {
        item->setText(QObject::tr("Couldn't download it — double-click to try again"));
        item->setToolTip(url);
    } else {
        item->setText(QObject::tr("Loading from iNaturalist…"));
    }
}

QString observationLink(qint64 id)
{
    return QStringLiteral("<a href=\"https://www.inaturalist.org/observations/%1\">#%1</a>").arg(id);
}

} // namespace

UploadDuplicateDialog::UploadDuplicateDialog(Application &app, const QList<inat::UploadPhoto> &photos,
                                             const QList<inat::DuplicateHit> &hits, QWidget *parent)
    : QDialog(parent), m_app(app), m_hits(hits)
{
    setWindowTitle(tr("Already on iNaturalist?"));
    resize(900, 600);

    auto *intro = new QLabel(hits.size() == 1
                                 ? tr("These photos look like an observation you already have on iNaturalist.")
                                 : tr("These photos look like observations you already have on iNaturalist. "
                                      "Pick the one that matches, if any."),
                             this);
    intro->setWordWrap(true);

    auto *yours = iconList(this);
    for (const inat::UploadPhoto &p : photos) {
        auto *item = new QListWidgetItem(p.label, yours);
        const QPixmap pixmap = m_app.thumbnails().thumbnail(p.contentHash, p.path, thumb::ThumbnailCache::kGridPx);
        if (!pixmap.isNull())
            item->setIcon(QIcon(pixmap));
        item->setData(kUrlRole, thumb::ThumbnailCache::keyFor(p.contentHash, p.path));
        item->setData(kUrlRole + 1, p.path);
        item->setData(kUrlRole + 2, p.contentHash);
    }

    m_hitList = new QListWidget(this);
    for (const inat::DuplicateHit &hit : hits) {
        const inat::ExistingObservation &e = hit.existing;
        const QString when = e.time.isValid() ? QLocale().toString(e.time, QLocale::ShortFormat)
                                              : QLocale().toString(e.date, QLocale::ShortFormat);
        new QListWidgetItem(QStringLiteral("#%1  %2\n%3 — %4")
                                .arg(e.id)
                                .arg(e.taxonName.isEmpty() ? tr("(not identified)") : e.taxonName, when,
                                     hit.reasons),
                            m_hitList);
    }
    m_hitList->setMaximumHeight(m_hitList->sizeHintForRow(0) * std::min<int>(int(hits.size()), 4)
                                + 2 * m_hitList->frameWidth() + 4);
    connect(m_hitList, &QListWidget::currentRowChanged, this, &UploadDuplicateDialog::showHit);

    m_details = new QLabel(this);
    m_details->setTextFormat(Qt::RichText);
    m_details->setOpenExternalLinks(true);
    m_details->setWordWrap(true);
    m_theirs = iconList(this);

    auto *left = new QVBoxLayout;
    left->addWidget(new QLabel(tr("<b>Your photos</b>"), this));
    left->addWidget(yours, 1);
    auto *right = new QVBoxLayout;
    right->addWidget(new QLabel(tr("<b>On iNaturalist</b>"), this));
    right->addWidget(m_hitList);
    right->addWidget(m_details);
    right->addWidget(m_theirs, 1);
    auto *sides = new QHBoxLayout;
    sides->addLayout(left, 1);
    sides->addLayout(right, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton *link = buttons->addButton(tr("Same Sighting — Don't Upload"), QDialogButtonBox::AcceptRole);
    link->setToolTip(tr("Nothing is sent. The photos are recorded as of this observation, so they aren't "
                        "offered for upload again."));
    m_addTo = buttons->addButton(tr("Add These Photos to It"), QDialogButtonBox::AcceptRole);
    m_addTo->setToolTip(tr("Sends these photos to that observation instead of making a new one."));
    QPushButton *asNew = buttons->addButton(tr("Upload as New"), QDialogButtonBox::AcceptRole);
    asNew->setToolTip(tr("It's a different sighting: upload it as a new observation."));
    link->setDefault(true);
    // The default button's text is bold (theme.qss), wider than its size hint allows for.
    QFont bold = link->font();
    bold.setWeight(QFont::DemiBold);
    link->setMinimumWidth(QFontMetrics(bold).horizontalAdvance(link->text()) + 40);
    connect(link, &QPushButton::clicked, this, [this] { m_choice = Link; accept(); });
    connect(m_addTo, &QPushButton::clicked, this, [this] { m_choice = AddTo; accept(); });
    connect(asNew, &QPushButton::clicked, this, [this] { m_choice = UploadNew; accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(sides, 1);
    layout->addWidget(buttons);

    connect(&m_app.photoCache(), &net::PhotoCache::ready, this, [this](const QString &url) {
        for (int i = 0; i < m_theirs->count(); ++i) {
            QListWidgetItem *item = m_theirs->item(i);
            if (item->data(kIsInatRole).toBool() && item->data(kUrlRole).toString() == url)
                showInatPhoto(item, m_app.photoCache());
        }
    });
    // One that didn't come: try it again.
    connect(m_theirs, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const QString url = item->data(kUrlRole).toString();
        if (!item->data(kIsInatRole).toBool() || !m_app.photoCache().hasFailed(url))
            return;
        m_app.photoCache().retry(url);
        showInatPhoto(item, m_app.photoCache());
    });
    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this, [this, yours](const QString &key, int px) {
        if (px != thumb::ThumbnailCache::kGridPx)
            return;
        for (QListWidget *list : {yours, m_theirs})
            for (int i = 0; i < list->count(); ++i) {
                QListWidgetItem *item = list->item(i);
                if (item->data(kUrlRole).toString() != key)
                    continue;
                const QPixmap pixmap = m_app.thumbnails().thumbnail(item->data(kUrlRole + 2).toString(),
                                                                    item->data(kUrlRole + 1).toString(), px);
                if (!pixmap.isNull())
                    item->setIcon(QIcon(pixmap));
            }
    });

    m_hitList->setCurrentRow(0);
}

qint64 UploadDuplicateDialog::observationId() const
{
    const int row = m_hitList->currentRow();
    return row >= 0 && row < m_hits.size() ? m_hits.at(row).existing.id : 0;
}

void UploadDuplicateDialog::showHit()
{
    const int row = m_hitList->currentRow();
    if (row < 0 || row >= m_hits.size())
        return;
    const inat::ExistingObservation &e = m_hits.at(row).existing;
    QStringList lines;
    lines << tr("Observation %1").arg(observationLink(e.id));
    if (!e.placeGuess.isEmpty())
        lines << e.placeGuess.toHtmlEscaped();
    if (e.fromLibrary && e.photoUrls.isEmpty())
        lines << tr("Known from your library: %1 is on it.").arg(e.libraryPhoto.toHtmlEscaped());
    else if (e.photoUrls.isEmpty())
        lines << tr("It has no photos.");
    m_details->setText(lines.join(QStringLiteral("<br>")));
    fillPhotos();
}

void UploadDuplicateDialog::fillPhotos()
{
    m_theirs->clear();
    const int row = m_hitList->currentRow();
    if (row < 0 || row >= m_hits.size())
        return;
    const inat::ExistingObservation &e = m_hits.at(row).existing;
    for (const QString &url : e.photoUrls) {
        auto *item = new QListWidgetItem(m_theirs);
        item->setData(kUrlRole, url);
        item->setData(kIsInatRole, true);
        showInatPhoto(item, m_app.photoCache());
    }
    if (e.photoUrls.isEmpty() && e.fromLibrary && !e.libraryPath.isEmpty()) {
        auto *item = new QListWidgetItem(e.libraryPhoto, m_theirs);
        item->setData(kUrlRole, thumb::ThumbnailCache::keyFor(e.libraryHash, e.libraryPath));
        item->setData(kUrlRole + 1, e.libraryPath);
        item->setData(kUrlRole + 2, e.libraryHash);
        const QPixmap pixmap = m_app.thumbnails().thumbnail(e.libraryHash, e.libraryPath, thumb::ThumbnailCache::kGridPx);
        if (!pixmap.isNull())
            item->setIcon(QIcon(pixmap));
    }
}

} // namespace pl
