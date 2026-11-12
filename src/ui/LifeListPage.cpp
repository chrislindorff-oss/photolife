#include "ui/LifeListPage.h"

#include "app/Application.h"
#include "catalogue/CameraInfoStore.h"
#include "db/Database.h"
#include "model/LifeListModel.h"
#include "scan/Exif.h"
#include "settings/Settings.h"
#include "ui/CaptureDelegate.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace pl {

LifeListPage::LifeListPage(Application &app, Hooks hooks, QWidget *parent)
    : QWidget(parent), m_app(app), m_hooks(std::move(hooks))
{
    using LL = model::LifeListModel;
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Filter by common or scientific name, or place…"));
    m_search->setClearButtonEnabled(true);

    m_infra = new QCheckBox(tr("Show subspecies, varieties and hybrids"), this);
    m_infra->setToolTip(tr("Also number subspecies, varieties, forms and hybrids as entries of their own. "
                                   "Off: species only (a photo of a subspecies still counts for its species)."));
    m_infra->setChecked(m_app.settings().lifeListShowsInfraspecific());
    connect(m_infra, &QCheckBox::toggled, this, [this](bool on) {
        m_app.settings().setLifeListShowsInfraspecific(on);
        showEntries();
    });
    m_thumbs = new QCheckBox(tr("Thumbnails"), this);
    m_thumbs->setToolTip(tr("Show each entry's first photo. Double-click or right-click one to see it full "
                                    "screen."));
    m_thumbs->setChecked(m_app.settings().lifeListThumbnails());

    m_counts = new QLabel(this);
    m_counts->setObjectName(QStringLiteral("muted"));
    m_counts->setWordWrap(true);

    m_model = new LL(m_app.thumbnails(), this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(LL::SortRole);
    m_proxy->setFilterKeyColumn(-1);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    connect(m_search, &QLineEdit::textChanged, m_proxy,
            &QSortFilterProxyModel::setFilterFixedString);

    m_view = new QTreeView(this);
    m_view->setModel(m_proxy);
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(LL::NumberColumn, Qt::AscendingOrder);
    QHeaderView *header = m_view->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(LL::PlaceColumn, QHeaderView::Stretch);
    const QFontMetrics fm(m_view->font());
    header->resizeSection(LL::NumberColumn, fm.horizontalAdvance(QStringLiteral("00000")) + 24);
    header->resizeSection(LL::PhotoColumn, 64 + 16);
    header->resizeSection(LL::DateColumn, fm.horizontalAdvance(tr("First photographed")) + 32);
    header->resizeSection(LL::CommonColumn, fm.horizontalAdvance(QStringLiteral("M")) * 16);
    header->resizeSection(LL::ScientificColumn, fm.horizontalAdvance(QStringLiteral("M")) * 17);
    header->resizeSection(LL::PhotosColumn, fm.horizontalAdvance(tr("Photos")) + 32);
    header->resizeSection(LL::CameraColumn, fm.horizontalAdvance(QStringLiteral("Nikon COOLPIX P330")) + 24);
    header->resizeSection(LL::FocalColumn, fm.horizontalAdvance(tr("Focal length")) + 32);

    const auto applyThumbnails = [this](bool on) {
        m_model->setShowThumbnails(on);
        m_view->setIconSize(on ? QSize(64, 64) : QSize(16, 16));
        m_view->header()->setSectionHidden(model::LifeListModel::PhotoColumn, !on);
        // Uniform rows take their height from the first: make it measure again.
        m_view->doItemsLayout();
    };
    applyThumbnails(m_thumbs->isChecked());

    // Columns…: which columns show (# always does). Photo is the Thumbnails
    // tick; Camera and Focal length come from the first photo's EXIF.
    m_columnsButton = new QToolButton(this);
    m_columnsButton->setText(tr("Columns…"));
    m_columnsButton->setToolTip(tr("Choose the columns to show — including the first photo's camera and "
                                           "focal length. Right-clicking the column headings does the same."));
    m_columnsButton->setPopupMode(QToolButton::InstantPopup);
    m_columnsMenu = new QMenu(m_columnsButton);
    m_columnsButton->setMenu(m_columnsMenu);
    QAction *photoAction = m_columnsMenu->addAction(tr("Photo (thumbnails)"));
    photoAction->setCheckable(true);
    photoAction->setChecked(m_thumbs->isChecked());
    connect(photoAction, &QAction::toggled, m_thumbs, &QCheckBox::setChecked);
    connect(m_thumbs, &QCheckBox::toggled, this, [this, applyThumbnails, photoAction](bool on) {
        m_app.settings().setLifeListThumbnails(on);
        applyThumbnails(on);
        photoAction->setChecked(on);
    });
    static const QList<QPair<int, const char *>> kOptionalColumns = {
        {LL::DateColumn, "date"},     {LL::CommonColumn, "common"}, {LL::ScientificColumn, "scientific"},
        {LL::PlaceColumn, "place"},   {LL::CameraColumn, "camera"}, {LL::FocalColumn, "focal"},
        {LL::PhotosColumn, "photos"},
    };
    const QStringList hidden = m_app.settings().lifeListHiddenColumns();
    for (const auto &[column, key] : kOptionalColumns) {
        const QString title = m_model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
        QAction *action = m_columnsMenu->addAction(title);
        action->setCheckable(true);
        const bool shown = !hidden.contains(QLatin1String(key));
        action->setChecked(shown);
        header->setSectionHidden(column, !shown);
        connect(action, &QAction::toggled, this, [this, column, key](bool on) {
            m_view->header()->setSectionHidden(column, !on);
            QStringList keys = m_app.settings().lifeListHiddenColumns();
            keys.removeAll(QLatin1String(key));
            if (!on)
                keys << QLatin1String(key);
            m_app.settings().setLifeListHiddenColumns(keys);
            if (on && (column == model::LifeListModel::CameraColumn || column == model::LifeListModel::FocalColumn))
                readCameras();
        });
    }
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { m_columnsMenu->exec(m_view->header()->mapToGlobal(pos)); });

    connect(m_view, &QAbstractItemView::activated, this,
            [this](const QModelIndex &idx) { openPhoto(idx); });
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex idx = m_view->indexAt(pos);
        if (!idx.isValid())
            return;
        m_view->setCurrentIndex(idx);
        const qint64 inatId = idx.data(model::LifeListModel::InatIdRole).toLongLong();
        const QPersistentModelIndex at(idx);
        QMenu menu(this);
        QAction *full = menu.addAction(tr("View Full Screen"), this, [this, at] {
            if (at.isValid())
                openPhoto(at);
        });
        full->setEnabled(!idx.data(model::LifeListModel::PreviewPathRole).toString().isEmpty());
        menu.setDefaultAction(full);
        menu.addAction(tr("Show All Photos of This Taxon"), this, [this, inatId] { emit showTaxonPhotos(inatId); });
        menu.addAction(tr("Open on iNaturalist"), this, [inatId] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.inaturalist.org/taxa/%1").arg(inatId)));
        });
        menu.exec(m_view->viewport()->mapToGlobal(pos));
    });

    auto *layout = new QVBoxLayout(this);
    auto *controls = new QHBoxLayout;
    controls->addWidget(m_search, 1);
    controls->addWidget(m_infra);
    controls->addWidget(m_thumbs);
    controls->addWidget(m_columnsButton);
    layout->addLayout(controls);
    layout->addWidget(m_counts);
    layout->addWidget(m_view, 1);
}

void LifeListPage::readCameras()
{
    if (!m_model || !m_app.database().isOpen())
        return;
    const QHeaderView *header = m_view->header();
    if (header->isSectionHidden(model::LifeListModel::CameraColumn)
        && header->isSectionHidden(model::LifeListModel::FocalColumn))
        return;   // nobody's looking: don't touch the disk
    for (const coverage::LifeListEntry &e : std::as_const(m_all)) {
        if (e.cameraChecked || e.previewPath.isEmpty() || m_cameraRequested.contains(e.captureId))
            continue;
        // Never make a cloud-synced file download itself just for this.
        if (CaptureDelegate::isOnlineOnly(e.previewPath))
            continue;
        m_cameraRequested.insert(e.captureId);
        m_cameraQueue.append({e.captureId, e.previewPath});
        ++m_cameraTotal;
    }
    if (!m_cameraReading)
        readNextCameraChunk();
}

void LifeListPage::readNextCameraChunk()
{
    constexpr int kChunk = 50;
    if (m_cameraQueue.isEmpty()) {
        if (m_cameraReading && m_cameraDone > 0)
            emit statusMessage(tr("Read the camera details of %n photo(s).", nullptr, m_cameraDone), 4000);
        m_cameraReading = false;
        m_cameraDone = m_cameraTotal = 0;
        return;
    }
    m_cameraReading = true;
    const QList<QPair<int, QString>> chunk = m_cameraQueue.mid(0, kChunk);
    m_cameraQueue.remove(0, chunk.size());
    emit statusMessage(tr("Reading camera details from the life list's photos… %1 of %2")
                                 .arg(m_cameraDone)
                                 .arg(m_cameraTotal), 0);
    auto *watcher = new QFutureWatcher<QList<catalogue::CameraInfo>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, chunk] {
        const QList<catalogue::CameraInfo> infos = watcher->result();
        watcher->deleteLater();
        const int read = int(chunk.size());
        // A file that wasn't there (renamed, moved) is tried again on a later reload.
        QSet<int> found;
        for (const catalogue::CameraInfo &info : infos)
            found.insert(info.captureId);
        for (const auto &item : chunk)
            if (!found.contains(item.first))
                m_cameraRequested.remove(item.first);
        catalogue::CameraInfoStore store(m_app.database().connectionName());
        if (!store.save(infos))
            qWarning() << "Couldn't keep the camera details:" << store.error();
        QHash<int, catalogue::CameraInfo> byCapture;
        for (const catalogue::CameraInfo &info : infos) {
            byCapture.insert(info.captureId, info);
            m_model->setCameraInfo(info.captureId, info.camera, info.focalLength);
        }
        for (coverage::LifeListEntry &e : m_all) {
            if (const auto it = byCapture.constFind(e.captureId); it != byCapture.constEnd()) {
                e.camera = it->camera;
                e.focalLength = it->focalLength;
                e.cameraChecked = true;
            }
        }
        m_cameraDone += read;
        readNextCameraChunk();
    });
    watcher->setFuture(QtConcurrent::run([chunk] {
        QList<catalogue::CameraInfo> out;
        for (const auto &[captureId, path] : chunk) {
            if (!QFileInfo::exists(path))
                continue;   // moved or renamed: tried again once the library has caught up
            const scan::ExifData exif = scan::readPhotoExif(path);
            out.append({captureId, scan::prettyCameraName(exif.cameraMake, exif.cameraModel), exif.focalLength});
        }
        return out;
    }));
}

void LifeListPage::reload(int projectId)
{
    m_projectId = projectId;
    const int pid = projectId;
    m_all = pid > 0 && m_app.database().isOpen()
                        ? coverage::loadLifeList(m_app.database().connectionName(), pid)
                        : QList<coverage::LifeListEntry>{};
    showEntries();
}

void LifeListPage::showEntries()
{
    const bool showBelow = m_infra->isChecked();
    const QList<coverage::LifeListEntry> shown = coverage::numberedLifeList(m_all, showBelow);
    // Kept across the reload: the row picked and how far down the list is.
    const qint64 picked = m_view->currentIndex().data(model::LifeListModel::InatIdRole).toLongLong();
    const int scrolled = m_view->verticalScrollBar()->value();
    m_model->setEntries(shown);
    if (picked > 0) {
        const QModelIndexList hit = m_proxy->match(m_proxy->index(0, 0),
                                                           model::LifeListModel::InatIdRole, picked, 1,
                                                           Qt::MatchExactly);
        if (!hit.isEmpty())
            m_view->setCurrentIndex(hit.first());
    }
    m_view->verticalScrollBar()->setValue(scrolled);
    readCameras();

    int species = 0, below = 0, belowTotal = 0, undated = 0;
    QString first, latest;
    for (const auto &e : std::as_const(m_all))
        belowTotal += e.belowSpecies ? 1 : 0;
    for (const auto &e : shown) {
        ++(e.belowSpecies ? below : species);
        if (e.firstOn.isEmpty()) {
            ++undated;
            continue;
        }
        if (first.isEmpty())
            first = e.firstOn;
        latest = e.firstOn;
    }
    m_infra->setText(tr("Show subspecies, varieties and hybrids (%1)").arg(belowTotal));
    m_infra->setEnabled(belowTotal > 0 || showBelow);

    QString text;
    if (shown.isEmpty()) {
        text = m_projectId > 0 ? tr("Nothing in this tree photographed yet.") : tr("Select a reference tree.");
    } else {
        text = species == 1 ? tr("1 species photographed") : tr("%1 species photographed").arg(species);
        if (showBelow)
            text += below == 1 ? tr(", and 1 subspecies, variety or hybrid")
                               : tr(", and %1 subspecies, varieties and hybrids").arg(below);
        if (!first.isEmpty())
            text += first == latest ? tr(" — the first on %1.").arg(model::LifeListModel::dateText(first))
                                    : tr(" — the first on %1, the latest new one on %2.")
                                          .arg(model::LifeListModel::dateText(first),
                                               model::LifeListModel::dateText(latest));
        else
            text += QLatin1Char('.');
        if (undated > 0)
            text += undated == 1 ? tr(" 1 has no usable date, so comes last.")
                                 : tr(" %1 have no usable date, so come last.").arg(undated);
    }
    m_counts->setText(text);

    emit countChanged(int(shown.size()));
}

void LifeListPage::openPhoto(const QModelIndex &proxyIndex)
{
    using LL = model::LifeListModel;
    if (!proxyIndex.isValid())
        return;
    const QString clickedPath = proxyIndex.data(LL::PreviewPathRole).toString();
    if (clickedPath.isEmpty())
        return;

    // A photo only online comes down first.
    if (CaptureDelegate::isOnlineOnly(clickedPath)) {
        const QStringList files = m_hooks.onlineOnlyRenditions({proxyIndex.data(LL::CaptureIdRole).toInt()});
        if (QMessageBox::question(this, tr("Photo Only Online"),
                                  tr("This photo is kept online, not on this computer. Download "
                                     "it now?\n\nIt then stays on this computer."))
            != QMessageBox::Yes)
            return;
        const QPersistentModelIndex index(proxyIndex);
        m_hooks.fetchPhotos(files, [this, index, clickedPath] {
            if (index.isValid() && QFileInfo::exists(clickedPath))
                openPhoto(index);
        });
        return;
    }
    // Renamed or moved outside PhotoLife: look, then open it under its new name.
    if (!QFileInfo::exists(clickedPath)) {
        if (!m_app.catalogueEditable()) {
            emit statusMessage(tr("This photo's file isn't at %1 any more.").arg(clickedPath), 8000);
            return;
        }
        const qint64 inatId = proxyIndex.data(LL::InatIdRole).toLongLong();
        emit statusMessage(tr("This photo's file has been renamed or moved — finding it…"), 0);
        m_hooks.requestScan([this, inatId, clickedPath] {
            reload(m_projectId);   // the scan's own refresh may not have run (nothing found)
            const QModelIndexList hit = m_proxy->match(m_proxy->index(0, 0), LL::InatIdRole,
                                                               inatId, 1, Qt::MatchExactly);
            if (!hit.isEmpty() && QFileInfo::exists(hit.first().data(LL::PreviewPathRole).toString())) {
                emit statusMessage(QString(), 0);
                openPhoto(hit.first());
                return;
            }
            emit statusMessage(tr("Couldn't find this photo's file: it isn't at %1 any more, and no "
                                        "file with the same contents turned up in your library folders.")
                                         .arg(clickedPath),
                                     10000);
        });
        return;
    }

    // Which are starred, in one go.
    QSet<int> starred;
    {
        QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
        if (q.exec(QStringLiteral("SELECT capture_id FROM best_shot"))) {
            while (q.next())
                starred.insert(q.value(0).toInt());
        }
    }
    // Every shown row's first photo, in the list's order, so the arrow keys step through it.
    QVector<ImageViewer::Item> items;
    int start = 0;
    for (int r = 0; r < m_proxy->rowCount(); ++r) {
        const QModelIndex at = m_proxy->index(r, 0);
        const coverage::LifeListEntry &e = m_model->entry(m_proxy->mapToSource(at).row());
        if (e.previewPath.isEmpty())
            continue;
        const bool hasCommon = !e.commonName.isEmpty() && e.commonName != e.name;
        const QString name = hasCommon ? QStringLiteral("%1 (%2)").arg(e.commonName, e.name) : e.name;
        const QString number = e.number > 0 ? QString::number(e.number) : QStringLiteral("—");
        ImageViewer::Item item;
        item.path = e.previewPath;
        item.title = QStringLiteral("%1 · %2").arg(number, hasCommon ? e.commonName : e.name);
        item.caption = QStringLiteral("%1. %2\n").arg(number, name)
                       + (e.firstOn.isEmpty()
                              ? tr("First photographed: no usable date")
                              : tr("First photographed %1").arg(model::LifeListModel::dateText(e.firstOn)))
                       + (e.locality.isEmpty() ? QString() : QStringLiteral(" · ") + e.locality);
        item.captureId = e.captureId;
        item.canBestShot = true;
        item.bestShot = starred.contains(e.captureId);
        item.canIdentify = true;
        item.taken = QDate::fromString(e.firstOn.left(10), Qt::ISODate);
        if (r == proxyIndex.row())
            start = items.size();
        items.append(item);
    }
    m_hooks.showInViewer(items, start);
}

} // namespace pl
