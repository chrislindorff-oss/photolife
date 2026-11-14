#include "ui/ReviewPane.h"

#include "db/Database.h"
#include "model/ReviewQueueFilterProxy.h"
#include "model/ReviewQueueGroupModel.h"
#include "model/ReviewQueueModel.h"
#include "net/INatClient.h"
#include "net/PhotoCache.h"
#include "taxonomy/TaxonomyStore.h"
#include "taxonomy/TaxonomyTypes.h"
#include "thumb/ThumbnailCache.h"
#include "ui/IdentifyPanel.h"
#include "ui/ImageViewer.h"
#include "ui/Theme.h"

#include <QFileInfo>

#include <algorithm>
#include <QContextMenuEvent>
#include <QMenu>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDesktopServices>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QSplitter>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {
namespace {
using model::ReviewQueueFilterProxy;
using model::ReviewQueueGroupModel;
using model::ReviewQueueModel;
constexpr int kQueueThumb = 128;   // longest edge of each queue-list thumbnail
constexpr int kMinPhoto = 180;     // each comparison photo's minimum box
constexpr int kMaxPhoto = 420;     // and its maximum, so the candidate list keeps room

void boldize(QLabel *label)
{
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
}
} // namespace

ReviewPane::ReviewPane(pl::Database &db, pl::thumb::ThumbnailCache &thumbs,
                       pl::net::INatClient &inat, pl::net::PhotoCache &photos, QWidget *parent)
    : QWidget(parent)
    , m_db(db)
    , m_thumbs(thumbs)
    , m_inat(inat)
    , m_photos(photos)
    , m_queue(new ReviewQueueModel(db, thumbs, this))
    , m_queueProxy(new ReviewQueueFilterProxy(this))
    , m_queueGroup(new ReviewQueueGroupModel(this))
    , m_reviewer(db.connectionName())
    , m_finder(db.connectionName())
{
    m_queueProxy->setSourceModel(m_queue);
    m_queueProxy->setFilterRole(ReviewQueueModel::SearchTextRole);
    m_queueProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_queueProxy->setDynamicSortFilter(true);
    m_queueGroup->setSourceModel(m_queueProxy);

    m_photoFilter = new QLineEdit(this);
    m_photoFilter->setPlaceholderText(tr("Filter photos by name, folder or guess…"));
    m_photoFilter->setClearButtonEnabled(true);
    connect(m_photoFilter, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_queueProxy->setFilterFixedString(text);
        const QModelIndex cur = m_queueView->currentIndex();
        if ((!cur.isValid() || m_queueGroup->isGroup(cur)) && visibleQueueCount() > 0)
            selectCapture(visibleQueueIndex(0));
        showCurrent();
    });

    auto makeBucketButton = [this](int bucket) {
        auto *btn = new QToolButton(this);
        btn->setCheckable(true);
        btn->setProperty("chip", true);   // the list on show is marked (theme.qss)
        connect(btn, &QToolButton::clicked, this, [this, bucket] { setBucket(bucket); });
        return btn;
    };
    m_bucketConfirmButton = makeBucketButton(int(ReviewQueueFilterProxy::Bucket::NeedsReview));
    m_bucketIdentifyButton = makeBucketButton(int(ReviewQueueFilterProxy::Bucket::NoCandidate));
    m_bucketConfirmButton->setToolTip(
        tr("Photos the engine has a guess for, waiting for a yes or no. They're grouped by the "
           "guess: click a group's heading to select it all, then Confirm."));
    m_bucketIdentifyButton->setToolTip(
        tr("Photos with no guess at all, usually named only by the camera. They're grouped by "
           "folder and day: name them here, with Identify, or open the folder in Organise."));
    m_bucketConfirmButton->setChecked(true);
    m_bucketGroup = new QButtonGroup(this);
    m_bucketGroup->setExclusive(true);
    m_bucketGroup->addButton(m_bucketConfirmButton);
    m_bucketGroup->addButton(m_bucketIdentifyButton);
    auto *bucketButtons = new QHBoxLayout;
    bucketButtons->setContentsMargins(0, 0, 0, 0);
    bucketButtons->addWidget(m_bucketConfirmButton);
    bucketButtons->addWidget(m_bucketIdentifyButton);
    bucketButtons->addStretch(1);

    auto *collapseBtn = new QToolButton(this);
    collapseBtn->setText(tr("Collapse all"));
    collapseBtn->setToolTip(tr("Collapse every group"));
    connect(collapseBtn, &QToolButton::clicked, this, &ReviewPane::collapseAllGroups);
    auto *expandBtn = new QToolButton(this);
    expandBtn->setText(tr("Expand all"));
    expandBtn->setToolTip(tr("Expand every group"));
    connect(expandBtn, &QToolButton::clicked, this, &ReviewPane::expandAllGroups);
    auto *groupButtons = new QHBoxLayout;
    groupButtons->setContentsMargins(0, 0, 0, 0);
    groupButtons->addWidget(collapseBtn);
    groupButtons->addWidget(expandBtn);
    groupButtons->addStretch(1);

    m_queueView = new QTreeView(this);
    m_queueView->setModel(m_queueGroup);
    m_queueView->setHeaderHidden(true);
    m_queueView->setRootIsDecorated(true);
    m_queueView->setExpandsOnDoubleClick(true);
    m_queueView->setUniformRowHeights(false);   // compact name-only headers, tall photo rows
    m_queueView->setIconSize(QSize(kQueueThumb, kQueueThumb));
    m_queueView->setMinimumWidth(320);
    // Several photos can be picked (Ctrl/Shift-click) for matching with one
    // iNaturalist observation; the form shows the current one.
    m_queueView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(m_queueView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this] { showCurrent(); });
    connect(m_queueView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateDecisionButtons(); });
    // A group's heading selects its photos, so one decision covers them all.
    connect(m_queueView, &QAbstractItemView::clicked, this, [this](const QModelIndex &idx) {
        if (m_queueGroup->isGroup(idx))
            selectGroup(idx);
    });
    connect(m_queueView, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &idx) {
                if (!m_queueGroup->isGroup(idx))
                    openViewerAt(idx);
            });
    connect(m_queueView, &QTreeView::collapsed, this, [this](const QModelIndex &idx) {
        if (m_queueGroup->isGroup(idx))
            m_collapsedGroups.insert(m_queueGroup->groupName(idx));
    });
    connect(m_queueView, &QTreeView::expanded, this, [this](const QModelIndex &idx) {
        if (m_queueGroup->isGroup(idx))
            m_collapsedGroups.remove(m_queueGroup->groupName(idx));
    });
    connect(m_queueGroup, &QAbstractItemModel::modelReset, this,
            &ReviewPane::restoreGroupExpansion);
    m_queueView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_queueView, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex idx = m_queueView->indexAt(pos);
        if (!idx.isValid())
            return;
        if (m_queueGroup->isGroup(idx)) {
            groupMenu(idx, m_queueView->viewport()->mapToGlobal(pos));
            return;
        }
        // Right-click doesn't extend a selection: on a photo outside it, act on that alone.
        if (!m_queueView->selectionModel()->isSelected(idx))
            m_queueView->setCurrentIndex(idx);
        photoMenu(idx, m_queueView->viewport()->mapToGlobal(pos));
    });

    auto makePhoto = [this] {
        auto *lbl = new QLabel(this);
        lbl->setMinimumSize(kMinPhoto, kMinPhoto);
        lbl->setMaximumHeight(kMaxPhoto);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setFrameShape(QFrame::StyledPanel);
        lbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        lbl->installEventFilter(this);
        return lbl;
    };

    m_yourPhotoTitle = new QLabel(tr("Your photo"), this);
    boldize(m_yourPhotoTitle);
    m_refPhotoTitle = new QLabel(tr("iNaturalist reference"), this);
    boldize(m_refPhotoTitle);

    m_thumb = makePhoto();

    m_refThumb = makePhoto();
    m_refThumb->setToolTip(tr("Reference photo of the selected candidate taxon, "
                              "from iNaturalist. Double-click to open the taxon."));
    m_refThumb->setText(tr("(no taxon selected)"));

    m_refCaption = new QLabel(this);
    m_refCaption->setWordWrap(true);
    m_refCaption->setStyleSheet(
        QStringLiteral("color: %1;").arg(pl::themeColors(pl::currentThemeVariant()).mutedText.name()));

    m_refDebounce = new QTimer(this);
    m_refDebounce->setSingleShot(true);
    m_refDebounce->setInterval(300);
    connect(m_refDebounce, &QTimer::timeout, this, &ReviewPane::updateReferencePhoto);
    connect(&m_photos, &pl::net::PhotoCache::ready, this, &ReviewPane::onRefPhotoReady);

    m_info = new QLabel(this);
    m_info->setTextFormat(Qt::RichText);
    m_info->setWordWrap(true);   // only a freakishly long name would wrap; the path is pre-elided

    m_guess = new QLabel(this);
    m_guess->setTextFormat(Qt::RichText);
    m_guess->setWordWrap(true);

    m_candidates = new QListWidget(this);
    m_candidates->setAlternatingRowColors(true);
    m_candidates->setUniformItemSizes(true);
    connect(m_candidates, &QListWidget::itemDoubleClicked, this, [this] { confirm(); });
    connect(m_candidates, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { m_refDebounce->start(); });

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search for a taxon…"));
    connect(m_search, &QLineEdit::textEdited, this, &ReviewPane::runSearch);

    m_useTreeTaxon = new QPushButton(this);
    m_useTreeTaxon->setToolTip(tr("Confirm this photo as the taxon currently "
                                  "selected in the Reference Trees panel. (Ctrl+T; F9 shows "
                                  "the panel)"));
    connect(m_useTreeTaxon, &QPushButton::clicked, this, &ReviewPane::useTreeTaxon);

    m_confirmButton = new QPushButton(tr("&Confirm"), this);
    m_rejectButton = new QPushButton(tr("&Reject"), this);
    auto *rejectBtn = m_rejectButton;
    auto *genusBtn = new QPushButton(tr("&Genus only"), this);
    auto *notTaxonBtn = new QPushButton(tr("&Not a taxon"), this);
    auto *skipBtn = new QPushButton(tr("&Skip"), this);
    m_undoButton = new QPushButton(tr("&Undo"), this);
    m_confirmButton->setDefault(true);
    m_confirmButton->setToolTip(tr("Confirm (Ctrl+Enter). With several photos selected, all of "
                                   "them are confirmed as the taxon chosen below."));
    rejectBtn->setToolTip(tr("Reject (Ctrl+Backspace) — every selected photo"));
    genusBtn->setToolTip(tr("Genus only (Ctrl+G) — every selected photo"));
    notTaxonBtn->setToolTip(tr("Not a taxon (Ctrl+Shift+N) — every selected photo"));
    skipBtn->setToolTip(tr("Skip (Ctrl+Right)"));
    m_undoButton->setToolTip(tr("Undo the last decision (Ctrl+Z)"));
    m_undoButton->setEnabled(false);
    connect(m_confirmButton, &QPushButton::clicked, this, &ReviewPane::confirm);
    connect(rejectBtn, &QPushButton::clicked, this, &ReviewPane::reject);
    connect(genusBtn, &QPushButton::clicked, this, &ReviewPane::genusOnly);
    connect(notTaxonBtn, &QPushButton::clicked, this, &ReviewPane::notATaxon);
    connect(skipBtn, &QPushButton::clicked, this, &ReviewPane::skip);
    connect(m_undoButton, &QPushButton::clicked, this, &ReviewPane::undo);

    auto *actions = new QHBoxLayout;
    actions->addWidget(m_confirmButton);
    actions->addWidget(rejectBtn);
    actions->addWidget(genusBtn);
    actions->addWidget(notTaxonBtn);
    actions->addWidget(skipBtn);
    actions->addStretch(1);
    actions->addWidget(m_undoButton);

    auto shortcut = [this](QKeySequence seq, void (ReviewPane::*slot)()) {
        auto *sc = new QShortcut(seq, this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, slot);
    };
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), &ReviewPane::confirm);
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), &ReviewPane::confirm);
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Backspace), &ReviewPane::reject);
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_G), &ReviewPane::genusOnly);
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), &ReviewPane::notATaxon);
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), &ReviewPane::skip);
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), &ReviewPane::applyToFolder);
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_T), &ReviewPane::useTreeTaxon);
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), &ReviewPane::undo);
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), &ReviewPane::identifyCurrent);

    m_bulkLabel = new QLabel(this);
    m_bulkLabel->setEnabled(false);   // greyed secondary text, house style
    m_recursiveCheck = new QCheckBox(tr("Include subfolders"), this);
    connect(m_recursiveCheck, &QCheckBox::toggled, this, [this] { showCurrent(); });
    m_bulkButton = new QPushButton(tr("Apply to folder"), this);
    m_bulkButton->setToolTip(tr("Apply to folder (Ctrl+Shift+F)"));
    connect(m_bulkButton, &QPushButton::clicked, this, &ReviewPane::applyToFolder);
    m_ignoreFolderButton = new QPushButton(tr("Ignore Folder…"), this);
    connect(m_ignoreFolderButton, &QPushButton::clicked, this, &ReviewPane::ignoreFolder);
    m_organiseButton = new QPushButton(tr("Open Folder in Organise"), this);
    m_organiseButton->setToolTip(tr("Name this folder's photos in Organise, a run of shots at a "
                                    "time; organising them into species folders matches them."));
    connect(m_organiseButton, &QPushButton::clicked, this, &ReviewPane::organiseCurrentFolder);
    m_suggestButton = new QPushButton(tr("Suggest Names"), this);
    m_suggestButton->setToolTip(tr("Ask iNaturalist's computer vision about the selected photos: "
                                   "one photo per run of shots (taken within 30 seconds of each "
                                   "other). Its top suggestion becomes their guess, and they move "
                                   "to To Confirm for you to check. Nothing is posted."));
    connect(m_suggestButton, &QPushButton::clicked, this, [this] {
        const QList<qint64> ids = selectedCaptureIds();
        if (!ids.isEmpty() && editAllowed())
            emit suggestNamesRequested(ids);
    });
    auto *bulk = new QHBoxLayout;
    bulk->addWidget(m_bulkLabel);
    bulk->addStretch(1);
    bulk->addWidget(m_recursiveCheck);
    bulk->addWidget(m_suggestButton);
    bulk->addWidget(m_organiseButton);
    bulk->addWidget(m_ignoreFolderButton);
    bulk->addWidget(m_bulkButton);

    // --- comparison strip: your photo | reference photo, equal columns ---
    auto *compare = new QGridLayout;
    compare->addWidget(m_yourPhotoTitle, 0, 0);
    compare->addWidget(m_refPhotoTitle, 0, 1);
    compare->addWidget(m_thumb, 1, 0);
    compare->addWidget(m_refThumb, 1, 1);
    compare->addWidget(m_refCaption, 2, 1);
    compare->setRowStretch(1, 1);
    compare->setColumnStretch(0, 1);
    compare->setColumnStretch(1, 1);

    auto *chooseHeading = new QLabel(tr("Choose the taxon"), this);
    boldize(chooseHeading);
    m_identifyButton = new QPushButton(tr("Identify (iNaturalist)"), this);
    m_identifyButton->setToolTip(tr("Ask iNaturalist's computer vision what this photo shows "
                                    "(Ctrl+Shift+I). The photo is sent to be scored, not posted; "
                                    "a suggestion picked goes to the top of the list below."));
    m_identifyButton->hide();   // until setIdentify()
    connect(m_identifyButton, &QPushButton::clicked, this, &ReviewPane::identifyCurrent);
    auto *chooseRow = new QHBoxLayout;
    chooseRow->addWidget(chooseHeading);
    chooseRow->addStretch(1);
    chooseRow->addWidget(m_identifyButton);

    auto *detail = new QWidget(this);
    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(6, 0, 0, 0);
    detailLayout->addLayout(compare, 3);
    detailLayout->addWidget(m_info);
    detailLayout->addWidget(m_guess);
    detailLayout->addLayout(chooseRow);
    detailLayout->addWidget(m_candidates, 2);
    detailLayout->addWidget(m_search);
    detailLayout->addWidget(m_useTreeTaxon);
    detailLayout->addLayout(bulk);
    detailLayout->addLayout(actions);

    m_empty = new QLabel(tr("The review queue is empty. Run Match Library, or everything is decided."),
                         this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setEnabled(false);

    m_rightStack = new QStackedWidget(this);
    m_rightStack->addWidget(m_empty);   // index 0
    m_rightStack->addWidget(detail);    // index 1

    auto *leftPane = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_photoFilter);
    leftLayout->addLayout(bucketButtons);
    leftLayout->addLayout(groupButtons);
    leftLayout->addWidget(m_queueView, 1);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(leftPane);
    split->addWidget(m_rightStack);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(split);

    m_queueProxy->setBucket(ReviewQueueFilterProxy::Bucket::NeedsReview);
    m_queueGroup->setGroupBy(ReviewQueueGroupModel::GroupBy::Guess);
    reload();
    // Nothing to confirm but photos to name: start on those.
    if (visibleQueueCount() == 0 && m_queue->noCandidateCount() > 0) {
        m_bucketIdentifyButton->setChecked(true);
        setBucket(int(ReviewQueueFilterProxy::Bucket::NoCandidate));
    }
}

void ReviewPane::reload()
{
    m_queue->reload();   // resets ReviewQueueModel -> proxy -> m_queueGroup (restoreGroupExpansion)
    if (visibleQueueCount() > 0)
        selectCapture(visibleQueueIndex(0));
    showCurrent();
}

void ReviewPane::selectCapture(const QModelIndex &leaf)
{
    if (!leaf.isValid())
        return;
    if (const QModelIndex group = m_queueGroup->groupParentOf(leaf); group.isValid())
        m_queueView->expand(group);
    m_queueView->setCurrentIndex(leaf);
    m_queueView->scrollTo(leaf);
    // The first of a group: its heading in view too, so it's clear what it's in.
    if (const QModelIndex group = m_queueGroup->groupParentOf(leaf); group.isValid() && leaf.row() == 0)
        m_queueView->scrollTo(group);
}

void ReviewPane::restoreGroupExpansion()
{
    for (int r = 0; r < m_queueGroup->rowCount(); ++r) {
        const QModelIndex g = m_queueGroup->index(r, 0);
        if (!m_queueGroup->isGroup(g))
            continue;
        m_queueView->setExpanded(g, !m_collapsedGroups.contains(m_queueGroup->groupName(g)));
    }
}

void ReviewPane::collapseAllGroups()
{
    for (int r = 0; r < m_queueGroup->rowCount(); ++r) {
        const QModelIndex g = m_queueGroup->index(r, 0);
        if (m_queueGroup->isGroup(g))
            m_collapsedGroups.insert(m_queueGroup->groupName(g));
    }
    m_queueView->collapseAll();
}

void ReviewPane::expandAllGroups()
{
    m_collapsedGroups.clear();
    m_queueView->expandAll();
}

int ReviewPane::queueCount() const
{
    return m_queue->queueCount();   // total pending, ignoring the UI filter
}

void ReviewPane::photoMenu(const QModelIndex &leaf, const QPoint &globalPos)
{
    // The selected photos (the one clicked, when it isn't among them).
    QModelIndexList leaves;
    for (const QModelIndex &i : m_queueView->selectionModel()->selectedIndexes())
        if (!m_queueGroup->isGroup(i))
            leaves << i;
    if (!leaves.contains(leaf))
        leaves = {leaf};
    QList<qint64> ids;
    QStringList paths, names;
    for (const QModelIndex &i : std::as_const(leaves)) {
        const qint64 id = i.data(ReviewQueueModel::CaptureIdRole).toLongLong();
        if (id <= 0)
            continue;
        ids << id;
        paths << i.data(ReviewQueueModel::PreviewPathRole).toString();
        // What it's filed under -- the name it was parsed from, else its
        // folder's -- as MatchReviewer learns it on a confirm.
        QString nameText = i.data(ReviewQueueModel::NameTextRole).toString();
        if (nameText.isEmpty())
            nameText = QFileInfo(i.data(ReviewQueueModel::FolderPathRole).toString()).fileName();
        names << nameText;
    }
    if (ids.isEmpty())
        return;
    QMenu menu(this);
    QAction *match = menu.addAction(
        ids.size() > 1 ? tr("Attempt to Match %1 Photos with iNat Observation to Fetch Data…").arg(ids.size())
                       : tr("Attempt to Match with iNat Observation to Fetch Data…"));
    match->setToolTip(tr("Find your iNaturalist observation of this photo — its name there may "
                         "have changed — and take its species and/or coordinates. Ctrl- or "
                         "Shift-click to pick several photos of the same observation."));
    connect(match, &QAction::triggered, this, [this, ids, paths, names] {
        emit inatMatchRequested(ids, paths, names);   // the window checks it may edit
    });
    menu.addSeparator();
    const QString folder = leaf.data(ReviewQueueModel::FolderPathRole).toString();
    menu.addAction(tr("Open Folder in Organise"), this, [this, folder] { emit organiseFolderRequested(folder); });
    menu.addAction(tr("View Full Size"), this, [this, leaf] { openViewerAt(leaf); });
    menu.exec(globalPos);
}

void ReviewPane::selectGroup(const QModelIndex &group)
{
    const QModelIndexList members = m_queueGroup->groupMembers(group);
    if (members.isEmpty())
        return;
    m_queueView->expand(group);
    QItemSelection selection;
    for (const QModelIndex &m : members)
        selection.select(m, m);
    QItemSelectionModel *sel = m_queueView->selectionModel();
    sel->setCurrentIndex(members.first(), QItemSelectionModel::NoUpdate);
    sel->select(selection, QItemSelectionModel::ClearAndSelect);
    m_queueView->scrollTo(members.first());
    showCurrent();
}

void ReviewPane::groupMenu(const QModelIndex &group, const QPoint &globalPos)
{
    const QModelIndexList members = m_queueGroup->groupMembers(group);
    if (members.isEmpty())
        return;
    QList<qint64> ids;
    for (const QModelIndex &m : members)
        ids << m.data(ReviewQueueModel::CaptureIdRole).toLongLong();
    QMenu menu(this);
    const qint64 guess = members.first().data(ReviewQueueModel::GuessInatIdRole).toLongLong();
    const QString guessName = members.first().data(ReviewQueueModel::GuessNameRole).toString();
    if (m_queueGroup->groupBy() == ReviewQueueGroupModel::GroupBy::Guess && guess > 0) {
        menu.addAction(tr("Confirm All %1 as %2").arg(ids.size()).arg(guessName), this, [this, ids, guess] {
            if (!editAllowed())
                return;
            decideEach(ids, [this, guess](qint64 cid) { return m_reviewer.confirm(cid, guess); });
        });
    }
    menu.addAction(tr("Select All %1").arg(ids.size()), this, [this, group] { selectGroup(group); });
    if (m_queueGroup->groupBy() == ReviewQueueGroupModel::GroupBy::FolderAndDate) {
        QAction *suggest = menu.addAction(tr("Suggest Names for These %1").arg(ids.size()), this, [this, ids] {
            if (editAllowed())
                emit suggestNamesRequested(ids);
        });
        suggest->setEnabled(!m_suggesting);
    }
    menu.addSeparator();
    const QString folder = members.first().data(ReviewQueueModel::FolderPathRole).toString();
    menu.addAction(tr("Open Folder in Organise"), this, [this, folder] { emit organiseFolderRequested(folder); });
    menu.exec(globalPos);
}

void ReviewPane::organiseCurrentFolder()
{
    const QModelIndex idx = m_queueView->currentIndex();
    const QString folder = idx.isValid() ? idx.data(ReviewQueueModel::FolderPathRole).toString() : QString();
    if (!folder.isEmpty())
        emit organiseFolderRequested(folder);
}

QList<qint64> ReviewPane::selectedCaptureIds() const
{
    QList<qint64> ids;
    for (const QModelIndex &i : m_queueView->selectionModel()->selectedIndexes()) {
        if (m_queueGroup->isGroup(i))
            continue;
        const qint64 id = i.data(ReviewQueueModel::CaptureIdRole).toLongLong();
        if (id > 0 && !ids.contains(id))
            ids << id;
    }
    const qint64 current = currentCaptureId();
    if (current > 0 && !ids.contains(current))
        ids = {current};
    return ids;
}

void ReviewPane::updateDecisionButtons()
{
    const int n = int(selectedCaptureIds().size());
    m_confirmButton->setText(n > 1 ? tr("&Confirm %1").arg(n) : tr("&Confirm"));
    m_rejectButton->setText(n > 1 ? tr("&Reject %1").arg(n) : tr("&Reject"));
    const bool identifying = m_queueProxy->bucket() == ReviewQueueFilterProxy::Bucket::NoCandidate;
    m_suggestButton->setVisible(identifying);
    m_suggestButton->setText(n > 1 ? tr("Suggest Names for %1").arg(n) : tr("Suggest Name"));
    m_suggestButton->setEnabled(n > 0 && !m_suggesting);
}

void ReviewPane::setSuggesting(bool running)
{
    m_suggesting = running;
    updateDecisionButtons();
}

void ReviewPane::decideEach(const QList<qint64> &captureIds, const std::function<bool(qint64)> &decide)
{
    QList<qint64> decided;
    for (qint64 cid : captureIds) {
        if (decide(cid))
            decided << cid;
    }
    if (!decided.isEmpty())
        afterDecisions(decided);
}

bool ReviewPane::askConfirmMany(const QList<qint64> &captureIds, qint64 taxonInatId)
{
    if (captureIds.size() < 2 || taxonInatId <= 0)
        return true;
    // Only worth asking when some had another guess: one taxon is going on all.
    int differing = 0;
    for (const QModelIndex &i : m_queueView->selectionModel()->selectedIndexes()) {
        if (!m_queueGroup->isGroup(i) && i.data(ReviewQueueModel::GuessInatIdRole).toLongLong() != taxonInatId)
            ++differing;
    }
    if (differing == 0)
        return true;
    const QListWidgetItem *item = m_candidates->currentItem();
    const QString name = item ? item->text().section(QStringLiteral("  ·  "), 0, 0).section(QStringLiteral("   ["), 0, 0)
                              : QString();
    return QMessageBox::question(
               this, tr("Confirm %1 Photos").arg(captureIds.size()),
               tr("Confirm all %1 selected photos as %2?\n\n%3 of them had a different guess, or none.")
                   .arg(captureIds.size())
                   .arg(name)
                   .arg(differing))
        == QMessageBox::Yes;
}

bool ReviewPane::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_thumb && event->type() == QEvent::ContextMenu) {
        const QModelIndex cur = m_queueView->currentIndex();
        if (cur.isValid() && !m_queueGroup->isGroup(cur))
            photoMenu(cur, static_cast<QContextMenuEvent *>(event)->globalPos());
        return true;
    }
    if (watched == m_thumb && event->type() == QEvent::MouseButtonDblClick) {
        openViewerAt(m_queueView->currentIndex());
        return true;
    }
    if (watched == m_refThumb && event->type() == QEvent::MouseButtonDblClick
        && m_refTaxonInatId > 0) {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://www.inaturalist.org/taxa/%1").arg(m_refTaxonInatId)));
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void ReviewPane::openViewerAt(const QModelIndex &groupIndex)
{
    if (!groupIndex.isValid() || m_queueGroup->isGroup(groupIndex))
        return;
    const qint64 targetId = groupIndex.data(ReviewQueueModel::CaptureIdRole).toLongLong();

    QVector<ImageViewer::Item> items;
    QList<qint64> ids;
    int start = 0;
    const int rows = visibleQueueCount();
    for (int r = 0; r < rows; ++r) {
        const QModelIndex idx = visibleQueueIndex(r);
        ImageViewer::Item item;
        item.path = idx.data(ReviewQueueModel::PreviewPathRole).toString();
        item.caption = idx.data(ReviewQueueModel::BaseNameRole).toString();
        if (const QString guess = idx.data(ReviewQueueModel::GuessNameRole).toString();
            !guess.isEmpty())
            item.caption += QStringLiteral("  ·  ") + guess;
        if (item.path.isEmpty())
            continue;
        if (idx.data(ReviewQueueModel::CaptureIdRole).toLongLong() == targetId)
            start = items.size();
        item.canIdentify = bool(m_makePanel);
        items.append(item);
        ids.append(idx.data(ReviewQueueModel::CaptureIdRole).toLongLong());
    }
    if (items.isEmpty())
        return;
    m_viewerCaptureIds = ids;
    // Where and when each was taken, for identifying: one query for them all.
    if (m_makePanel) {
        QStringList idList;
        for (qint64 id : std::as_const(ids))
            idList << QString::number(id);
        QHash<qint64, int> at;
        for (int i = 0; i < ids.size(); ++i)
            at.insert(ids.at(i), i);
        QSqlQuery q(QSqlDatabase::database(m_db.connectionName(), false));
        q.setForwardOnly(true);
        if (q.exec(QStringLiteral("SELECT id, latitude, longitude, captured_on FROM capture WHERE id IN (%1)")
                       .arg(idList.join(QLatin1Char(','))))) {
            while (q.next()) {
                ImageViewer::Item &item = items[at.value(q.value(0).toLongLong())];
                if (!q.value(1).isNull() && !q.value(2).isNull()) {
                    item.hasGps = true;
                    item.latitude = q.value(1).toDouble();
                    item.longitude = q.value(2).toDouble();
                }
                item.taken = QDate::fromString(q.value(3).toString().left(10), Qt::ISODate);
            }
        }
    }

    if (!m_viewer) {
        m_viewer = new ImageViewer(this);
        if (m_makePanel) {
            // I in the viewer: a suggestion picked is the decision for that photo.
            m_viewer->setIdentifyPanel(m_makePanel());
            connect(m_viewer, &ImageViewer::taxonChosen, this, [this](int index, const taxonomy::Taxon &taxon) {
                const qint64 cid = m_viewerCaptureIds.value(index);
                if (cid <= 0 || !editAllowed())
                    return;
                m_ensureKnown(taxon, [this, cid, taxon] {
                    if (m_reviewer.confirm(cid, taxon.inatId))
                        afterDecision(cid);
                });
            });
        }
    }
    m_viewer->setItems(items, start);
    m_viewer->show();
    m_viewer->raise();
    m_viewer->activateWindow();
}

int ReviewPane::visibleQueueCount() const
{
    return m_queueGroup->captureCount();
}

QModelIndex ReviewPane::visibleQueueIndex(int row) const
{
    return m_queueGroup->captureAt(row);
}

qint64 ReviewPane::currentCaptureId() const
{
    const QModelIndex idx = m_queueView->currentIndex();
    return idx.isValid() ? idx.data(ReviewQueueModel::CaptureIdRole).toLongLong() : 0;
}

int ReviewPane::currentFolderId() const
{
    const QModelIndex idx = m_queueView->currentIndex();
    return idx.isValid() ? idx.data(ReviewQueueModel::FolderIdRole).toInt() : 0;
}

qint64 ReviewPane::chosenTaxonInatId() const
{
    const QListWidgetItem *item = m_candidates->currentItem();
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

void ReviewPane::showCurrent()
{
    updateBucketButtons();
    if (m_identify) {
        m_identify->clear();
        m_identify->hide();
    }

    const QModelIndex idx = m_queueView->currentIndex();
    const bool have = idx.isValid() && !m_queueGroup->isGroup(idx);

    if (!have) {
        const bool identifying = m_queueProxy->bucket() == ReviewQueueFilterProxy::Bucket::NoCandidate;
        m_empty->setText(m_queue->queueCount() == 0 ? tr("The review queue is empty. Run Match Library, or "
                                                         "everything is decided.")
                         : !m_photoFilter->text().isEmpty() ? tr("No photos match the filter.")
                         : identifying ? tr("No photos need identifying: every one left has a guess "
                                            "under To Confirm.")
                                       : tr("No guesses left to confirm. Photos with no guess are "
                                            "under Needs Identifying."));
        m_rightStack->setCurrentIndex(0);
        m_captureName.clear();
        m_captureFolder.clear();
        renderCaptureInfo();
        m_guess->clear();
        m_candidates->clear();
        m_confirmButton->setEnabled(false);
        m_capturePixmapSrc = QPixmap();
        m_thumb->setPixmap(QPixmap());
        m_bulkLabel->clear();
        m_bulkButton->setEnabled(false);
        m_ignoreFolderButton->setEnabled(false);
        m_organiseButton->setEnabled(false);
        clearReferencePhoto();
        updateTreeTaxonButton();
        return;
    }
    m_rightStack->setCurrentIndex(1);

    const QString folder = idx.data(ReviewQueueModel::FolderPathRole).toString();
    const QString nameText = idx.data(ReviewQueueModel::NameTextRole).toString();
    const QString baseName = idx.data(ReviewQueueModel::BaseNameRole).toString();
    m_captureName = nameText.isEmpty() ? baseName : nameText;
    m_captureFolder = folder;
    renderCaptureInfo();

    const QString guessName = idx.data(ReviewQueueModel::GuessNameRole).toString();
    const double conf = idx.data(ReviewQueueModel::ConfidenceRole).toDouble();
    const QString note = idx.data(ReviewQueueModel::NoteRole).toString();
    if (!guessName.isEmpty() && idx.data(ReviewQueueModel::SuggestedRole).toBool())
        m_guess->setText(tr("iNaturalist suggests: <b>%1</b>  ·  %2%")
                             .arg(guessName.toHtmlEscaped())
                             .arg(qRound(conf * 100)));
    else if (!guessName.isEmpty())
        m_guess->setText(tr("Engine's guess: <b>%1</b>  ·  %2% confidence")
                             .arg(guessName.toHtmlEscaped())
                             .arg(int(conf * 100)));
    else
        m_guess->setText(
            QStringLiteral("<span style='color:%1'>%2</span>")
                .arg(pl::themeColors(pl::currentThemeVariant()).mutedText.name(),
                     (note.isEmpty() ? tr("No engine guess for this photo.") : note)
                         .toHtmlEscaped()));

    const QString path = idx.data(ReviewQueueModel::PreviewPathRole).toString();
    m_capturePixmapSrc = path.isEmpty()
                             ? QPixmap()
                             : m_thumbs.thumbnail({}, path, thumb::ThumbnailCache::kPreviewPx);
    m_thumb->setText(m_capturePixmapSrc.isNull() ? tr("(preview loading)") : QString());
    rescalePhotos();

    const auto guessInat = idx.data(ReviewQueueModel::GuessInatIdRole).toLongLong();
    QList<pl::match::TaxonCandidate> candidates = m_finder.forCapture(currentCaptureId());
    // iNaturalist's suggestion isn't from the name, so it's not among them: on top.
    if (guessInat > 0 && std::none_of(candidates.cbegin(), candidates.cend(),
                                      [&](const auto &c) { return c.inatId == guessInat; })) {
        pl::match::TaxonCandidate c;
        c.inatId = guessInat;
        c.name = guessName;
        QSqlQuery t(QSqlDatabase::database(m_db.connectionName(), false));
        t.prepare(QStringLiteral("SELECT rank, common_name FROM taxon WHERE inat_id = ?"));
        t.addBindValue(guessInat);
        if (t.exec() && t.next()) {
            c.rank = t.value(0).toString();
            c.commonName = t.value(1).toString();
        }
        c.matchedVia = idx.data(ReviewQueueModel::SuggestedRole).toBool() ? tr("iNaturalist suggestion")
                                                                          : QString();
        c.score = conf;
        candidates.prepend(c);
    }
    populateCandidates(candidates, guessInat);
    m_confirmButton->setEnabled(m_candidates->count() > 0 || guessInat > 0);

    const bool recursive = m_recursiveCheck->isChecked();
    const int inFolder = recursive ? m_queue->pendingUnderFolder(folder)
                                    : m_queue->pendingInFolder(currentFolderId());
    m_bulkLabel->setText(inFolder > 1
                             ? (recursive ? tr("%1 more pending in this folder and its subfolders")
                                                .arg(inFolder - 1)
                                          : tr("%1 more pending in this folder").arg(inFolder - 1))
                             : QString());
    m_bulkButton->setEnabled(inFolder > 1);
    m_ignoreFolderButton->setEnabled(currentFolderId() > 0);
    m_organiseButton->setEnabled(!folder.isEmpty());
    m_search->clear();
    updateTreeTaxonButton();
    updateDecisionButtons();
}

void ReviewPane::populateCandidates(const QList<pl::match::TaxonCandidate> &candidates,
                                    qint64 preselectInatId)
{
    m_candidates->clear();
    for (const auto &c : candidates) {
        QString label = c.name;
        if (!c.commonName.isEmpty() && c.commonName != c.name)
            label += QStringLiteral("  ·  %1").arg(c.commonName);
        label += QStringLiteral("   [%1").arg(c.rank);
        if (!c.matchedVia.isEmpty())
            label += QStringLiteral(", %1").arg(c.matchedVia);
        if (c.score > 0.0)
            label += QStringLiteral(" %1%").arg(int(c.score * 100));
        label += QLatin1Char(']');

        auto *item = new QListWidgetItem(label, m_candidates);
        item->setData(Qt::UserRole, c.inatId);
        if (c.inatId == preselectInatId)
            m_candidates->setCurrentItem(item);
    }
    if (!m_candidates->currentItem() && m_candidates->count() > 0)
        m_candidates->setCurrentRow(0);
}

void ReviewPane::runSearch(const QString &text)
{
    if (text.trimmed().size() < 3) {
        // Restore the auto-suggested candidates without disturbing the search box
        // or the queue selection.
        const QModelIndex idx = m_queueView->currentIndex();
        if (idx.isValid())
            populateCandidates(m_finder.forCapture(currentCaptureId()),
                               idx.data(ReviewQueueModel::GuessInatIdRole).toLongLong());
        return;
    }
    populateCandidates(m_finder.search(text), 0);
}

void ReviewPane::rescalePhotos()
{
    auto fit = [](QLabel *lbl, const QPixmap &src) {
        if (src.isNull())
            return;
        const QSize t = lbl->contentsRect().size();
        if (t.width() < 8 || t.height() < 8)
            return;
        lbl->setPixmap((src.width() > t.width() || src.height() > t.height())
                           ? src.scaled(t, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                           : src);
    };
    fit(m_thumb, m_capturePixmapSrc);
    fit(m_refThumb, m_refPixmapSrc);
}

void ReviewPane::renderCaptureInfo()
{
    if (m_captureName.isEmpty() && m_captureFolder.isEmpty()) {
        m_info->clear();
        m_info->setToolTip(QString());
        return;
    }
    const int w = qMax(80, m_info->width() - 4);
    const QString shortFolder =
        QFontMetrics(m_info->font()).elidedText(m_captureFolder, Qt::ElideLeft, w);
    m_info->setText(
        QStringLiteral("<b>%1</b><br><span style='color:%2'>%3</span>")
            .arg(m_captureName.toHtmlEscaped(),
                 pl::themeColors(pl::currentThemeVariant()).mutedText.name(),
                 shortFolder.toHtmlEscaped()));
    m_info->setToolTip(m_captureFolder);
}

void ReviewPane::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    rescalePhotos();
    renderCaptureInfo();
}

void ReviewPane::clearReferencePhoto()
{
    m_refDebounce->stop();
    m_refTaxonInatId = 0;
    m_refPendingUrl.clear();
    m_refPixmapSrc = QPixmap();
    m_refThumb->setPixmap(QPixmap());
    m_refThumb->setText(tr("(no taxon selected)"));
    m_refPhotoTitle->setText(tr("iNaturalist reference"));
    m_refCaption->clear();
}

void ReviewPane::updateReferencePhoto()
{
    const qint64 inat = chosenTaxonInatId();
    if (inat <= 0) {
        clearReferencePhoto();
        return;
    }
    if (inat == m_refTaxonInatId)
        return;   // already resolved for this taxon

    m_refTaxonInatId = inat;
    m_refPendingUrl.clear();
    m_refPixmapSrc = QPixmap();
    m_refThumb->setPixmap(QPixmap());
    m_refPhotoTitle->setText(tr("iNaturalist reference"));
    m_refCaption->clear();

    taxonomy::TaxonomyStore store(m_db.connectionName());
    const auto tp = store.taxonPhoto(inat);
    if (!tp.photoUrl.isEmpty()) {
        showRefPixmap(inat, tp.photoUrl, tp.name, tp.attribution);
        return;
    }
    if (m_refNoPhoto.contains(inat)) {
        m_refThumb->setText(tr("(no reference photo)"));
        return;
    }

    // Not cached locally — ask iNaturalist for this taxon's default photo.
    m_refThumb->setText(tr("loading…"));
    m_inat.fetchTaxon(inat, [this, inat](pl::net::Outcome<pl::net::TaxonDetail> out) {
        if (inat != m_refTaxonInatId)
            return;   // the user moved on
        const pl::taxonomy::Taxon &t = out.value.taxon;
        if (!out.ok() || t.photoUrl.isEmpty()) {
            if (out.ok())
                m_refNoPhoto.insert(inat);
            m_refPixmapSrc = QPixmap();
            m_refThumb->setPixmap(QPixmap());
            m_refThumb->setText(out.ok() ? tr("(no reference photo)")
                                         : tr("(couldn't reach iNaturalist)"));
            m_refCaption->clear();
            return;
        }
        taxonomy::TaxonomyStore(m_db.connectionName()).upsertTaxon(t);
        showRefPixmap(inat, t.photoUrl, t.name, t.photoAttribution);
    });
}

void ReviewPane::showRefPixmap(qint64 inatId, const QString &url, const QString &name,
                               const QString &attribution)
{
    if (inatId != m_refTaxonInatId)
        return;
    m_refPendingName = name;
    m_refPendingAttr = attribution;
    m_refPhotoTitle->setText(name.isEmpty() ? tr("iNaturalist reference")
                                            : tr("Reference — %1").arg(name));

    const QPixmap pm = m_photos.photo(url);
    if (pm.isNull()) {
        // Miss: PhotoCache is downloading it. onRefPhotoReady() finishes the job.
        m_refPendingUrl = url;
        m_refPixmapSrc = QPixmap();
        m_refThumb->setPixmap(QPixmap());
        m_refThumb->setText(tr("loading…"));
        m_refCaption->clear();
        return;
    }
    m_refPendingUrl.clear();
    m_refPixmapSrc = pm;
    m_refThumb->setText(QString());
    rescalePhotos();
    m_refCaption->setText(attribution);
}

void ReviewPane::onRefPhotoReady(const QString &url)
{
    if (m_refPendingUrl.isEmpty() || url != m_refPendingUrl)
        return;
    m_refPendingUrl.clear();

    // ready() fires on download failure too, so a still-null pixmap means it
    // could not be fetched — don't leave the label stuck on "loading…".
    const QPixmap pm = m_photos.photo(url);
    if (pm.isNull()) {
        m_refPixmapSrc = QPixmap();
        m_refThumb->setPixmap(QPixmap());
        m_refThumb->setText(tr("(couldn't load photo)"));
        m_refCaption->clear();
        return;
    }
    m_refPixmapSrc = pm;
    m_refThumb->setText(QString());
    rescalePhotos();
    m_refCaption->setText(m_refPendingAttr);
}

void ReviewPane::afterDecisions(const QList<qint64> &captureIds)
{
    m_lastDecided = captureIds;
    m_undoButton->setEnabled(true);
    int pos = m_queueGroup->captureNumberOf(m_queueView->currentIndex());
    // Several decided: carry on from the first of them in the list.
    for (int r = 0; r < visibleQueueCount() && captureIds.size() > 1; ++r) {
        if (captureIds.contains(visibleQueueIndex(r).data(ReviewQueueModel::CaptureIdRole).toLongLong())) {
            pos = r;
            break;
        }
    }
    // By id on the source; proxy + group model follow.
    m_queue->dropCaptures(QSet<qint64>(captureIds.cbegin(), captureIds.cend()));
    if (visibleQueueCount() > 0) {
        const int row = qBound(0, pos < 0 ? 0 : pos, visibleQueueCount() - 1);
        selectCapture(visibleQueueIndex(row));
    }
    showCurrent();
    emit queueChanged();
}

void ReviewPane::undo()
{
    if (!editAllowed())
        return;
    if (m_lastDecided.isEmpty())
        return;
    const QList<qint64> ids = std::exchange(m_lastDecided, {});
    m_undoButton->setEnabled(false);
    bool any = false;
    for (qint64 id : ids)
        any = m_reviewer.resetToPending(id) || any;
    if (!any)
        return;
    const qint64 cid = ids.first();
    m_queue->reload();
    for (int r = 0; r < visibleQueueCount(); ++r) {
        const QModelIndex idx = visibleQueueIndex(r);
        if (idx.data(ReviewQueueModel::CaptureIdRole).toLongLong() == cid) {
            selectCapture(idx);
            break;
        }
    }
    showCurrent();
    emit queueChanged();
}

void ReviewPane::setBucket(int bucket)
{
    m_queueProxy->setBucket(ReviewQueueFilterProxy::Bucket(bucket));
    m_queueGroup->setGroupBy(ReviewQueueFilterProxy::Bucket(bucket) == ReviewQueueFilterProxy::Bucket::NoCandidate
                                 ? ReviewQueueGroupModel::GroupBy::FolderAndDate
                                 : ReviewQueueGroupModel::GroupBy::Guess);
    const QModelIndex cur = m_queueView->currentIndex();
    if ((!cur.isValid() || m_queueGroup->isGroup(cur)) && visibleQueueCount() > 0)
        selectCapture(visibleQueueIndex(0));
    showCurrent();
}

void ReviewPane::updateBucketButtons()
{
    const int total = m_queue->queueCount();
    const int noCandidate = m_queue->noCandidateCount();
    m_bucketConfirmButton->setText(tr("To Confirm (%1)").arg(total - noCandidate));
    m_bucketIdentifyButton->setText(tr("Needs Identifying (%1)").arg(noCandidate));
}

void ReviewPane::setIdentify(std::function<IdentifyPanel *()> makePanel, EnsureKnown ensureKnown)
{
    m_makePanel = std::move(makePanel);
    m_ensureKnown = std::move(ensureKnown);
    m_identify = m_makePanel();
    m_identify->setParent(this);
    m_identify->hide();
    auto *box = static_cast<QBoxLayout *>(m_candidates->parentWidget()->layout());
    box->insertWidget(box->indexOf(m_candidates), m_identify, 2);
    connect(m_identify, &IdentifyPanel::chosen, this, [this](const taxonomy::Taxon &taxon) {
        useIdentified(currentCaptureId(), taxon);
    });
    m_identifyButton->show();
}

pl::inat::IdentifyRequest ReviewPane::identifyRequest(qint64 captureId, const QString &path) const
{
    pl::inat::IdentifyRequest request;
    request.key = path;
    request.path = path;
    QSqlQuery q(QSqlDatabase::database(m_db.connectionName(), false));
    q.prepare(QStringLiteral("SELECT latitude, longitude, captured_on FROM capture WHERE id = ?"));
    q.addBindValue(captureId);
    if (q.exec() && q.next()) {
        if (!q.value(0).isNull() && !q.value(1).isNull())
            request.lonLat = QPointF(q.value(1).toDouble(), q.value(0).toDouble());
        if (const QDate d = QDate::fromString(q.value(2).toString().left(10), Qt::ISODate); d.isValid())
            request.date = d;
    }
    return request;
}

void ReviewPane::identifyCurrent()
{
    const QModelIndex idx = m_queueView->currentIndex();
    const qint64 cid = currentCaptureId();
    if (!m_identify || cid <= 0 || !idx.isValid())
        return;
    const QString path = idx.data(ReviewQueueModel::PreviewPathRole).toString();
    if (path.isEmpty())
        return;
    m_identify->show();
    m_identify->identify(identifyRequest(cid, path));
}

void ReviewPane::useIdentified(qint64 captureId, const taxonomy::Taxon &taxon)
{
    if (captureId <= 0 || !editAllowed())
        return;
    m_ensureKnown(taxon, [this, captureId, taxon] {
        if (captureId != currentCaptureId())
            return;   // moved on meanwhile
        QListWidgetItem *item = nullptr;
        for (int i = 0; i < m_candidates->count(); ++i) {
            if (m_candidates->item(i)->data(Qt::UserRole).toLongLong() == taxon.inatId)
                item = m_candidates->item(i);
        }
        if (!item) {
            QString label = taxon.name;
            if (!taxon.commonName.isEmpty() && taxon.commonName != taxon.name)
                label += QStringLiteral("  ·  %1").arg(taxon.commonName);
            label += QStringLiteral("   [%1, %2]").arg(taxon.rank, tr("iNaturalist suggestion"));
            item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, taxon.inatId);
            m_candidates->insertItem(0, item);
        }
        m_candidates->setCurrentItem(item);
        m_confirmButton->setEnabled(true);
        m_confirmButton->setFocus();
    });
}

void ReviewPane::confirm()
{
    if (!editAllowed())
        return;
    const QList<qint64> ids = selectedCaptureIds();
    const qint64 taxon = chosenTaxonInatId();
    if (ids.isEmpty() || !askConfirmMany(ids, taxon))
        return;
    decideEach(ids, [this, taxon](qint64 cid) { return m_reviewer.confirm(cid, taxon); });
}

void ReviewPane::setTreeTaxon(qint64 inatId, const QString &name)
{
    m_treeTaxonInatId = inatId;
    m_treeTaxonName = name;
    updateTreeTaxonButton();
}

void ReviewPane::updateTreeTaxonButton()
{
    const bool haveTaxon = m_treeTaxonInatId > 0;
    m_useTreeTaxon->setText(haveTaxon && !m_treeTaxonName.isEmpty()
                                ? tr("Use Selected Taxon on Tree — %1").arg(m_treeTaxonName)
                                : tr("Use Selected Taxon on Tree"));
    m_useTreeTaxon->setEnabled(haveTaxon && currentCaptureId() > 0);
}

void ReviewPane::useTreeTaxon()
{
    if (!editAllowed())
        return;
    const QList<qint64> ids = selectedCaptureIds();
    const qint64 taxon = m_treeTaxonInatId;
    if (ids.isEmpty() || taxon <= 0)
        return;
    decideEach(ids, [this, taxon](qint64 cid) { return m_reviewer.confirm(cid, taxon); });
}

void ReviewPane::reject()
{
    if (!editAllowed())
        return;
    decideEach(selectedCaptureIds(), [this](qint64 cid) { return m_reviewer.reject(cid); });
}

void ReviewPane::genusOnly()
{
    if (!editAllowed())
        return;
    decideEach(selectedCaptureIds(), [this](qint64 cid) {
        const qint64 genus = m_finder.folderGenusInatId(cid);
        return genus > 0 && m_reviewer.setGenusOnly(cid, genus);
    });
}

void ReviewPane::notATaxon()
{
    if (!editAllowed())
        return;
    decideEach(selectedCaptureIds(), [this](qint64 cid) { return m_reviewer.markNotATaxon(cid, true); });
}

void ReviewPane::skip()
{
    const int n = visibleQueueCount();
    if (n < 2)
        return;
    const int pos = m_queueGroup->captureNumberOf(m_queueView->currentIndex());
    selectCapture(visibleQueueIndex(((pos < 0 ? 0 : pos) + 1) % n));
}

void ReviewPane::applyToFolder()
{
    if (!editAllowed())
        return;
    const int folderId = currentFolderId();
    const qint64 taxon = chosenTaxonInatId();
    if (folderId <= 0 || taxon <= 0)
        return;

    const bool recursive = m_recursiveCheck->isChecked();
    const QModelIndex idx = m_queueView->currentIndex();
    const QString folder = idx.data(ReviewQueueModel::FolderPathRole).toString();
    const int pending = recursive ? m_queue->pendingUnderFolder(folder)
                                  : m_queue->pendingInFolder(folderId);
    const QString question = recursive
        ? tr("Apply the selected taxon to %1 pending photo(s) in \"%2\" and its subfolders? "
             "This can't be undone.")
              .arg(pending)
              .arg(folder)
        : tr("Apply the selected taxon to %1 pending photo(s) in \"%2\"? This can't be undone.")
              .arg(pending)
              .arg(folder);
    if (QMessageBox::question(this, tr("Apply to Folder"), question)
        != QMessageBox::Yes)
        return;

    const int changed = m_reviewer.applyTaxonToFolder(folderId, taxon, recursive, true);
    if (changed <= 0)
        return;

    m_queue->reload();
    if (visibleQueueCount() > 0)
        selectCapture(visibleQueueIndex(0));
    showCurrent();
    emit queueChanged();
}

void ReviewPane::ignoreFolder()
{
    if (!editAllowed())
        return;
    const int folderId = currentFolderId();
    if (folderId <= 0)
        return;

    const QModelIndex idx = m_queueView->currentIndex();
    const QString folder = idx.data(ReviewQueueModel::FolderPathRole).toString();
    const int pending = m_queue->pendingUnderFolder(folder);
    const QString question =
        tr("Mark \"%1\" and its subfolders as not-a-taxon? %2 pending photo(s) will leave "
           "the review queue.")
            .arg(folder)
            .arg(pending);
    if (QMessageBox::question(this, tr("Ignore Folder"), question) != QMessageBox::Yes)
        return;

    if (m_reviewer.ignoreFolderTree(folderId) <= 0)
        return;

    m_queue->reload();
    if (visibleQueueCount() > 0)
        selectCapture(visibleQueueIndex(0));
    showCurrent();
    emit queueChanged();
}

void ReviewPane::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
}

bool ReviewPane::editAllowed()
{
    if (!m_readOnly)
        return true;
    emit editRequested();
    return false;
}

} // namespace pl
