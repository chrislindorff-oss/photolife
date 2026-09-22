#include "ui/ReviewPane.h"

#include "db/Database.h"
#include "model/ReviewQueueGroupModel.h"
#include "model/ReviewQueueModel.h"
#include "net/INatClient.h"
#include "net/PhotoCache.h"
#include "taxonomy/TaxonomyStore.h"
#include "taxonomy/TaxonomyTypes.h"
#include "thumb/ThumbnailCache.h"
#include "ui/ImageViewer.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {
namespace {
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
    , m_queueProxy(new QSortFilterProxyModel(this))
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

    auto *collapseBtn = new QToolButton(this);
    collapseBtn->setText(tr("Collapse all"));
    collapseBtn->setToolTip(tr("Collapse every group of same-named photos"));
    connect(collapseBtn, &QToolButton::clicked, this, &ReviewPane::collapseAllGroups);
    auto *expandBtn = new QToolButton(this);
    expandBtn->setText(tr("Expand all"));
    expandBtn->setToolTip(tr("Expand every group of same-named photos"));
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
    connect(m_queueView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this] { showCurrent(); });
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
                                  "selected in the Reference Trees dock. (Ctrl+T)"));
    connect(m_useTreeTaxon, &QPushButton::clicked, this, &ReviewPane::useTreeTaxon);

    auto *confirmBtn = new QPushButton(tr("&Confirm"), this);
    auto *rejectBtn = new QPushButton(tr("&Reject"), this);
    auto *genusBtn = new QPushButton(tr("&Genus only"), this);
    auto *notTaxonBtn = new QPushButton(tr("&Not a taxon"), this);
    auto *skipBtn = new QPushButton(tr("&Skip"), this);
    m_undoButton = new QPushButton(tr("&Undo"), this);
    confirmBtn->setDefault(true);
    confirmBtn->setToolTip(tr("Confirm (Ctrl+Enter)"));
    rejectBtn->setToolTip(tr("Reject (Ctrl+Backspace)"));
    genusBtn->setToolTip(tr("Genus only (Ctrl+G)"));
    notTaxonBtn->setToolTip(tr("Not a taxon (Ctrl+Shift+N)"));
    skipBtn->setToolTip(tr("Skip (Ctrl+Right)"));
    m_undoButton->setToolTip(tr("Undo the last decision (Ctrl+Z)"));
    m_undoButton->setEnabled(false);
    connect(confirmBtn, &QPushButton::clicked, this, &ReviewPane::confirm);
    connect(rejectBtn, &QPushButton::clicked, this, &ReviewPane::reject);
    connect(genusBtn, &QPushButton::clicked, this, &ReviewPane::genusOnly);
    connect(notTaxonBtn, &QPushButton::clicked, this, &ReviewPane::notATaxon);
    connect(skipBtn, &QPushButton::clicked, this, &ReviewPane::skip);
    connect(m_undoButton, &QPushButton::clicked, this, &ReviewPane::undo);

    auto *actions = new QHBoxLayout;
    actions->addWidget(confirmBtn);
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

    m_bulkLabel = new QLabel(this);
    m_bulkLabel->setEnabled(false);   // greyed secondary text, house style
    m_recursiveCheck = new QCheckBox(tr("Include subfolders"), this);
    connect(m_recursiveCheck, &QCheckBox::toggled, this, [this] { showCurrent(); });
    m_bulkButton = new QPushButton(tr("Apply to folder"), this);
    m_bulkButton->setToolTip(tr("Apply to folder (Ctrl+Shift+F)"));
    connect(m_bulkButton, &QPushButton::clicked, this, &ReviewPane::applyToFolder);
    m_ignoreFolderButton = new QPushButton(tr("Ignore Folder…"), this);
    connect(m_ignoreFolderButton, &QPushButton::clicked, this, &ReviewPane::ignoreFolder);
    auto *bulk = new QHBoxLayout;
    bulk->addWidget(m_bulkLabel);
    bulk->addStretch(1);
    bulk->addWidget(m_recursiveCheck);
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

    auto *detail = new QWidget(this);
    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(6, 0, 0, 0);
    detailLayout->addLayout(compare, 3);
    detailLayout->addWidget(m_info);
    detailLayout->addWidget(m_guess);
    detailLayout->addWidget(chooseHeading);
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

    reload();
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

bool ReviewPane::eventFilter(QObject *watched, QEvent *event)
{
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
        items.append(item);
    }
    if (items.isEmpty())
        return;

    if (!m_viewer)
        m_viewer = new ImageViewer(this);
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
    const QModelIndex idx = m_queueView->currentIndex();
    const bool have = idx.isValid() && !m_queueGroup->isGroup(idx);

    if (!have) {
        m_empty->setText(m_queue->queueCount() == 0
                             ? tr("The review queue is empty. Run Match Library, or "
                                  "everything is decided.")
                             : tr("No photos match the filter."));
        m_rightStack->setCurrentIndex(0);
        m_captureName.clear();
        m_captureFolder.clear();
        renderCaptureInfo();
        m_guess->clear();
        m_candidates->clear();
        m_capturePixmapSrc = QPixmap();
        m_thumb->setPixmap(QPixmap());
        m_bulkLabel->clear();
        m_bulkButton->setEnabled(false);
        m_ignoreFolderButton->setEnabled(false);
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
    if (!guessName.isEmpty())
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
    populateCandidates(m_finder.forCapture(currentCaptureId()), guessInat);

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
    m_search->clear();
    updateTreeTaxonButton();
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

void ReviewPane::afterDecision(qint64 captureId)
{
    m_lastDecidedCaptureId = captureId;
    m_undoButton->setEnabled(true);
    const int pos = m_queueGroup->captureNumberOf(m_queueView->currentIndex());
    m_queue->dropCapture(captureId);   // by id on the source; proxy + group model follow
    if (visibleQueueCount() > 0) {
        const int row = qBound(0, pos < 0 ? 0 : pos, visibleQueueCount() - 1);
        selectCapture(visibleQueueIndex(row));
    }
    showCurrent();
    emit queueChanged();
}

void ReviewPane::undo()
{
    if (m_lastDecidedCaptureId <= 0)
        return;
    const qint64 cid = m_lastDecidedCaptureId;
    m_lastDecidedCaptureId = 0;
    m_undoButton->setEnabled(false);
    if (!m_reviewer.resetToPending(cid))
        return;
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

void ReviewPane::confirm()
{
    const qint64 cid = currentCaptureId();
    if (cid <= 0)
        return;
    if (m_reviewer.confirm(cid, chosenTaxonInatId()))
        afterDecision(cid);
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
    const qint64 cid = currentCaptureId();
    if (cid <= 0 || m_treeTaxonInatId <= 0)
        return;
    if (m_reviewer.confirm(cid, m_treeTaxonInatId))
        afterDecision(cid);
}

void ReviewPane::reject()
{
    const qint64 cid = currentCaptureId();
    if (cid > 0 && m_reviewer.reject(cid))
        afterDecision(cid);
}

void ReviewPane::genusOnly()
{
    const qint64 cid = currentCaptureId();
    if (cid <= 0)
        return;
    const qint64 genus = m_finder.folderGenusInatId(cid);
    if (genus > 0 && m_reviewer.setGenusOnly(cid, genus))
        afterDecision(cid);
}

void ReviewPane::notATaxon()
{
    const qint64 cid = currentCaptureId();
    if (cid > 0 && m_reviewer.markNotATaxon(cid, true))
        afterDecision(cid);
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

} // namespace pl
