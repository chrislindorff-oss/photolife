#include "ui/InatCheckIdsPage.h"

#include "app/Application.h"
#include "net/INatClient.h"
#include "settings/Settings.h"
#include "thumb/ThumbnailCache.h"
#include "ui/InatToolsView.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {
namespace {
enum Column { PhotoColumn, MineColumn, TheirsColumn, VerdictColumn, QualityColumn, IdsColumn, ColumnCount };
constexpr int kObservationRole = Qt::UserRole + 1;
constexpr int kIconPx = 56;

// "Longleaf Wattle (Acacia longifolia)", with the rank when it isn't a
// species: a genus, or a complex of the same name, is then told apart.
QString nameText(const QString &common, const QString &scientific, const QString &rank)
{
    if (scientific.isEmpty())
        return QStringLiteral("—");
    QString text = common.isEmpty() || common == scientific ? scientific : QStringLiteral("%1 (%2)").arg(common, scientific);
    if (!rank.isEmpty() && rank != QLatin1String("species"))
        text += QStringLiteral(" · ") + rank;
    return text;
}

QString qualityText(const QString &grade)
{
    if (grade == QLatin1String("research"))
        return QObject::tr("Research grade");
    if (grade == QLatin1String("needs_id"))
        return QObject::tr("Needs ID");
    if (grade == QLatin1String("casual"))
        return QObject::tr("Casual");
    return grade;
}
} // namespace

InatCheckIdsPage::InatCheckIdsPage(Application &app, InatAccountBar &account, QWidget *parent)
    : QWidget(parent), m_app(app), m_account(account)
{
    auto *intro = new QLabel(tr("Your photos that are on iNaturalist (uploaded from here, or downloaded from there), "
                                "set against what the community has made of them since. Nothing is changed on "
                                "iNaturalist."),
                             this);
    intro->setWordWrap(true);
    m_check = new QPushButton(tr("Check Now"), this);
    connect(m_check, &QPushButton::clicked, this, &InatCheckIdsPage::check);
    m_differencesOnly = new QCheckBox(tr("Only where iNaturalist differs"), this);
    m_differencesOnly->setChecked(true);
    connect(m_differencesOnly, &QCheckBox::toggled, this, &InatCheckIdsPage::fill);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    auto *top = new QHBoxLayout;
    top->addWidget(m_check);
    top->addWidget(m_differencesOnly);
    top->addWidget(m_status, 1);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels({tr("Photo"), tr("Your name"), tr("iNaturalist's name"), tr(""), tr("Quality"),
                             tr("IDs")});
    m_tree->setRootIsDecorated(false);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setIconSize(QSize(kIconPx, kIconPx));
    m_tree->setSortingEnabled(true);
    m_tree->header()->setSectionResizeMode(MineColumn, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(TheirsColumn, QHeaderView::Stretch);
    m_tree->header()->resizeSection(PhotoColumn, kIconPx + 24);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        bool usable = false;
        for (QTreeWidgetItem *item : selectedRows()) {
            const auto verdict = inat::IdVerdict(item->data(VerdictColumn, Qt::UserRole).toInt());
            usable = usable || (verdict != inat::IdVerdict::Same && verdict != inat::IdVerdict::NoName);
        }
        m_use->setEnabled(usable);
        m_keep->setEnabled(usable);
        m_open->setEnabled(!selectedRows().isEmpty());
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this] { openSelected(); });

    m_use = new QPushButton(tr("Use iNaturalist's Name"), this);
    m_use->setToolTip(tr("Give the photos here iNaturalist's name for them, as Match with iNat Observation does."));
    connect(m_use, &QPushButton::clicked, this, &InatCheckIdsPage::useSelected);
    m_keep = new QPushButton(tr("Keep Mine"), this);
    m_keep->setToolTip(tr("Leave your name, and don't show this again unless iNaturalist's name changes."));
    connect(m_keep, &QPushButton::clicked, this, &InatCheckIdsPage::keepSelected);
    m_open = new QPushButton(tr("Open on iNaturalist"), this);
    connect(m_open, &QPushButton::clicked, this, &InatCheckIdsPage::openSelected);
    for (QPushButton *b : {m_use, m_keep, m_open})
        b->setEnabled(false);
    auto *actions = new QHBoxLayout;
    actions->addWidget(m_use);
    actions->addWidget(m_keep);
    actions->addStretch(1);
    actions->addWidget(m_open);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(top);
    layout->addWidget(m_tree, 1);
    layout->addLayout(actions);

    connect(&m_app.thumbnails(), &thumb::ThumbnailCache::ready, this, [this](const QString &key, int px) {
        if (px != thumb::ThumbnailCache::kGridPx)
            return;
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_tree->topLevelItem(i);
            if (item->data(PhotoColumn, Qt::UserRole).toString() == key)
                item->setIcon(PhotoColumn, QIcon(m_app.thumbnails().thumbnail(key, item->data(PhotoColumn, Qt::UserRole + 1).toString(),
                                                                              thumb::ThumbnailCache::kGridPx)));
        }
    });
    refreshLocal();
}

QString InatCheckIdsPage::keptKey(qint64 observationId, qint64 taxonId)
{
    return QStringLiteral("%1:%2").arg(observationId).arg(taxonId);
}

void InatCheckIdsPage::refreshLocal()
{
    m_linked = inat::linkedObservations(m_app.database().connectionName());
    if (m_theirs.isEmpty())
        m_status->setText(m_linked.isEmpty()
                              ? tr("None of your photos here are linked to iNaturalist observations yet.")
                              : tr("%n observation(s) of yours have photos here. Check Now asks iNaturalist about them.",
                                   nullptr, int(m_linked.size())));
    m_check->setEnabled(!m_linked.isEmpty() && !m_checking);
    fill();
}

void InatCheckIdsPage::check()
{
    m_account.commit();
    refreshLocal();
    m_toFetch.clear();
    for (const inat::LinkedObservation &l : std::as_const(m_linked))
        m_toFetch << l.observationId;
    if (m_toFetch.isEmpty())
        return;
    m_theirs.clear();
    ++m_generation;
    m_checking = true;
    m_check->setEnabled(false);
    fetchNext();
}

void InatCheckIdsPage::fetchNext()
{
    if (m_toFetch.isEmpty()) {
        m_checking = false;
        m_check->setEnabled(true);
        fill();
        return;
    }
    m_status->setText(tr("Asking iNaturalist… %1 of %2").arg(m_theirs.size()).arg(m_theirs.size() + m_toFetch.size()));
    const QList<qint64> batch = m_toFetch.mid(0, net::INatClient::kMaxObservationsPerCall);
    m_toFetch.remove(0, batch.size());
    const int generation = m_generation;
    QPointer<InatCheckIdsPage> self(this);
    m_app.inat().fetchObservations(batch, [self, generation, batch](net::Outcome<QList<taxonomy::Observation>> out) {
        if (!self || generation != self->m_generation)
            return;
        if (!out.ok()) {
            self->m_checking = false;
            self->m_check->setEnabled(true);
            self->m_status->setText(tr("iNaturalist couldn't be asked: %1").arg(out.error));
            return;
        }
        for (const taxonomy::Observation &o : std::as_const(out.value))
            self->m_theirs.insert(o.id, o);
        self->fetchNext();
    });
}

void InatCheckIdsPage::fill()
{
    m_tree->setSortingEnabled(false);
    m_tree->clear();
    const QStringList kept = m_app.settings().inatKeptIdentifications();
    const ThemeColors &t = themeColors(currentThemeVariant());
    int differ = 0, gone = 0, hiddenKept = 0;
    for (const inat::LinkedObservation &l : std::as_const(m_linked)) {
        const auto it = m_theirs.constFind(l.observationId);
        if (it == m_theirs.cend()) {
            if (!m_theirs.isEmpty())
                ++gone;   // deleted on iNaturalist, or not visible
            continue;
        }
        const taxonomy::Observation &theirs = *it;
        const inat::IdVerdict verdict = inat::compareIds(l, theirs);
        const bool differs = verdict != inat::IdVerdict::Same && verdict != inat::IdVerdict::NoName;
        differ += differs ? 1 : 0;
        if (differs && kept.contains(keptKey(l.observationId, theirs.taxonInatId))) {
            ++hiddenKept;
            continue;
        }
        if (m_differencesOnly->isChecked() && !differs)
            continue;
        auto *item = new QTreeWidgetItem(m_tree);
        item->setData(PhotoColumn, Qt::UserRole, l.previewHash);
        item->setData(PhotoColumn, Qt::UserRole + 1, l.previewPath);
        item->setIcon(PhotoColumn, QIcon(m_app.thumbnails().thumbnail(l.previewHash, l.previewPath,
                                                                        thumb::ThumbnailCache::kGridPx)));
        item->setData(PhotoColumn, kObservationRole, l.observationId);
        item->setText(MineColumn, nameText(l.mineCommonName, l.mineName, l.mineRank));
        item->setText(TheirsColumn, nameText(theirs.taxonCommonName, theirs.taxonName, theirs.taxonRank));
        item->setText(VerdictColumn, inat::verdictText(verdict));
        item->setData(VerdictColumn, Qt::UserRole, int(verdict));
        item->setText(QualityColumn, qualityText(theirs.qualityGrade));
        item->setText(IdsColumn, theirs.identificationsCount > 0
                                     ? tr("%1 of %2 agree").arg(theirs.agreements).arg(theirs.identificationsCount)
                                     : QString());
        item->setToolTip(MineColumn, tr("%n photo(s) here", nullptr, int(l.captureIds.size())));
        if (differs)
            item->setForeground(VerdictColumn, verdict == inat::IdVerdict::Different ? t.statusUnmatched : t.primaryAccent);
        item->setSizeHint(PhotoColumn, QSize(kIconPx + 8, kIconPx + 8));
    }
    m_tree->setSortingEnabled(true);
    m_tree->resizeColumnToContents(VerdictColumn);
    if (!m_checking && !m_theirs.isEmpty()) {
        QString text = differ == 0 ? tr("iNaturalist agrees with all %n of them.", nullptr, int(m_theirs.size()))
                                   : tr("iNaturalist's name differs for %1 of %n observation(s).", nullptr,
                                        int(m_theirs.size()))
                                         .arg(differ);
        if (hiddenKept > 0)
            text += QLatin1Char(' ') + tr("%n you've kept are hidden.", nullptr, hiddenKept);
        if (gone > 0)
            text += QLatin1Char(' ') + tr("%n couldn't be found (deleted, or private).", nullptr, gone);
        m_status->setText(text);
    }
}

QList<QTreeWidgetItem *> InatCheckIdsPage::selectedRows() const
{
    return m_tree->selectedItems();
}

void InatCheckIdsPage::useSelected()
{
    for (QTreeWidgetItem *item : selectedRows()) {
        const qint64 obs = item->data(PhotoColumn, kObservationRole).toLongLong();
        const auto verdict = inat::IdVerdict(item->data(VerdictColumn, Qt::UserRole).toInt());
        if (verdict == inat::IdVerdict::Same || verdict == inat::IdVerdict::NoName)
            continue;
        for (const inat::LinkedObservation &l : std::as_const(m_linked)) {
            if (l.observationId == obs && m_theirs.contains(obs))
                emit useInaturalistName(l.captureIds, m_theirs.value(obs));
        }
    }
}

void InatCheckIdsPage::keepSelected()
{
    QStringList kept = m_app.settings().inatKeptIdentifications();
    for (QTreeWidgetItem *item : selectedRows()) {
        const qint64 obs = item->data(PhotoColumn, kObservationRole).toLongLong();
        if (m_theirs.contains(obs))
            kept << keptKey(obs, m_theirs.value(obs).taxonInatId);
    }
    kept.removeDuplicates();
    m_app.settings().setInatKeptIdentifications(kept);
    fill();
}

void InatCheckIdsPage::openSelected()
{
    const QList<QTreeWidgetItem *> rows = selectedRows();
    for (QTreeWidgetItem *item : rows.mid(0, 10))
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.inaturalist.org/observations/%1")
                                           .arg(item->data(PhotoColumn, kObservationRole).toLongLong())));
}

} // namespace pl
