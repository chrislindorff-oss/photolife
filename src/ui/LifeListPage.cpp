#include "ui/LifeListPage.h"

#include <QCoreApplication>

#include "app/Application.h"
#include "catalogue/CameraInfoStore.h"
#include "db/Database.h"
#include "model/LifeListModel.h"
#include "raw/RawPreview.h"
#include "collection/ContactSheetWriter.h"
#include "scan/Exif.h"
#include "settings/Settings.h"
#include "ui/CaptureDelegate.h"

#include <QCheckBox>
#include <QLocale>
#include <QSaveFile>
#include <QRegularExpression>
#include <QDir>
#include <QImageReader>
#include <QBuffer>
#include <QStandardPaths>
#include <QProgressDialog>
#include <QPushButton>
#include <QListWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDateEdit>
#include <QComboBox>
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

    // The period: all time, a year, or a trip (a date range and place words).
    m_period = new QComboBox(this);
    m_period->setToolTip(tr("All time, a year's list (the species photographed that year), or a trip's."));
    connect(m_period, &QComboBox::activated, this, [this] {
        if (m_period->currentData().toString() == QLatin1String("newTrip")) {
            editTrips();
            return;
        }
        reload(m_projectId, m_treeName);
    });
    m_onlyNew = new QCheckBox(tr("Only new to my list"), this);
    m_onlyNew->setToolTip(tr("Only the species first photographed ever in this period."));
    m_onlyNew->hide();
    connect(m_onlyNew, &QCheckBox::toggled, this, [this] { showEntries(); });
    m_tripsButton = new QToolButton(this);
    m_tripsButton->setText(tr("Trips…"));
    m_tripsButton->setToolTip(tr("Name trips -- a date range and, if you like, words from the places -- to see "
                                 "each one's list."));
    connect(m_tripsButton, &QToolButton::clicked, this, &LifeListPage::editTrips);
    m_shareButton = new QToolButton(this);
    m_shareButton->setText(tr("Share…"));
    m_shareButton->setPopupMode(QToolButton::InstantPopup);
    auto *shareMenu = new QMenu(m_shareButton);
    shareMenu->addAction(tr("Save as a Web Page…"), this, [this] { share(false); });
    shareMenu->addAction(tr("Save as a PDF…"), this, [this] { share(true); });
    m_shareButton->setMenu(shareMenu);
    m_shareButton->setToolTip(tr("The list as shown -- this period, filtered as it is -- with each first photo: a "
                                 "web page to send or put online, or a PDF to print."));
    fillPeriods();

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
    controls->addWidget(m_period);
    controls->addWidget(m_tripsButton);
    controls->addWidget(m_onlyNew);
    controls->addWidget(m_search, 1);
    controls->addWidget(m_infra);
    controls->addWidget(m_thumbs);
    controls->addWidget(m_columnsButton);
    controls->addWidget(m_shareButton);
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
    for (const coverage::LifeListEntry &e : std::as_const(m_entries)) {
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
        for (QList<coverage::LifeListEntry> *list : {&m_all, &m_entries}) {
            for (coverage::LifeListEntry &e : *list) {
                if (const auto it = byCapture.constFind(e.captureId); it != byCapture.constEnd()) {
                    e.camera = it->camera;
                    e.focalLength = it->focalLength;
                    e.cameraChecked = true;
                }
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

void LifeListPage::reload(int projectId, const QString &treeName)
{
    m_projectId = projectId;
    if (!treeName.isEmpty())
        m_treeName = treeName;
    const int pid = projectId;
    const bool open = pid > 0 && m_app.database().isOpen();
    m_all = open ? coverage::loadLifeList(m_app.database().connectionName(), pid) : QList<coverage::LifeListEntry>{};
    const coverage::LifeListFilter filter = periodFilter();
    m_entries = filter.isEmpty() || !open
                    ? m_all
                    : coverage::loadLifeList(m_app.database().connectionName(), pid, QDate::currentDate(), filter);
    m_onlyNew->setVisible(!filter.isEmpty());
    showEntries();
}

void LifeListPage::fillPeriods()
{
    const QVariant current = m_period->currentData();
    m_period->clear();
    m_period->addItem(tr("All time"), QStringLiteral("all"));
    for (int year = QDate::currentDate().year(); year >= QDate::currentDate().year() - 30; --year)
        m_period->addItem(QString::number(year), QStringLiteral("year:%1").arg(year));
    const QStringList trips = m_app.settings().lifeListTrips();
    if (!trips.isEmpty())
        m_period->insertSeparator(m_period->count());
    for (int i = 0; i < trips.size(); ++i)
        m_period->addItem(tr("Trip: %1").arg(trips.at(i).section(QLatin1Char('\t'), 0, 0)), QStringLiteral("trip:%1").arg(i));
    m_period->addItem(tr("New Trip…"), QStringLiteral("newTrip"));
    m_period->setCurrentIndex(std::max(0, m_period->findData(current)));
}

coverage::LifeListFilter LifeListPage::periodFilter() const
{
    const QString key = m_period ? m_period->currentData().toString() : QString();
    if (key.startsWith(QLatin1String("year:"))) {
        const int year = key.mid(5).toInt();
        return {QDate(year, 1, 1), QDate(year, 12, 31), QString()};
    }
    if (key.startsWith(QLatin1String("trip:"))) {
        const QString trip = m_app.settings().lifeListTrips().value(key.mid(5).toInt());
        return {QDate::fromString(trip.section(QLatin1Char('\t'), 1, 1), Qt::ISODate),
                QDate::fromString(trip.section(QLatin1Char('\t'), 2, 2), Qt::ISODate), trip.section(QLatin1Char('\t'), 3, 3)};
    }
    return {};
}

QString LifeListPage::periodLabel() const
{
    const QString key = m_period->currentData().toString();
    return key == QLatin1String("all") || key.isEmpty() ? QString() : m_period->currentText().remove(tr("Trip: "));
}

void LifeListPage::showEntries()
{
    const bool showBelow = m_infra->isChecked();
    QList<coverage::LifeListEntry> source = m_entries;
    int newInPeriod = 0;
    const coverage::LifeListFilter filter = periodFilter();
    if (!filter.isEmpty()) {
        // New to the list: first photographed ever within the period.
        QHash<qint64, QString> firstEver;
        for (const auto &e : std::as_const(m_all))
            firstEver.insert(e.inatId, e.firstOn);
        QList<coverage::LifeListEntry> onlyNew;
        for (const auto &e : std::as_const(source)) {
            if (firstEver.value(e.inatId) == e.firstOn && !e.firstOn.isEmpty()) {
                onlyNew << e;
                newInPeriod += e.belowSpecies && !showBelow ? 0 : 1;
            }
        }
        if (m_onlyNew->isChecked())
            source = onlyNew;
    }
    const QList<coverage::LifeListEntry> shown = coverage::numberedLifeList(source, showBelow);
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
    for (const auto &e : std::as_const(m_entries))
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
        if (!filter.isEmpty() && !m_onlyNew->isChecked())
            text += QLatin1Char(' ') + tr("%n of them new to your list.", nullptr, newInPeriod);
    }
    if (shown.isEmpty() && !filter.isEmpty() && m_projectId > 0)
        text = tr("Nothing in this tree photographed in %1.").arg(periodLabel());
    m_counts->setText(text);

    emit countChanged(int(shown.size()));
}

QString lifeListWebPage(const QList<coverage::LifeListEntry> &entries, const QList<QImage> &images,
                        const QString &title, const QString &subtitle)
{
    auto tr = [](const char *text, const char *, int n = -1) {
        return QCoreApplication::translate("pl::LifeListPage", text, nullptr, n);
    };
    QString html;
    html += QStringLiteral("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                           "<title>%1</title><style>"
                           "body{font-family:system-ui,sans-serif;margin:0 auto;max-width:1100px;padding:16px;"
                           "color:#1d2433;background:#fafaf7}"
                           "h1{margin:0}p.sub{color:#5b6475;margin:4px 0 20px}"
                           ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:14px}"
                           ".card{background:#fff;border:1px solid #e2e4e8;border-radius:8px;overflow:hidden}"
                           ".card img{width:100%;aspect-ratio:4/3;object-fit:cover;display:block;background:#eee}"
                           ".card .t{padding:8px 10px;font-size:14px}.n{color:#5b6475}.s{font-style:italic}"
                           ".m{color:#5b6475;font-size:12px;margin-top:4px}"
                           "@media (prefers-color-scheme:dark){body{background:#16181d;color:#e6e8ec}"
                           ".card{background:#20232a;border-color:#2e323b}p.sub,.n,.m{color:#9aa3b2}}"
                           "</style></head><body>")
                .arg(title.toHtmlEscaped());
    html += QStringLiteral("<h1>%1</h1><p class=\"sub\">%2 · %3</p><div class=\"grid\">")
                .arg(title.toHtmlEscaped(), subtitle.toHtmlEscaped(),
                     tr("%n species", nullptr, int(entries.size())).toHtmlEscaped());
    for (int i = 0; i < entries.size(); ++i) {
        const coverage::LifeListEntry &e = entries.at(i);
        QString img;
        if (!images.at(i).isNull()) {
            QByteArray jpeg;
            QBuffer buffer(&jpeg);
            buffer.open(QIODevice::WriteOnly);
            images.at(i).save(&buffer, "JPG", 80);
            img = QStringLiteral("<img alt=\"%1\" src=\"data:image/jpeg;base64,%2\">")
                      .arg(e.name.toHtmlEscaped(), QString::fromLatin1(jpeg.toBase64()));
        } else {
            img = QStringLiteral("<img alt=\"\">");
        }
        const bool hasCommon = !e.commonName.isEmpty() && e.commonName != e.name;
        html += QStringLiteral("<div class=\"card\">%1<div class=\"t\"><span class=\"n\">%2.</span> %3"
                               "<div class=\"s\">%4</div><div class=\"m\">%5%6</div></div></div>")
                    .arg(img, e.number > 0 ? QString::number(e.number) : QStringLiteral("—"),
                         (hasCommon ? e.commonName : e.name).toHtmlEscaped(),
                         hasCommon ? e.name.toHtmlEscaped() : QString(),
                         (e.firstOn.isEmpty() ? tr("undated", nullptr) : model::LifeListModel::dateText(e.firstOn)).toHtmlEscaped(),
                         e.locality.isEmpty() ? QString() : QStringLiteral(" · ") + e.locality.toHtmlEscaped());
    }
    html += QStringLiteral("</div><p class=\"sub\">%1</p></body></html>\n")
                .arg(tr("Made with PhotoLife on %1", nullptr).arg(QLocale().toString(QDate::currentDate(), QLocale::LongFormat))
                         .toHtmlEscaped());
    return html;
}

void LifeListPage::editTrips()
{
    QStringList trips = m_app.settings().lifeListTrips();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Trips"));
    auto *list = new QListWidget(&dialog);
    auto describe = [](const QString &trip) {
        const QStringList f = trip.split(QLatin1Char('\t'));
        const QLocale locale;
        QString text = QStringLiteral("%1 — %2 to %3")
                           .arg(f.value(0),
                                locale.toString(QDate::fromString(f.value(1), Qt::ISODate), QLocale::ShortFormat),
                                locale.toString(QDate::fromString(f.value(2), Qt::ISODate), QLocale::ShortFormat));
        if (!f.value(3).isEmpty())
            text += QStringLiteral(" · ") + f.value(3);
        return text;
    };
    auto refill = [&] {
        list->clear();
        for (const QString &t : std::as_const(trips))
            list->addItem(describe(t));
    };
    refill();
    auto *name = new QLineEdit(&dialog);
    name->setPlaceholderText(tr("e.g. Grampians, spring 2025"));
    auto *from = new QDateEdit(QDate::currentDate().addDays(-7), &dialog);
    auto *to = new QDateEdit(QDate::currentDate(), &dialog);
    for (QDateEdit *d : {from, to}) {
        d->setCalendarPopup(true);
        d->setDisplayFormat(QStringLiteral("d MMM yyyy"));
    }
    auto *place = new QLineEdit(&dialog);
    place->setPlaceholderText(tr("optional: words in the places, e.g. Grampians"));
    auto *add = new QPushButton(tr("Add Trip"), &dialog);
    auto *remove = new QPushButton(tr("Remove"), &dialog);
    add->setAutoDefault(false);
    remove->setAutoDefault(false);
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        if (name->text().trimmed().isEmpty() || from->date() > to->date())
            return;
        QString clean = name->text().simplified();
        clean.remove(QLatin1Char('\t'));
        trips << QStringLiteral("%1\t%2\t%3\t%4")
                     .arg(clean, from->date().toString(Qt::ISODate), to->date().toString(Qt::ISODate),
                          place->text().simplified().remove(QLatin1Char('\t')));
        refill();
        list->setCurrentRow(int(trips.size()) - 1);
        name->clear();
        place->clear();
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        const int row = list->currentRow();
        if (row >= 0 && row < trips.size()) {
            trips.removeAt(row);
            refill();
        }
    });
    auto *form = new QFormLayout;
    form->addRow(tr("Name:"), name);
    auto *dates = new QHBoxLayout;
    dates->addWidget(from);
    dates->addWidget(new QLabel(tr("to"), &dialog));
    dates->addWidget(to);
    dates->addStretch(1);
    form->addRow(tr("Dates:"), dates);
    form->addRow(tr("Places:"), place);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(remove);
    row->addWidget(add);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(tr("A trip's list is the species photographed between its dates -- and, with place "
                                "words, only where the photo's place has them all."),
                             &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addWidget(list, 1);
    layout->addLayout(form);
    layout->addLayout(row);
    layout->addWidget(buttons);
    dialog.resize(520, 420);
    const int before = int(trips.size());
    if (dialog.exec() != QDialog::Accepted) {
        m_period->setCurrentIndex(std::max(0, m_period->findData(QStringLiteral("all"))));
        fillPeriods();
        return;
    }
    m_app.settings().setLifeListTrips(trips);
    fillPeriods();
    // A trip just added: show it.
    if (trips.size() > before)
        m_period->setCurrentIndex(std::max(0, m_period->findData(QStringLiteral("trip:%1").arg(trips.size() - 1))));
    else if (m_period->currentData().toString() == QLatin1String("newTrip"))
        m_period->setCurrentIndex(0);
    reload(m_projectId, m_treeName);
}

void LifeListPage::share(bool pdf)
{
    // What's listed, in the order shown.
    QList<coverage::LifeListEntry> entries;
    for (int r = 0; r < m_proxy->rowCount(); ++r)
        entries << m_model->entry(m_proxy->mapToSource(m_proxy->index(r, 0)).row());
    if (entries.isEmpty())
        return;
    const QString period = periodLabel();
    const QString title = m_treeName.isEmpty() ? tr("Photographic Life List") : m_treeName;
    const QString subtitle = period.isEmpty() ? tr("Photographic Life List") : tr("Photographed in %1").arg(period);
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString base = QStringLiteral("%1 - %2").arg(title, period.isEmpty() ? tr("life list") : period);
    base.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("-"));
    const QString path = QFileDialog::getSaveFileName(
        this, pdf ? tr("Save the List as a PDF") : tr("Save the List as a Web Page"),
        QDir(docs).filePath(base + (pdf ? QStringLiteral(".pdf") : QStringLiteral(".html"))),
        pdf ? tr("PDF (*.pdf)") : tr("Web page (*.html)"));
    if (path.isEmpty())
        return;

    // The first photos, small: only those on this computer (never a download).
    QProgressDialog progress(tr("Gathering the photos…"), tr("Cancel"), 0, int(entries.size()), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    QList<QImage> images;
    for (int i = 0; i < entries.size(); ++i) {
        progress.setValue(i);
        if (progress.wasCanceled())
            return;
        const QString preview = entries.at(i).previewPath;
        QImage image;
        if (!preview.isEmpty() && !CaptureDelegate::isOnlineOnly(preview) && QFileInfo::exists(preview)) {
            QImageReader reader(preview);
            reader.setAutoTransform(true);
            const QSize full = reader.size();
            if (full.isValid())
                reader.setScaledSize(full.scaled(480, 480, Qt::KeepAspectRatio));
            image = reader.read();
            if (image.isNull() && raw::isAvailable())
                image = raw::extractPreview(preview, 480).scaled(480, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        images << image;
    }
    progress.setValue(int(entries.size()));

    QString error;
    bool ok = false;
    if (pdf) {
        collection::ContactSheetOptions options = m_app.settings().contactSheetOptions();
        options.title = title;
        options.subtitle = subtitle;
        QList<collection::ContactSheetEntry> sheet;
        for (int i = 0; i < entries.size(); ++i) {
            const coverage::LifeListEntry &e = entries.at(i);
            collection::ContactSheetEntry c;
            c.name = e.name;
            c.commonName = e.commonName;
            c.attribution = QStringLiteral("%1 · %2%3")
                                .arg(e.number > 0 ? QString::number(e.number) : QStringLiteral("—"),
                                     e.firstOn.isEmpty() ? tr("undated") : model::LifeListModel::dateText(e.firstOn),
                                     e.locality.isEmpty() ? QString() : QStringLiteral(" · ") + e.locality);
            c.photo = images.at(i);
            sheet << c;
        }
        ok = collection::writeContactSheet(path, sheet, options, &error);
    } else {
        const QString html = lifeListWebPage(entries, images, title, subtitle);
        QSaveFile file(path);
        ok = file.open(QIODevice::WriteOnly) && file.write(html.toUtf8()) >= 0 && file.commit();
        if (!ok)
            error = file.errorString();
    }
    if (!ok) {
        QMessageBox::warning(this, tr("Share"), tr("It couldn't be saved: %1").arg(error));
        return;
    }
    emit statusMessage(tr("Saved %n species to %1.", nullptr, int(entries.size())).arg(QDir::toNativeSeparators(path)), 8000);
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
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
