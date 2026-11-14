#include "ui/InatPresencePage.h"

#include "app/Application.h"
#include "db/Database.h"
#include "inat/InatPresence.h"
#include "inat/PresenceCheck.h"
#include "net/INatClient.h"
#include "net/PhotoCache.h"
#include "settings/Settings.h"
#include "thumb/ThumbnailCache.h"
#include "ui/FlowLayout.h"
#include "ui/InatToolsView.h"
#include "ui/InatTokenGetter.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {
namespace {

enum Column { PhotoColumn, TakenColumn, WhyColumn, ColumnCount };
enum Role { CaptureRole = Qt::UserRole, ObservationRole, HashRole, PathRole, PreviewUrlRole };
constexpr int kIconPx = 72;

QString confidenceTitle(inat::PresenceConfidence c)
{
    switch (c) {
    case inat::PresenceConfidence::High:
        return InatPresencePage::tr("High — marked on iNaturalist");
    case inat::PresenceConfidence::Medium:
        return InatPresencePage::tr("Medium — probably on iNaturalist");
    case inat::PresenceConfidence::Low:
        return InatPresencePage::tr("Low — possibly on iNaturalist");
    case inat::PresenceConfidence::None:
        break;
    }
    return InatPresencePage::tr("Marked by hand");
}

} // namespace

InatPresencePage::InatPresencePage(Application &app, InatAccountBar &account, QWidget *parent)
    : QWidget(parent), m_app(app), m_account(account)
{
    auto *intro = new QLabel(
        tr("Finds your photos that are already on iNaturalist — posted from the iNaturalist app or website, or before "
           "PhotoLife — so they leave the To upload list. PhotoLife reads the details of your observations (species, "
           "date, time and place, not their photos) and compares your named photos with them. Close matches "
           "(same species, within an hour and 200 m) are marked on iNaturalist; the others are listed below for "
           "you to say yes or no. Nothing is changed on iNaturalist."),
        this);
    intro->setWordWrap(true);

    m_check = new inat::PresenceCheck(m_app.inat(), m_app.database().connectionName(), this);
    connect(m_check, &inat::PresenceCheck::progress, this,
            [this](const QString &stage, int done, int) {
                m_status->setText(done > 0 ? tr("%1 %2 read.").arg(stage).arg(done) : stage);
            });
    connect(m_check, &inat::PresenceCheck::finished, this, [this](const inat::PresenceCheck::Result &r) {
        m_checkButton->setText(tr("Check Now"));
        if (r.cancelled) {
            m_status->setText(tr("Stopped. The observations read so far are kept; Check Now carries on."));
        } else if (!r.ok) {
            m_status->setText(r.status == 401 ? tr("iNaturalist didn't accept your token (it lasts a day). Get a "
                                                   "fresh one with Get Token…, or clear it to check without one.")
                                              : tr("Couldn't check: %1").arg(r.error));
        } else {
            m_status->setText(tr("%1 observations of yours known (%2 new). Of %3 photos checked: %4 marked on "
                                 "iNaturalist, %5 to check (%6 medium, %7 low).")
                                  .arg(r.observationsKnown)
                                  .arg(r.observationsRead)
                                  .arg(r.photosChecked)
                                  .arg(r.marked)
                                  .arg(r.medium + r.low)
                                  .arg(r.medium)
                                  .arg(r.low));
            if (r.marked > 0)
                emit changed();
        }
        reload();
    });

    m_checkButton = new QPushButton(tr("Check Now"), this);
    connect(m_checkButton, &QPushButton::clicked, this, [this] {
        if (m_check->isRunning())
            m_check->cancel();
        else
            check();
    });
    m_rereadAll = new QCheckBox(tr("Read all my observations again"), this);
    m_rereadAll->setToolTip(tr("Normally only observations added since the last check are read. Tick this after "
                               "changing or deleting observations on iNaturalist."));
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    auto *top = new FlowLayout;
    top->addWidget(m_checkButton);
    top->addWidget(m_rereadAll);
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    top->addWidget(m_status);

    // Which decisions: the ones waiting, or the ones the check made.
    auto makeChip = [this](const QString &text, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setProperty("chip", true);
        connect(b, &QToolButton::clicked, this, &InatPresencePage::fill);
        return b;
    };
    m_toCheck = makeChip(tr("To check"), tr("Matches you haven't decided on yet, most likely first."));
    m_autoMarked = makeChip(tr("Marked automatically"),
                            tr("Photos the check marked on iNaturalist itself (High). Say No to any that aren't."));
    m_toCheck->setChecked(true);
    auto *chips = new QButtonGroup(this);
    chips->addButton(m_toCheck);
    chips->addButton(m_autoMarked);
    auto *chipRow = new QHBoxLayout;
    chipRow->addWidget(m_toCheck);
    chipRow->addWidget(m_autoMarked);
    chipRow->addStretch(1);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels({tr("Your photo / the observation"), tr("Taken / seen"), tr("Why")});
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setIconSize(QSize(kIconPx, kIconPx));
    m_tree->setUniformRowHeights(false);
    m_tree->header()->setSectionResizeMode(PhotoColumn, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(WhyColumn, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(TakenColumn, QHeaderView::ResizeToContents);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &InatPresencePage::updateButtons);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this] { openSelected(); });

    m_yes = new QPushButton(tr("Yes, It's on iNaturalist"), this);
    m_yes->setToolTip(tr("Mark the selected photos (or every photo under a selected heading) as on iNaturalist, "
                         "linked to the observation: they leave To upload."));
    connect(m_yes, &QPushButton::clicked, this, &InatPresencePage::yes);
    m_no = new QPushButton(tr("No, Not on iNaturalist"), this);
    m_no->setToolTip(tr("Not that observation: remembered, so later checks don't suggest it again. The photos stay "
                        "in To upload."));
    connect(m_no, &QPushButton::clicked, this, &InatPresencePage::no);
    m_open = new QPushButton(tr("Open on iNaturalist"), this);
    connect(m_open, &QPushButton::clicked, this, &InatPresencePage::openSelected);
    m_undo = new QPushButton(tr("Undo Last Check"), this);
    m_undo->setToolTip(tr("Forget everything the last check decided (photos you've said yes or no to since keep "
                          "your decision)."));
    connect(m_undo, &QPushButton::clicked, this, &InatPresencePage::undoLastCheck);
    auto *actions = new FlowLayout;
    actions->addWidget(m_yes);
    actions->addWidget(m_no);
    actions->addWidget(m_open);
    actions->addStretch();
    actions->addWidget(m_undo);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(top);
    layout->addLayout(chipRow);
    layout->addWidget(m_tree, 1);
    layout->addLayout(actions);

    // Thumbnails and observation previews as they arrive.
    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this, [this](const QString &key, int px) {
        if (px != thumb::ThumbnailCache::kGridPx)
            return;
        for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
            if ((*it)->data(PhotoColumn, HashRole).toString() == key)
                (*it)->setIcon(PhotoColumn, QIcon(m_app.thumbnails().thumbnail(
                                                key, (*it)->data(PhotoColumn, PathRole).toString(), px)));
    });
    connect(&m_app.photoCache(), &net::PhotoCache::ready, this, [this](const QString &url) {
        for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
            if ((*it)->data(PhotoColumn, PreviewUrlRole).toString() == url) {
                const QPixmap pm = m_app.photoCache().photo(url);
                if (!pm.isNull())
                    (*it)->setIcon(PhotoColumn, QIcon(pm));
            }
    });
    reload();
}

bool InatPresencePage::canEdit()
{
    return !m_canEdit || m_canEdit();
}

void InatPresencePage::check()
{
    if (!canEdit())
        return;
    m_account.commit();
    const QString login = m_app.settings().inatUsername().trimmed();
    if (login.isEmpty()) {
        m_status->setText(tr("Enter your iNaturalist username above first."));
        m_account.focusUsername();
        return;
    }
    // With the token, observations whose location you've hidden come with
    // their true place; without it, only their rough one.
    const QString token = m_app.settings().inatApiToken();
    m_app.inat().setAccessToken(InatTokenGetter::isUsable(token) ? token : QString());
    m_checkButton->setText(tr("Stop"));
    m_status->setText(tr("Reading your observations from iNaturalist…"));
    m_check->start(login, m_rereadAll->isChecked());
    m_rereadAll->setChecked(false);
}

void InatPresencePage::reload()
{
    const QString conn = m_app.database().connectionName();
    const int toCheck = int(inat::PresenceStore::items(conn, QStringLiteral("maybe")).size());
    const int autoMarked = int(inat::PresenceStore::items(conn, QStringLiteral("auto")).size());
    m_toCheck->setText(tr("To check (%1)").arg(toCheck));
    m_autoMarked->setText(tr("Marked automatically (%1)").arg(autoMarked));
    fill();
}

void InatPresencePage::fill()
{
    m_tree->clear();
    const bool automatic = m_autoMarked->isChecked();
    const QList<inat::PresenceStore::Item> items =
        inat::PresenceStore::items(m_app.database().connectionName(),
                                   automatic ? QStringLiteral("auto") : QStringLiteral("maybe"));
    // By confidence, then by observation, then the photos.
    QTreeWidgetItem *group = nullptr;
    QTreeWidgetItem *observation = nullptr;
    inat::PresenceConfidence groupKey = inat::PresenceConfidence::None;
    qint64 observationKey = -1;
    int groupCount = 0;
    const auto closeGroup = [&] {
        if (group)
            group->setText(PhotoColumn, QStringLiteral("%1  (%2)").arg(confidenceTitle(groupKey)).arg(groupCount));
    };
    for (const inat::PresenceStore::Item &it : items) {
        if (!group || it.confidence != groupKey) {
            closeGroup();
            group = new QTreeWidgetItem(m_tree);
            QFont bold = group->font(PhotoColumn);
            bold.setBold(true);
            group->setFont(PhotoColumn, bold);
            group->setFirstColumnSpanned(true);
            groupKey = it.confidence;
            groupCount = 0;
            observationKey = -1;
        }
        if (it.observationId != observationKey) {
            observation = new QTreeWidgetItem(group);
            observation->setText(PhotoColumn, it.observationTaxon.isEmpty()
                                                  ? tr("Observation %1").arg(it.observationId)
                                                  : tr("%1 — observation %2").arg(it.observationTaxon).arg(it.observationId));
            observation->setText(TakenColumn, QString(it.observationOn).replace(QLatin1Char('T'), QLatin1Char(' ')));
            observation->setText(WhyColumn, it.observationPlace);
            observation->setData(PhotoColumn, ObservationRole, it.observationId);
            observation->setData(PhotoColumn, PreviewUrlRole, it.observationPreviewUrl);
            observation->setToolTip(PhotoColumn, tr("On iNaturalist. Double-click to open it."));
            if (!it.observationPreviewUrl.isEmpty()) {
                const QPixmap pm = m_app.photoCache().photo(it.observationPreviewUrl);
                if (!pm.isNull())
                    observation->setIcon(PhotoColumn, QIcon(pm));
            }
            observationKey = it.observationId;
        }
        auto *photo = new QTreeWidgetItem(observation);
        photo->setText(PhotoColumn, it.photoName);
        photo->setText(TakenColumn, it.photoTaken.isEmpty() ? tr("no date")
                                                            : QString(it.photoTaken).replace(QLatin1Char('T'), QLatin1Char(' ')));
        photo->setText(WhyColumn, it.reasons);
        photo->setToolTip(WhyColumn, it.reasons);
        photo->setData(PhotoColumn, CaptureRole, it.captureId);
        photo->setData(PhotoColumn, ObservationRole, it.observationId);
        photo->setData(PhotoColumn, HashRole, it.photoHash);
        photo->setData(PhotoColumn, PathRole, it.photoPath);
        if (!it.photoPath.isEmpty()) {
            const QPixmap pm = m_app.thumbnails().thumbnail(it.photoHash, it.photoPath, thumb::ThumbnailCache::kGridPx);
            if (!pm.isNull())
                photo->setIcon(PhotoColumn, QIcon(pm));
        }
        ++groupCount;
    }
    closeGroup();
    m_tree->expandAll();
    if (items.isEmpty()) {
        auto *none = new QTreeWidgetItem(m_tree);
        none->setText(PhotoColumn, automatic ? tr("None yet: Check Now marks the close matches it finds.")
                                             : tr("Nothing to check. Check Now looks for your photos on iNaturalist."));
        none->setFlags(Qt::ItemIsEnabled);
        none->setFirstColumnSpanned(true);
    }
    m_undo->setEnabled(!inat::PresenceStore::lastRun(m_app.database().connectionName()).isEmpty());
    updateButtons();
}

QList<QTreeWidgetItem *> InatPresencePage::selectedPhotoRows() const
{
    // A heading stands for every photo under it.
    QList<QTreeWidgetItem *> rows;
    QSet<QTreeWidgetItem *> seen;
    std::function<void(QTreeWidgetItem *)> add = [&](QTreeWidgetItem *item) {
        if (item->data(PhotoColumn, CaptureRole).toInt() > 0) {
            if (!seen.contains(item)) {
                seen.insert(item);
                rows << item;
            }
            return;
        }
        for (int i = 0; i < item->childCount(); ++i)
            add(item->child(i));
    };
    for (QTreeWidgetItem *item : m_tree->selectedItems())
        add(item);
    return rows;
}

QList<int> InatPresencePage::selectedCaptures() const
{
    QList<int> ids;
    for (QTreeWidgetItem *row : selectedPhotoRows())
        ids << row->data(PhotoColumn, CaptureRole).toInt();
    return ids;
}

void InatPresencePage::updateButtons()
{
    const bool automatic = m_autoMarked->isChecked();
    const bool any = !selectedPhotoRows().isEmpty();
    m_yes->setVisible(!automatic);
    m_yes->setEnabled(any);
    m_no->setEnabled(any);
    bool observation = false;
    for (QTreeWidgetItem *item : m_tree->selectedItems())
        observation = observation || item->data(PhotoColumn, ObservationRole).toLongLong() > 0;
    m_open->setEnabled(observation);
}

void InatPresencePage::yes()
{
    if (!canEdit())
        return;
    // Each photo linked to its own observation.
    QHash<qint64, QList<int>> byObservation;
    for (QTreeWidgetItem *row : selectedPhotoRows())
        byObservation[row->data(PhotoColumn, ObservationRole).toLongLong()] << row->data(PhotoColumn, CaptureRole).toInt();
    const QString conn = m_app.database().connectionName();
    for (auto it = byObservation.cbegin(); it != byObservation.cend(); ++it) {
        // Kept as the check judged it, now decided by you.
        QString reasons;
        inat::PresenceConfidence confidence = inat::PresenceConfidence::None;
        for (const inat::PresenceStore::Item &i : inat::PresenceStore::items(conn, QStringLiteral("maybe")))
            if (it.value().contains(i.captureId)) {
                reasons = i.reasons;
                confidence = i.confidence;
                break;
            }
        inat::PresenceStore::markOnInat(conn, it.value(), it.key(), confidence, reasons, QStringLiteral("user"));
    }
    reload();
    emit changed();
}

void InatPresencePage::no()
{
    if (!canEdit())
        return;
    const QList<int> ids = selectedCaptures();
    if (ids.isEmpty())
        return;
    inat::PresenceStore::markNotOnInat(m_app.database().connectionName(), ids);
    reload();
    emit changed();
}

void InatPresencePage::openSelected()
{
    QSet<qint64> opened;
    for (QTreeWidgetItem *item : m_tree->selectedItems()) {
        const qint64 id = item->data(PhotoColumn, ObservationRole).toLongLong();
        if (id > 0 && !opened.contains(id) && opened.size() < 5) {
            opened.insert(id);
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.inaturalist.org/observations/%1").arg(id)));
        }
    }
}

void InatPresencePage::undoLastCheck()
{
    if (!canEdit())
        return;
    const QString conn = m_app.database().connectionName();
    const QString run = inat::PresenceStore::lastRun(conn);
    if (run.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Undo Last Check"),
                              tr("Forget what the last check decided: the photos it marked on iNaturalist go back to "
                                 "To upload, and its matches to check are removed. Photos you've decided on since "
                                 "keep your decision."))
        != QMessageBox::Yes)
        return;
    const int undone = inat::PresenceStore::undoRun(conn, run);
    m_status->setText(tr("Undone: %n photo(s).", nullptr, std::max(0, undone)));
    reload();
    emit changed();
}

} // namespace pl
