#include "ui/MainWindow.h"

#include "app/Application.h"
#include "app/StorageStats.h"
#include "catalogue/BestShotStore.h"
#include "db/Database.h"
#include "model/CaptureListModel.h"
#include "model/ReferencePhotoModel.h"
#include "model/TaxonomyTreeModel.h"
#include "pl/Version.h"
#include "checklist/ChecklistImporter.h"
#include "checklist/ChecklistParser.h"
#include "coverage/CoverageCalculator.h"
#include "geo/LocalityFetcher.h"
#include "inat/ExifWriter.h"
#include "inat/InatDownloadModel.h"
#include "inat/InatImportService.h"
#include "inat/InatObservationFetcher.h"
#include "inat/InatPhotoDownloader.h"
#include "match/MatchReviewer.h"
#include "match/MatchService.h"
#include "net/INatClient.h"
#include "net/PhotoCache.h"
#include "net/TileCache.h"
#include "net/UpdateChecker.h"
#include "scan/CatalogueMaintenance.h"
#include "scan/LibraryWatcher.h"
#include "scan/ScanService.h"
#include "settings/Settings.h"
#include "taxonomy/InfraspecificFiller.h"
#include "taxonomy/ProjectBuilder.h"
#include "taxonomy/ReferencePhotoFetcher.h"
#include "taxonomy/TaxonomyStore.h"
#include "thumb/ThumbnailCache.h"
#include "ui/AliasEditorDialog.h"
#include "ui/CoveragePanel.h"
#include "ui/HelpWindow.h"
#include "ui/ImageViewer.h"
#include "ui/MapView.h"
#include "ui/NewProjectDialog.h"
#include "ui/PlaceConfirmDialog.h"
#include "ui/ReferencePhotoDialog.h"
#include "ui/ReviewPane.h"
#include "ui/TaxonConfirmDialog.h"
#include "ui/TaxonPickerDialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDir>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QWidgetAction>

#include <cmath>
#include <functional>

#include "db/Database.h"

namespace pl {
namespace {

// A five-pointed star polygon inscribed in `box`, point-up.
QPolygonF makeStar(const QRectF &box)
{
    constexpr double kPi = 3.14159265358979323846;
    const QPointF c = box.center();
    const qreal outer = qMin(box.width(), box.height()) / 2.0;
    const qreal inner = outer * 0.42;
    QPolygonF star;
    for (int i = 0; i < 10; ++i) {
        const qreal r = (i % 2 == 0) ? outer : inner;
        const qreal a = -kPi / 2.0 + i * kPi / 5.0;
        star << QPointF(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
    }
    return star;
}

// Draws the base thumbnail plus a small status pip in the corner.
class CaptureDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    // Suppress the base class's own icon and (word-wrapping) text drawing —
    // both are drawn manually in paint() below. The icon is drawn ourselves
    // so we know its *exact* rect (Qt's own centering within decorationSize
    // isn't something we can reliably reproduce from the outside), and the
    // caption is drawn one elided line per field, so a long field value can
    // never wrap onto a second line and overflow the grid cell's fixed
    // height.
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->text.clear();
        option->icon = QIcon();
    }

public:
    // With both icon and text cleared above, the base class's own sizeHint()
    // (which measures those two) collapses to nearly nothing — silently
    // decoupling the item's actual paint rect from the gridSize MainWindow
    // configures, which is what left the border/caption misaligned with the
    // laid-out cell. Reporting the exact same size here (mirroring
    // MainWindow::applyCaptionFields()'s formula) keeps them locked together
    // regardless of which one the view actually uses for layout.
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        int fields = 0;
        if (const auto *m = qobject_cast<const model::CaptureListModel *>(index.model()))
            fields = m->captionFields();
        int lines = 0;
        for (int b = fields & ~model::CaptureListModel::CaptionFileType; b; b &= (b - 1))
            ++lines;
        lines = std::max(lines, 1);
        return QSize(option.decorationSize.width() + 20,
                    option.decorationSize.height() + 20 + lines * 16);
    }


    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        // Deliberately not calling the base class's paint() at all: even with
        // icon/text cleared, it still computes its own internal "text rect"
        // purely to paint a selection-highlight fill behind it, and that
        // rect collapses to a small stray sliver landing wherever Qt's
        // layout math happens to put it — visible as a small coloured
        // artifact wherever it overlaps our own caption text. We draw the
        // background and every bit of content ourselves instead; the thick
        // border below already makes selection unambiguous.
        painter->fillRect(option.rect, option.palette.color(QPalette::Base));

        const bool selected = option.state & QStyle::State_Selected;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);
        painter->setPen(QPen(selected ? option.palette.color(QPalette::Highlight)
                                      : QColor(0, 0, 0),
                            selected ? 3 : 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(option.rect.adjusted(1, 1, -2, -2));
        painter->restore();

        // Fit the thumbnail into the decoration box ourselves (KeepAspectRatio,
        // centered) so we know its exact rect — needed to anchor the pills and
        // caption to the photo's real edge rather than the (usually taller or
        // wider) bounding box reserved for it.
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        const QRect decoBox(option.rect.left() + (option.rect.width() - option.decorationSize.width()) / 2,
                            option.rect.top(), option.decorationSize.width(),
                            option.decorationSize.height());
        QRect imageRect = decoBox;
        if (!icon.isNull()) {
            const QSize native = icon.availableSizes().value(0, option.decorationSize);
            const QSize fitted = native.scaled(option.decorationSize, Qt::KeepAspectRatio);
            imageRect = QRect(QPoint(0, 0), fitted);
            imageRect.moveCenter(decoBox.center());
            painter->drawPixmap(imageRect, icon.pixmap(native));
        }
        const int imageBottom = imageRect.bottom();

        const QString status = index.data(model::CaptureListModel::MatchStatusRole).toString();
        QColor colour;
        if (status == QLatin1String("auto"))
            colour = QColor(0x2E, 0x7D, 0x32);
        else if (status == QLatin1String("confirmed"))
            colour = QColor(0x15, 0x65, 0xC0);
        else if (status == QLatin1String("pending"))
            colour = QColor(0xE6, 0x9A, 0x00);
        else
            colour = QColor(0xC6, 0x28, 0x28);

        const int d = 10;
        const QRect r = option.rect.adjusted(6, 6, 0, 0);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(colour);
        painter->drawEllipse(QRect(r.left(), r.top(), d, d));
        painter->restore();

        // A gold star just right of the status dot marks a capture the user has
        // starred as a best shot of its species.
        if (index.data(model::CaptureListModel::IsBestShotRole).toBool()) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(QColor(0, 0, 0, 90), 0.8));
            painter->setBrush(QColor(0xFF, 0xC1, 0x07));
            painter->drawPolygon(makeStar(QRectF(r.left() + d + 4, r.top() - 1, 13, 13)));
            painter->restore();
        }

        // File type and GPS are both drawn as a row of pills across the
        // bottom-center of the thumbnail, in the same style, side by side.
        QStringList badges;
        if (index.data(model::CaptureListModel::ShowFileTypeBadgeRole).toBool()) {
            const QString ext = index.data(model::CaptureListModel::ExtRole).toString().toUpper();
            if (!ext.isEmpty())
                badges << ext;
        }
        if (index.data(model::CaptureListModel::HasGpsRole).toBool())
            badges << QStringLiteral("GEO");

        if (!badges.isEmpty()) {
            QFont font = painter->font();
            font.setPointSize(7);
            font.setBold(true);
            QFontMetrics fm(font);

            const int padH = 6, pillH = 16, gap = 4;
            QList<int> widths;
            int totalW = -gap;
            for (const QString &b : std::as_const(badges)) {
                const int w = fm.horizontalAdvance(b) + padH * 2;
                widths << w;
                totalW += w + gap;
            }

            int x = option.rect.left() + (option.rect.width() - totalW) / 2;
            const int y = imageBottom - pillH - 3;

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setFont(font);
            for (int i = 0; i < badges.size(); ++i) {
                const QRect pillRect(x, y, widths.at(i), pillH);
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor(0, 0, 0, 200));
                painter->drawRoundedRect(pillRect, pillH / 2.0, pillH / 2.0);
                painter->setPen(Qt::white);
                painter->drawText(pillRect, Qt::AlignCenter, badges.at(i));
                x += widths.at(i) + gap;
            }
            painter->restore();
        }

        const QString caption = index.data(Qt::DisplayRole).toString();
        if (!caption.isEmpty()) {
            QFont font = painter->font();
            if (font.pointSize() > 0)
                font.setPointSize(qMax(1, font.pointSize() - 1));
            else
                font.setPixelSize(qMax(1, font.pixelSize() - 1));
            QFontMetrics fm(font);
            const int lineH = 16;
            const int top = imageBottom + 4;
            const QRect textArea(option.rect.left() + 4, top, option.rect.width() - 8,
                                 option.rect.bottom() - top);

            // Always the plain text colour: the caption sits below the icon,
            // outside any selection-highlight fill, so HighlightedText here
            // would render (near-)invisible against the ordinary background.
            // The border above already makes selection obvious.
            painter->save();
            painter->setFont(font);
            painter->setPen(option.palette.color(QPalette::Text));
            int y = textArea.top();
            for (const QString &line : caption.split(QLatin1Char('\n'))) {
                const QString elided = fm.elidedText(line, Qt::ElideRight, textArea.width());
                painter->drawText(QRect(textArea.left(), y, textArea.width(), lineH),
                                  Qt::AlignHCenter | Qt::AlignVCenter, elided);
                y += lineH;
            }
            painter->restore();
        }
    }
};

// Draws the same border/caption look as CaptureDelegate, for the iNaturalist
// download grid's InatDownloadModel rows. A separate class rather than reuse:
// CaptureDelegate reads model::CaptureListModel-specific roles (status,
// best-shot, badges) whose numeric values collide with unrelated
// InatDownloadModel roles (e.g. MatchStatusRole == HasGpsRole == UserRole+7),
// so calling it against this model would paint bogus status dots. The
// "faded" look for likely-duplicate candidates is already baked into the
// decoration pixmap itself (InatDownloadModel::dimmed()), so no extra
// painting is needed for that here.
class InatCandidateDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->text.clear();
        option->icon = QIcon();
    }

public:
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QString caption = index.data(Qt::DisplayRole).toString();
        const int lines = std::max(1, int(caption.count(QLatin1Char('\n'))) + 1);
        return QSize(option.decorationSize.width() + 20,
                    option.decorationSize.height() + 20 + lines * 16);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->fillRect(option.rect, option.palette.color(QPalette::Base));

        const bool selected = option.state & QStyle::State_Selected;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);
        painter->setPen(QPen(selected ? option.palette.color(QPalette::Highlight)
                                      : QColor(0, 0, 0),
                            selected ? 3 : 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(option.rect.adjusted(1, 1, -2, -2));
        painter->restore();

        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        const QRect decoBox(option.rect.left() + (option.rect.width() - option.decorationSize.width()) / 2,
                            option.rect.top(), option.decorationSize.width(),
                            option.decorationSize.height());
        QRect imageRect = decoBox;
        if (!icon.isNull()) {
            const QSize native = icon.availableSizes().value(0, option.decorationSize);
            const QSize fitted = native.scaled(option.decorationSize, Qt::KeepAspectRatio);
            imageRect = QRect(QPoint(0, 0), fitted);
            imageRect.moveCenter(decoBox.center());
            painter->drawPixmap(imageRect, icon.pixmap(native));
        }
        const int imageBottom = imageRect.bottom();

        const QString caption = index.data(Qt::DisplayRole).toString();
        if (!caption.isEmpty()) {
            QFont font = painter->font();
            if (font.pointSize() > 0)
                font.setPointSize(qMax(1, font.pointSize() - 1));
            else
                font.setPixelSize(qMax(1, font.pixelSize() - 1));
            QFontMetrics fm(font);
            const int lineH = 16;
            const int top = imageBottom + 4;
            const QRect textArea(option.rect.left() + 4, top, option.rect.width() - 8,
                                 option.rect.bottom() - top);

            painter->save();
            painter->setFont(font);
            painter->setPen(option.palette.color(QPalette::Text));
            int y = textArea.top();
            for (const QString &line : caption.split(QLatin1Char('\n'))) {
                const QString elided = fm.elidedText(line, Qt::ElideRight, textArea.width());
                painter->drawText(QRect(textArea.left(), y, textArea.width(), lineH),
                                  Qt::AlignHCenter | Qt::AlignVCenter, elided);
                y += lineH;
            }
            painter->restore();
        }
    }
};

} // namespace

// A QSortFilterProxyModel that additionally hides InatDownloadModel rows
// flagged LikelyDuplicateRole when hideFaded is on, layered on top of the
// base class's own name-substring filtering.
class InatFilterProxyModel : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setHideFaded(bool hide)
    {
        if (m_hideFaded == hide)
            return;
        m_hideFaded = hide;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        if (!QSortFilterProxyModel::filterAcceptsRow(row, parent))
            return false;
        if (m_hideFaded) {
            const QModelIndex idx = sourceModel()->index(row, 0, parent);
            if (idx.data(inat::InatDownloadModel::LikelyDuplicateRole).toBool())
                return false;
        }
        return true;
    }

private:
    bool m_hideFaded = false;
};

MainWindow::MainWindow(Application &app, QWidget *parent)
    : QMainWindow(parent), m_app(app)
{
    setWindowTitle(QString::fromLatin1(kAppName));

    m_model = new model::CaptureListModel(m_app.database(), m_app.thumbnails(), this);
    m_treeModel = new model::TaxonomyTreeModel(m_app.database(), this);
    m_builder = new taxonomy::ProjectBuilder(m_app.inat(), m_app.taxonomyStore(), this);
    m_infraFiller = new taxonomy::InfraspecificFiller(m_app.inat(), m_app.taxonomyStore(), this);
    m_refPhotoFetcher =
        new taxonomy::ReferencePhotoFetcher(m_app.inat(), m_app.taxonomyStore(), this);
    m_checklistImporter =
        new checklist::ChecklistImporter(m_app.inat(), m_app.taxonomyStore(), this);
    m_localityFetcher =
        new geo::LocalityFetcher(m_app.geocoder(), m_app.database().connectionName(), this);
    m_inatObservationFetcher = new inat::InatObservationFetcher(
        m_app.inat(), m_app.database().connectionName(), this);
    m_inatImportService =
        new inat::InatImportService(m_app.inatPhotoDownloader(), QString(), this);
    m_inatImportService->setExifWriter(&inat::writeExif);

    buildMenus();
    buildCentralWidget();
    applyCaptionFields(m_app.settings().captureCaptionFields());
    buildReferenceTreeDock();
    restoreLayout();

    connect(m_builder, &taxonomy::ProjectBuilder::progress, this,
            [this](const QString &phase, int done, int total) {
                statusBar()->showMessage(total > 0
                                             ? tr("%1 — %2 / %3").arg(phase).arg(done).arg(total)
                                             : phase);
            });
    connect(m_builder, &taxonomy::ProjectBuilder::confirmMoreSpecies, this,
            [this](int fetched, int total) {
                const auto reply = QMessageBox::question(
                    this, tr("Large Reference Tree"),
                    tr("This reference tree has reached %L1 species"
                       "%2.\n\nDownloading more means many more requests to "
                       "iNaturalist. Fetch the next %L3 species?\n\n"
                       "Choose No to stop here — the tree built so far is kept and "
                       "you can extend it later with Refresh Reference Tree.")
                        .arg(fetched)
                        .arg(total > fetched ? tr(" of about %L1").arg(total) : QString())
                        .arg(5000),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (reply == QMessageBox::Yes)
                    m_builder->continueFetching();
                else
                    m_builder->stopFetching();
            });
    auto &matcher = m_app.matchService();
    connect(&matcher, &match::MatchService::started, this,
            [this] { m_matchAction->setEnabled(false); statusBar()->showMessage(tr("Matching…")); });
    connect(&matcher, &match::MatchService::progress, this, [this](int done, int total) {
        statusBar()->showMessage(tr("Matching %1 / %2").arg(done).arg(total));
    });
    connect(&matcher, &match::MatchService::finished, this,
            [this](match::MatchEngine::Stats s) {
                m_matchAction->setEnabled(true);
                m_model->reload();
                m_bestShotModel->reload();
                updateEmptyState();
                refreshCoverage();
                m_reviewPane->reload();
                updateReviewTabText();
                updateBestShotsTabText();
                if (!s.ok()) {
                    statusBar()->showMessage(tr("Match failed: %1").arg(s.error), 10000);
                    return;
                }
                statusBar()->showMessage(
                    tr("Matched — %1 automatic, %2 to review, %3 unmatched")
                        .arg(s.autoApplied).arg(s.pending - s.unmatched).arg(s.unmatched),
                    8000);
            });

    connect(m_builder, &taxonomy::ProjectBuilder::finished, this,
            [this](bool ok, const QString &error, int projectId) {
                const bool wasNewTree = m_builderIsNewTree;
                m_builderIsNewTree = false;
                m_newTreeAction->setEnabled(true);
                m_refreshTreeAction->setEnabled(true);
                if (!ok) {
                    statusBar()->showMessage(tr("Reference tree build failed: %1").arg(error),
                                             10000);
                    return;
                }
                statusBar()->showMessage(tr("Reference tree ready."), 6000);
                reloadProjectList();
                const int idx = m_projectCombo->findData(projectId);
                if (idx >= 0)
                    m_projectCombo->setCurrentIndex(idx);
                refreshCoverage();
                if (wasNewTree)
                    fetchInfraspecificTaxa();
            });

    connect(m_infraFiller, &taxonomy::InfraspecificFiller::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(
                    tr("Checking species for subspecies/varieties — %1 / %2").arg(done).arg(total));
            });
    connect(m_infraFiller, &taxonomy::InfraspecificFiller::finished, this,
            [this](bool ok, const QString &error, int added) {
                m_fetchInfraAction->setEnabled(true);
                if (!ok) {
                    statusBar()->showMessage(
                        tr("Fetching subspecies/varieties failed: %1").arg(error), 10000);
                    return;
                }
                statusBar()->showMessage(
                    added > 0 ? tr("Added %1 subspecies/varieties.").arg(added)
                             : tr("No new subspecies or varieties found."),
                    6000);
                if (added > 0) {
                    m_treeModel->setProject(currentProjectId());
                    m_treeView->expandToDepth(1);
                    refreshCoverage();
                }
            });

    connect(m_refPhotoFetcher, &taxonomy::ReferencePhotoFetcher::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(
                    tr("Fetching reference photos — %1 / %2").arg(done).arg(total));
            });
    connect(m_refPhotoFetcher, &taxonomy::ReferencePhotoFetcher::finished, this,
            [this](bool ok, const QString &error, int updated) {
                if (m_fetchRefPhotosAction)
                    m_fetchRefPhotosAction->setEnabled(true);
                if (!ok) {
                    statusBar()->showMessage(
                        tr("Fetching reference photos failed: %1").arg(error), 10000);
                    return;
                }
                statusBar()->showMessage(
                    updated > 0 ? tr("Added %n reference photo(s).", nullptr, updated)
                                : tr("No new reference photos found."),
                    6000);
                if (m_refPhotoModel)
                    m_refPhotoModel->reload();
            });

    connect(m_localityFetcher, &geo::LocalityFetcher::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(
                    tr("Fetching photo localities — %1 / %2").arg(done).arg(total));
            });
    connect(m_localityFetcher, &geo::LocalityFetcher::finished, this,
            [this](bool ok, const QString &error, int updated) {
                if (m_fetchLocalitiesAction)
                    m_fetchLocalitiesAction->setEnabled(true);
                if (!ok) {
                    statusBar()->showMessage(
                        tr("Fetching photo localities failed: %1").arg(error), 10000);
                    return;
                }
                statusBar()->showMessage(
                    updated > 0 ? tr("Looked up %n new location(s).", nullptr, updated)
                                : tr("No new photo locations to look up."),
                    6000);
                m_model->reload();
                if (m_taxonModel)
                    m_taxonModel->reload();
                if (m_bestShotModel)
                    m_bestShotModel->reload();
            });

    connect(m_checklistImporter, &checklist::ChecklistImporter::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(tr("Importing checklist %1 / %2").arg(done).arg(total));
            });
    connect(m_checklistImporter, &checklist::ChecklistImporter::finished, this,
            [this](bool ok, const QString &error, int imported, int skipped, int unresolved) {
                m_importChecklistAction->setEnabled(true);
                if (!ok) {
                    statusBar()->showMessage(tr("Checklist import failed: %1").arg(error), 10000);
                    return;
                }
                statusBar()->showMessage(
                    tr("Checklist imported — %1 added, %2 skipped, %3 unresolved")
                        .arg(imported).arg(skipped).arg(unresolved),
                    8000);
                m_treeModel->setProject(currentProjectId());
                m_treeView->expandToDepth(1);
                refreshCoverage();
            });

    auto &scanner = m_app.scanService();
    connect(&scanner, &scan::ScanService::started, this, [this] { setScanUiRunning(true); });
    connect(&scanner, &scan::ScanService::progress, this, &MainWindow::onScanProgress);
    connect(&scanner, &scan::ScanService::finished, this, &MainWindow::onScanFinished);

    connect(&m_app.libraryWatcher(), &scan::LibraryWatcher::changeDetected, this, [this] {
        if (!m_app.scanService().isRunning()) {
            statusBar()->showMessage(tr("Library folders changed — rescanning…"), 4000);
            startScan();
        }
    });

    m_model->reload();
    updateEmptyState();
    reloadProjectList();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));

    m_addFolderAction = fileMenu->addAction(tr("&Add Folder to Library…"),
                                            this, &MainWindow::addWatchedFolder);
    m_addFolderAction->setShortcut(QKeySequence::Open);

    m_scanAction = fileMenu->addAction(tr("&Rescan Library"), this, &MainWindow::startScan);
    m_scanAction->setShortcut(QKeySequence::Refresh);

    m_cancelAction = fileMenu->addAction(tr("&Stop Scan"),
                                         &m_app.scanService(), &scan::ScanService::cancel);
    m_cancelAction->setEnabled(false);

    fileMenu->addAction(tr("Remove Photos With &Missing Files…"),
                        this, &MainWindow::removeMissingCaptures);
    fileMenu->addAction(tr("Fix RAW Photo &Locations…"), this, &MainWindow::fixRawGeolocation);
    m_fetchLocalitiesAction = fileMenu->addAction(tr("Fetch Photo &Localities"),
                                                  this, &MainWindow::fetchPhotoLocalities);

    fileMenu->addSeparator();
    m_newTreeAction = fileMenu->addAction(tr("&New Reference Tree…"),
                                          this, &MainWindow::newReferenceTree);
    m_refreshTreeAction = fileMenu->addAction(tr("Re&fresh Reference Tree"),
                                              this, &MainWindow::refreshReferenceTree);
    m_deleteTreeAction = fileMenu->addAction(tr("&Delete Reference Tree…"),
                                             this, &MainWindow::deleteReferenceTree);
    m_fetchInfraAction = fileMenu->addAction(tr("Fetch &Subspecies/Varieties…"),
                                             this, &MainWindow::fetchInfraspecificTaxa);
    m_fetchRefPhotosAction = fileMenu->addAction(tr("Fetch Reference &Photos"),
                                                 this, &MainWindow::fetchReferencePhotos);
    m_importChecklistAction = fileMenu->addAction(tr("&Import Checklist…"),
                                                  this, &MainWindow::importChecklist);
    m_matchAction = fileMenu->addAction(tr("&Match Library"), this, &MainWindow::startMatch);
    fileMenu->addAction(tr("&Learned Names…"), this, [this] {
        AliasEditorDialog dialog(m_app.database(), this);
        connect(&dialog, &AliasEditorDialog::aliasesChanged, this, [this] {
            statusBar()->showMessage(
                tr("Forgotten names take effect on the next Match Library run."), 6000);
        });
        dialog.exec();
    });

    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    m_viewModeGroup = new QActionGroup(this);
    m_viewModeGroup->setExclusive(true);

    m_viewTreeAction = viewMenu->addAction(tr("&Reference Tree"));
    m_viewTreeAction->setCheckable(true);
    m_viewTreeAction->setObjectName(QStringLiteral("viewTreeAction"));
    m_viewModeGroup->addAction(m_viewTreeAction);

    m_viewLibraryAction = viewMenu->addAction(tr("&All Library Photos"));
    m_viewLibraryAction->setCheckable(true);
    m_viewLibraryAction->setObjectName(QStringLiteral("viewLibraryAction"));
    m_viewModeGroup->addAction(m_viewLibraryAction);

    m_viewReviewAction = viewMenu->addAction(tr("&Review Unmatched"));
    m_viewReviewAction->setCheckable(true);
    m_viewReviewAction->setObjectName(QStringLiteral("viewReviewAction"));
    m_viewModeGroup->addAction(m_viewReviewAction);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *helpContents = helpMenu->addAction(
        tr("%1 &Help").arg(QString::fromLatin1(kAppName)), this, &MainWindow::showHelp);
    helpContents->setShortcut(QKeySequence::HelpContents);
    helpMenu->addSeparator();
    QAction *checkUpdate = helpMenu->addAction(tr("Check for &Updates…"), this, [this] {
        auto &checker = m_app.updateChecker();
        statusBar()->showMessage(tr("Checking for updates…"), 4000);
        checker.check();
    });
    checkUpdate->setMenuRole(QAction::ApplicationSpecificRole);
    helpMenu->addAction(tr("Storage &Usage…"), this, &MainWindow::showStorageUsage);
    QAction *about = helpMenu->addAction(
        tr("&About %1").arg(QString::fromLatin1(kAppName)), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);

    connect(&m_app.updateChecker(), &net::UpdateChecker::upToDate, this, [this](const QString &v) {
        QMessageBox::information(this, tr("Check for Updates"),
                                tr("PhotoLife %1 is up to date.").arg(v));
    });
    connect(&m_app.updateChecker(), &net::UpdateChecker::updateAvailable, this,
            [this](const QString &latest, const QString &url) {
                QMessageBox box(QMessageBox::Information, tr("Update Available"),
                                tr("PhotoLife %1 is available.").arg(latest), QMessageBox::Close,
                                this);
                if (!url.isEmpty()) {
                    box.addButton(tr("Open Download Page"), QMessageBox::AcceptRole);
                    if (box.exec() == QMessageBox::AcceptRole)
                        QDesktopServices::openUrl(QUrl(url));
                } else {
                    box.exec();
                }
            });
    connect(&m_app.updateChecker(), &net::UpdateChecker::checkFailed, this,
            [this](const QString &err) {
                statusBar()->showMessage(tr("Update check failed: %1").arg(err), 8000);
            });

    QToolBar *toolbar = addToolBar(tr("Library"));
    toolbar->setObjectName(QStringLiteral("libraryToolBar"));
    toolbar->setMovable(false);
    toolbar->addAction(m_addFolderAction);
    toolbar->addAction(m_scanAction);
    toolbar->addAction(m_cancelAction);
    toolbar->addSeparator();
    toolbar->addAction(m_newTreeAction);
    toolbar->addAction(m_importChecklistAction);
    toolbar->addAction(m_matchAction);

    toolbar->addSeparator();
    toolbar->addAction(m_viewTreeAction);
    toolbar->addAction(m_viewLibraryAction);
    toolbar->addAction(m_viewReviewAction);

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr("Show: "), toolbar));
    m_filterCombo = new QComboBox(toolbar);
    m_filterCombo->addItem(tr("All"), QString());
    m_filterCombo->addItem(tr("Auto-matched"), QStringLiteral("auto"));
    m_filterCombo->addItem(tr("Needs review"), QStringLiteral("pending"));
    m_filterCombo->addItem(tr("Unmatched"), QStringLiteral("unmatched"));
    m_filterCombo->addItem(tr("Confirmed"), QStringLiteral("confirmed"));
    connect(m_filterCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_model->setStatusFilter(m_filterCombo->itemData(i).toString());
        updateEmptyState();
    });
    toolbar->addWidget(m_filterCombo);

    toolbar->addSeparator();
    auto *displayButton = new QToolButton(toolbar);
    displayButton->setText(tr("Display…"));
    displayButton->setToolTip(
        tr("Choose what to show under each photo, in the Photos of Tree Selection "
           "and All Library Photos grids"));
    displayButton->setPopupMode(QToolButton::InstantPopup);
    auto *displayMenu = new QMenu(displayButton);
    displayButton->setMenu(displayMenu);
    toolbar->addWidget(displayButton);

    struct FieldOption { int bit; QString label; };
    const QList<FieldOption> fieldOptions = {
        {model::CaptureListModel::CaptionName, tr("Name")},
        {model::CaptureListModel::CaptionTaxon, tr("Matched Taxon")},
        {model::CaptureListModel::CaptionDate, tr("Date Taken")},
        {model::CaptureListModel::CaptionFilename, tr("Filename")},
        {model::CaptureListModel::CaptionLocality, tr("Locality")},
        {model::CaptureListModel::CaptionFileType, tr("File Type")},
    };
    const int initialFields = m_app.settings().captureCaptionFields();
    for (const FieldOption &opt : fieldOptions) {
        auto *box = new QCheckBox(opt.label, displayMenu);
        box->setChecked(initialFields & opt.bit);
        connect(box, &QCheckBox::toggled, this, [this, bit = opt.bit](bool on) {
            int fields = m_app.settings().captureCaptionFields();
            fields = on ? (fields | bit) : (fields & ~bit);
            applyCaptionFields(fields);
        });
        auto *action = new QWidgetAction(displayMenu);
        action->setDefaultWidget(box);
        displayMenu->addAction(action);
    }
}

void MainWindow::buildReferenceTreeDock()
{
    auto *dock = new QDockWidget(tr("Reference Trees"), this);
    dock->setObjectName(QStringLiteral("referenceTreeDock"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto *panel = new QWidget(dock);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(6, 6, 6, 6);

    m_projectCombo = new QComboBox(panel);
    connect(m_projectCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_treeModel->setProject(index >= 0 ? m_projectCombo->itemData(index).toInt() : -1);
        m_treeView->expandToDepth(1);
        refreshCoverage();

        // Land the user on the newly chosen tree: select its top node, scroll
        // to the top, give the tree keyboard focus, and switch to the photo
        // grid that follows the selection.
        const QModelIndex root = m_treeModel->index(0, 0);
        if (root.isValid()) {
            m_treeView->setCurrentIndex(root);
            m_treeView->scrollToTop();
            m_treeView->setFocus();
        }
        switchToTreeTab(m_tabs->widget(0));   // Photos of Tree Selection
    });

    m_treeSearch = new QLineEdit(panel);
    m_treeSearch->setPlaceholderText(tr("Search taxa — jumps as you type, Enter for next"));
    m_treeSearch->setClearButtonEnabled(true);
    connect(m_treeSearch, &QLineEdit::textChanged, this, &MainWindow::onTreeSearchChanged);
    connect(m_treeSearch, &QLineEdit::returnPressed, this, &MainWindow::onTreeSearchNext);

    m_photographedOnly = new QCheckBox(tr("Only taxa I've photographed"), panel);
    connect(m_photographedOnly, &QCheckBox::toggled, this, [this](bool on) {
        const bool wasCollapsed = collectExpandedTaxa().isEmpty();
        mutateTreePreservingState([this, on] { m_treeModel->setPhotographedOnly(on); });
        if (on && wasCollapsed)
            m_treeView->expandToDepth(6);   // one-shot reveal of the photographed taxa
        onTreeSelectionChanged();
    });

    m_rankFilterButton = new QToolButton(panel);
    m_rankFilterButton->setText(tr("Ranks…"));
    m_rankFilterButton->setToolTip(tr("Choose which taxonomic ranks to show in the tree"));
    m_rankFilterButton->setPopupMode(QToolButton::InstantPopup);
    m_rankFilterMenu = new QMenu(m_rankFilterButton);
    m_rankFilterButton->setMenu(m_rankFilterMenu);
    connect(m_treeModel, &QAbstractItemModel::modelReset, this, &MainWindow::rebuildRankFilterMenu);

    m_treeView = new QTreeView(panel);
    m_treeView->setModel(m_treeModel);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setAlternatingRowColors(true);
    m_treeView->header()->setStretchLastSection(false);
    m_treeView->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_treeView->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_treeView->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_treeView->setColumnWidth(0, 560);
    m_treeView->setAutoScroll(false);   // keep shallow names visible when selecting deep nodes
    connect(m_treeView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &MainWindow::onTreeSelectionChanged);
    connect(m_treeView, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &) { switchToTreeTab(m_tabs->widget(0)); });   // Photos of Tree Selection
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_treeView, &QWidget::customContextMenuRequested,
            this, &MainWindow::showTreeContextMenu);

    m_coveragePanel = new CoveragePanel(panel);

    auto *split = new QSplitter(Qt::Vertical, panel);
    split->addWidget(m_treeView);
    split->addWidget(m_coveragePanel);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);

    layout->addWidget(m_projectCombo);
    layout->addWidget(m_treeSearch);
    layout->addWidget(m_photographedOnly);
    layout->addWidget(m_rankFilterButton);
    layout->addWidget(split, 1);
    dock->setWidget(panel);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    resizeDocks({dock}, {360}, Qt::Horizontal);
}

int MainWindow::currentProjectId() const
{
    const QVariant data = m_projectCombo->currentData();
    return data.isValid() ? data.toInt() : -1;
}

void MainWindow::refreshCoverage()
{
    const int pid = currentProjectId();
    if (m_refPhotoModel)
        m_refPhotoModel->setProject(pid);
    if (m_mapView)
        m_mapView->setActiveLocality(pid > 0 ? m_app.taxonomyStore().projectLocalityBox(pid)
                                              : std::nullopt);
    if (pid <= 0 || !m_app.database().isOpen()) {
        m_coveragePanel->clear();
        mutateTreePreservingState([this] { m_treeModel->setCoverage({}); });
        onTreeSelectionChanged();
        return;
    }
    m_coverage = coverage::computeCoverage(m_app.database().connectionName(), pid);
    coverage::pickRepresentatives(m_app.database().connectionName(), pid);
    m_coveragePanel->setCoverage(m_coverage);

    QStringList photoUrls;
    for (const auto &leaf : m_app.taxonomyStore().projectLeafPhotos(pid)) {
        if (!leaf.photoUrl.isEmpty())
            photoUrls << leaf.photoUrl;
    }
    const auto usage = m_app.photoCache().diskUsage(photoUrls);
    m_coveragePanel->setCacheUsage(usage.count, photoUrls.size(), usage.bytes);

    mutateTreePreservingState([this] { m_treeModel->setCoverage(m_coverage); });
    updateMissingList();
    onTreeSelectionChanged();
}

QList<qint64> MainWindow::collectExpandedTaxa(const QModelIndex &parent) const
{
    QList<qint64> ids;
    const int rows = m_treeModel->rowCount(parent);
    for (int r = 0; r < rows; ++r) {
        const QModelIndex idx = m_treeModel->index(r, 0, parent);
        if (m_treeView->isExpanded(idx)) {
            ids << idx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong();
            ids += collectExpandedTaxa(idx);   // only descend into expanded nodes
        }
    }
    return ids;
}

void MainWindow::mutateTreePreservingState(const std::function<void()> &mutate)
{
    // Snapshot while the view is idle (nothing here forces a layout).
    const QList<qint64> expanded = collectExpandedTaxa();
    const QModelIndex topIdx = m_treeView->indexAt(QPoint(2, 2));
    const qint64 topTaxon = topIdx.isValid()
                                ? topIdx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong()
                                : 0;
    const QModelIndex curIdx = m_treeView->currentIndex();
    const int curColumn = curIdx.isValid() ? curIdx.column() : 0;
    const qint64 curTaxon = curIdx.isValid()
                                ? curIdx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong()
                                : m_lastSelectedTaxon;

    m_restoringTreeState = true;
    mutate();

    // Restore: expansions, then the current index, then scroll (last — scrollTo
    // forces the pending layout; nothing before it may).
    for (qint64 id : expanded) {
        const QModelIndex idx = m_treeModel->indexForTaxon(id);
        if (idx.isValid())
            m_treeView->setExpanded(idx, true);
    }
    if (curTaxon > 0) {
        const QModelIndex idx = m_treeModel->indexForTaxon(curTaxon);
        if (idx.isValid())
            m_treeView->setCurrentIndex(idx.sibling(idx.row(), curColumn));
    }
    if (const QModelIndex top = m_treeModel->indexForTaxon(topTaxon); top.isValid())
        m_treeView->scrollTo(top, QAbstractItemView::PositionAtTop);
    else if (m_treeView->currentIndex().isValid())
        m_treeView->scrollTo(m_treeView->currentIndex(), QAbstractItemView::EnsureVisible);

    m_restoringTreeState = false;
}

void MainWindow::rebuildRankFilterMenu()
{
    const QStringList ranks = m_treeModel->availableRanks();
    if (ranks == m_rankFilterOrder)
        return;   // same ranks as last time; leave the menu (and its open state) alone

    m_rankFilterOrder = ranks;
    m_rankFilterMenu->clear();
    m_rankCheckboxes.clear();

    for (const QString &rank : ranks) {
        auto *box = new QCheckBox(rank, m_rankFilterMenu);
        box->setChecked(!m_hiddenRanks.contains(rank));
        connect(box, &QCheckBox::toggled, this, [this, rank](bool on) {
            if (on)
                m_hiddenRanks.remove(rank);
            else
                m_hiddenRanks.insert(rank);
            mutateTreePreservingState([this] { m_treeModel->setHiddenRanks(m_hiddenRanks); });
        });
        auto *action = new QWidgetAction(m_rankFilterMenu);
        action->setDefaultWidget(box);
        m_rankFilterMenu->addAction(action);
        m_rankCheckboxes.insert(rank, box);
    }
}

void MainWindow::onTreeSelectionChanged()
{
    if (m_restoringTreeState)
        return;

    const QModelIndex idx = m_treeView->currentIndex();
    m_selectedTaxon = idx.isValid()
                          ? idx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong()
                          : 0;
    if (m_selectedTaxon > 0)
        m_lastSelectedTaxon = m_selectedTaxon;
    const int projectScope = currentProjectId();
    // Both "Photos of Tree Selection" and "My Best Shots" are confined to the
    // active reference tree; a taxon scope narrows them further to one branch.
    m_taxonModel->setProjectScope(projectScope);
    m_taxonModel->setTaxonScope(m_selectedTaxon);
    if (m_refPhotoModel)
        m_refPhotoModel->setScope(m_selectedTaxon);
    if (m_bestShotModel) {
        m_bestShotModel->setProjectScope(projectScope);
        m_bestShotModel->setTaxonScope(m_selectedTaxon);
        updateBestShotsTabText();
    }
    if (m_reviewPane) {
        m_reviewPane->setTreeTaxon(
            m_selectedTaxon,
            m_selectedTaxon > 0 ? idx.data(model::TaxonomyTreeModel::NameRole).toString()
                                : QString());
    }

    if (m_selectedTaxon <= 0) {
        m_taxonInfo->setText(projectScope > 0
                                 ? tr("<i>Showing every photo in this reference tree. "
                                      "Select a taxon to narrow it.</i>")
                                 : tr("<i>Select a taxon in the tree to see its photos.</i>"));
        m_taxonRepImage->clear();
        if (m_bestShotHeader)
            m_bestShotHeader->setText(
                projectScope > 0
                    ? tr("Your best shots of species in \"%1\". Select a taxon to narrow "
                         "this to one branch.").arg(m_projectCombo->currentText())
                    : tr("Every photo you've starred as a best shot of its species."));
        return;
    }

    const auto cov = m_coverage.byTaxon.value(m_selectedTaxon);
    if (m_bestShotHeader)
        m_bestShotHeader->setText(
            tr("Your best shots of %1 and everything below it.").arg(cov.name));
    QString html = QStringLiteral("<h3 style='margin:0'>%1</h3>").arg(cov.name.toHtmlEscaped());
    if (!cov.commonName.isEmpty() && cov.commonName != cov.name)
        html += QStringLiteral("<div>%1</div>").arg(cov.commonName.toHtmlEscaped());
    html += QStringLiteral("<div style='color:gray'>%1</div>").arg(cov.rank);
    if (!cov.status.isEmpty())
        html += QStringLiteral("<div><b>%1</b></div>").arg(cov.status.toHtmlEscaped());

    if (cov.speciesTotal > 0)
        html += QStringLiteral("<p>%1 of %2 species photographed</p>")
                    .arg(cov.speciesWithPhotos).arg(cov.speciesTotal);
    html += QStringLiteral("<p>%1</p>")
                .arg(cov.captureCount == 1 ? tr("1 capture")
                                           : tr("%1 captures").arg(cov.captureCount));
    if (!cov.newestCapture.isEmpty())
        html += QStringLiteral("<p style='color:gray'>Most recent: %1</p>").arg(cov.newestCapture);
    m_taxonInfo->setText(html);

    const auto rep = coverage::representativeFor(m_app.database().connectionName(),
                                                 currentProjectId(), m_selectedTaxon);
    if (!rep.previewPath.isEmpty()) {
        const QPixmap pm = m_app.thumbnails().thumbnail(rep.previewHash, rep.previewPath,
                                                        thumb::ThumbnailCache::kGridPx);
        m_taxonRepImage->setPixmap(pm.isNull()
                                       ? QPixmap()
                                       : pm.scaled(m_taxonRepImage->size(), Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation));
    } else {
        m_taxonRepImage->setPixmap({});
        m_taxonRepImage->setText(tr("no photo"));
    }
}

void MainWindow::updateMissingList()
{
    if (!m_missingList)
        return;
    m_missingList->clear();

    QList<coverage::TaxonCoverage> missing;
    for (const auto &tc : m_coverage.byTaxon) {
        if (taxonomy::TaxonomyStore::isLeafRank(tc.rank) && !tc.subtreeHasPhotos) {
            missing.append(tc);
        }
    }
    std::sort(missing.begin(), missing.end(),
              [](const coverage::TaxonCoverage &a, const coverage::TaxonCoverage &b) {
                  return a.name < b.name;
              });

    const QString filter = m_missingSearch ? m_missingSearch->text().trimmed() : QString();
    for (const auto &tc : missing) {
        if (!filter.isEmpty() && !tc.name.contains(filter, Qt::CaseInsensitive)
            && !tc.commonName.contains(filter, Qt::CaseInsensitive))
            continue;

        QString label = tc.name;
        if (!tc.commonName.isEmpty() && tc.commonName != tc.name)
            label = QStringLiteral("%1  ·  %2").arg(tc.name, tc.commonName);
        if (!tc.status.isEmpty())
            label += QStringLiteral("   — %1").arg(tc.status);
        auto *item = new QListWidgetItem(label, m_missingList);
        item->setData(Qt::UserRole, tc.inatId);
        if (!tc.status.isEmpty())
            item->setForeground(QColor(0xB0, 0x50, 0x00));
    }

    const int missingTab = m_tabs->indexOf(m_missingList->parentWidget());
    if (missingTab >= 0)
        m_tabs->setTabText(missingTab, missing.isEmpty()
                                          ? tr("Missing Species")
                                          : tr("Missing Species (%1)").arg(missing.size()));
}

void MainWindow::reassignTaxonPhotos()
{
    QList<int> captureIds;
    const QModelIndexList selected = m_taxonGrid->selectionModel()->selectedIndexes();
    if (!selected.isEmpty()) {
        for (const QModelIndex &idx : selected)
            captureIds << idx.data(model::CaptureListModel::IdRole).toInt();
    } else {
        const int rows = m_taxonModel->rowCount();
        if (rows == 0) {
            statusBar()->showMessage(tr("No photos to reassign."), 5000);
            return;
        }
        if (QMessageBox::question(
                this, tr("Reassign All Photos"),
                tr("Nothing is selected. Reassign all %1 photo(s) shown here to a "
                   "different taxon?")
                    .arg(rows))
            != QMessageBox::Yes)
            return;
        for (int r = 0; r < rows; ++r)
            captureIds << m_taxonModel->index(r, 0).data(model::CaptureListModel::IdRole).toInt();
    }

    TaxonPickerDialog picker(m_app.database().connectionName(), this);
    picker.setWindowTitle(tr("Reassign %1 Photo(s) To…").arg(captureIds.size()));
    if (picker.exec() != QDialog::Accepted)
        return;
    const qint64 newTaxonInatId = picker.chosenTaxonInatId();
    if (newTaxonInatId <= 0)
        return;

    match::MatchReviewer reviewer(m_app.database().connectionName());
    int ok = 0;
    for (int captureId : std::as_const(captureIds)) {
        if (reviewer.confirm(captureId, newTaxonInatId, true))
            ++ok;
    }

    m_taxonModel->reload();
    m_model->reload();
    m_bestShotModel->reload();
    m_reviewPane->reload();
    updateEmptyState();
    refreshCoverage();
    updateReviewTabText();
    updateBestShotsTabText();

    statusBar()->showMessage(
        tr("Reassigned %1 of %2 photo(s).").arg(ok).arg(captureIds.size()), 6000);
}

void MainWindow::applyCaptionFields(int fields)
{
    m_app.settings().setCaptureCaptionFields(fields);
    m_model->setCaptionFields(fields);
    if (m_taxonModel)
        m_taxonModel->setCaptionFields(fields);
    if (m_bestShotModel)
        m_bestShotModel->setCaptionFields(fields);

    // Grow the grid cells to fit however many caption lines are now showing.
    // File Type doesn't count — it's drawn as an on-image badge, not a line.
    int lines = 0;
    for (int b = fields & ~model::CaptureListModel::CaptionFileType; b; b &= (b - 1))
        ++lines;
    lines = std::max(lines, 1);
    const QSize gridSize(212, 192 + 20 + lines * 16);
    if (m_grid)
        m_grid->setGridSize(gridSize);
    if (m_taxonGrid)
        m_taxonGrid->setGridSize(gridSize);
    if (m_bestShotGrid)
        m_bestShotGrid->setGridSize(gridSize);
}

void MainWindow::updateReviewTabText()
{
    const int n = m_reviewPane->queueCount();
    m_viewReviewAction->setText(n > 0 ? tr("&Review Unmatched (%1)").arg(n) : tr("&Review Unmatched"));
}

void MainWindow::switchToTreeTab(QWidget *page)
{
    m_viewTreeAction->setChecked(true);
    m_tabs->setCurrentWidget(page);
}

void MainWindow::updateBestShotsTabText()
{
    if (!m_bestShotModel)
        return;
    const int tab = m_tabs->indexOf(m_bestShotGrid->parentWidget());
    if (tab < 0)
        return;
    const int n = m_bestShotModel->captureCount();
    m_tabs->setTabText(tab, n > 0 ? tr("My Best Shots (%1)").arg(n) : tr("My Best Shots"));
}

void MainWindow::openViewer(QAbstractItemModel *model, const QModelIndex &clicked)
{
    if (!model || !clicked.isValid())
        return;

    QVector<ImageViewer::Item> items;
    int start = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex idx = model->index(r, 0);
        ImageViewer::Item item;
        item.path = idx.data(model::CaptureListModel::PreviewPathRole).toString();
        item.title = idx.data(model::CaptureListModel::DisplayNameRole).toString();
        item.caption = idx.data(model::CaptureListModel::FullCaptionRole).toString();
        item.captureId = idx.data(model::CaptureListModel::IdRole).toInt();
        item.canBestShot =
            !idx.data(model::CaptureListModel::MatchedNameRole).toString().isEmpty();
        item.bestShot = idx.data(model::CaptureListModel::IsBestShotRole).toBool();
        item.hasGps = idx.data(model::CaptureListModel::HasGpsRole).toBool();
        item.latitude = idx.data(model::CaptureListModel::LatitudeRole).toDouble();
        item.longitude = idx.data(model::CaptureListModel::LongitudeRole).toDouble();
        if (item.path.isEmpty())
            continue;
        if (r == clicked.row())
            start = items.size();
        items.append(item);
    }
    if (items.isEmpty())
        return;

    if (!m_viewer) {
        m_viewer = new ImageViewer(this);
        connect(m_viewer, &ImageViewer::bestShotToggleRequested, this,
                [this](qint64 captureId, bool nominate) {
                    if (captureId <= 0)
                        return;
                    catalogue::BestShotStore store(m_app.database().connectionName());
                    if (store.setBestShots({int(captureId)}, nominate) < 0) {
                        statusBar()->showMessage(
                            tr("Could not update best shots: %1").arg(store.error()), 8000);
                        return;
                    }
                    const QList<int> ids{int(captureId)};
                    m_model->applyBestShot(ids, nominate);
                    m_taxonModel->applyBestShot(ids, nominate);
                    m_bestShotModel->reload();
                    updateBestShotsTabText();
                    m_viewer->markBestShot(captureId, nominate);
                    statusBar()->showMessage(nominate
                                                 ? tr("Added to your best shots.")
                                                 : tr("Removed from your best shots."),
                                             4000);
                });
    }
    // Show before loading the image: on a freshly constructed dialog that has
    // never been shown, resize() in the constructor only records the desired
    // top-level size -- children (including the scroll area's viewport) aren't
    // actually laid out to it until the widget is shown. Calling setItems()
    // first would make its rescale() read the viewport's stale, tiny
    // pre-layout size, scaling the first photo down to a thumbnail that stays
    // wrong until the viewer is reopened.
    m_viewer->show();
    m_viewer->raise();
    m_viewer->activateWindow();
    m_viewer->setItems(items, start);
}

void MainWindow::refreshReferenceTree()
{
    if (m_builder->isRunning())
        return;
    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Refresh Reference Tree"),
                                tr("Select a reference tree first."));
        return;
    }

    QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
    q.prepare(QStringLiteral(
        "SELECT p.name, rt.name, rt.rank, pl.name "
        "FROM project p "
        "LEFT JOIN taxon rt ON rt.inat_id = p.root_taxon_inat_id "
        "LEFT JOIN place pl ON pl.inat_id = p.place_inat_id WHERE p.id = ?"));
    q.addBindValue(pid);
    if (!q.exec() || !q.next() || q.value(1).isNull()) {
        QMessageBox::information(this, tr("Refresh Reference Tree"),
                                tr("This tree can't be refreshed automatically."));
        return;
    }

    taxonomy::ProjectBuilder::Request request;
    request.projectName = q.value(0).toString();
    request.taxonQuery = q.value(1).toString();
    request.rank = q.value(2).toString();
    request.placeQuery = q.value(3).toString();

    m_refreshTreeAction->setEnabled(false);
    statusBar()->showMessage(tr("Refreshing reference tree…"));
    m_builder->start(request);
}

void MainWindow::deleteReferenceTree()
{
    if (m_builder->isRunning())
        return;
    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Delete Reference Tree"),
                                tr("Select a reference tree first."));
        return;
    }

    const QString name = m_projectCombo->currentText();
    const auto reply = QMessageBox::question(
        this, tr("Delete Reference Tree"),
        tr("Delete \"%1\"? This removes the tree and its coverage tracking. "
           "Your photos and any matches already made to its taxa are not affected.")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    taxonomy::TaxonomyStore store(m_app.database().connectionName());
    if (!store.deleteProject(pid)) {
        QMessageBox::warning(this, tr("Delete Reference Tree"),
                             tr("Could not delete \"%1\".").arg(name));
        return;
    }

    reloadProjectList();
    statusBar()->showMessage(tr("Deleted \"%1\".").arg(name), 6000);
}

void MainWindow::fetchInfraspecificTaxa()
{
    startInfraspecificFetch(0, QString());   // whole tree
}

void MainWindow::startInfraspecificFetch(qint64 scopeInatId, const QString &scopeName)
{
    if (m_infraFiller->isRunning())
        return;
    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Fetch Subspecies/Varieties"),
                                tr("Select a reference tree first."));
        return;
    }

    const bool scoped = scopeInatId > 0;
    const QString where = scoped && !scopeName.isEmpty() ? scopeName
                                                         : m_projectCombo->currentText();

    taxonomy::TaxonomyStore store(m_app.database().connectionName());
    const int pendingCount =
        int(store.projectSpeciesNeedingInfraCheck(pid, scopeInatId).size());
    if (pendingCount == 0) {
        QMessageBox::information(
            this, tr("Fetch Subspecies/Varieties"),
            scoped ? tr("No species to check under \"%1\".").arg(where)
                   : tr("Every species in \"%1\" has already been checked.").arg(where));
        return;
    }

    const auto reply = QMessageBox::question(
        this, tr("Fetch Subspecies/Varieties"),
        tr("Check %1 species %2 \"%3\" for iNaturalist subspecies, varieties and hybrids? "
           "This makes a few requests per species, so it can be slow for a large branch. "
           "It's safe to quit and run again later.")
            .arg(pendingCount)
            .arg(scoped ? tr("under") : tr("in"))
            .arg(where),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    m_fetchInfraAction->setEnabled(false);
    statusBar()->showMessage(tr("Checking species for subspecies/varieties…"));
    m_infraFiller->start(pid, scopeInatId);
}

void MainWindow::showTreeContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_treeView->indexAt(pos);
    if (!idx.isValid())
        return;

    const qint64 inatId = idx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong();
    if (inatId <= 0)
        return;
    const QString name = idx.data(model::TaxonomyTreeModel::NameRole).toString();

    // Right-click acts on the clicked row, so make it the selection too — the
    // coverage panel and the photo grids follow along.
    m_treeView->setCurrentIndex(idx.sibling(idx.row(), 0));

    QMenu menu(this);

    QAction *infra = menu.addAction(tr("Fetch Subspecies/Varieties under \"%1\"…").arg(name));
    infra->setEnabled(!m_infraFiller->isRunning() && currentProjectId() > 0);
    connect(infra, &QAction::triggered, this,
            [this, inatId, name] { startInfraspecificFetch(inatId, name); });

    menu.addSeparator();

    QAction *showPhotos = menu.addAction(tr("Show Photos of This Taxon"));
    connect(showPhotos, &QAction::triggered, this, [this, inatId] {
        selectTaxonInTree(inatId);
        switchToTreeTab(m_tabs->widget(0));   // Photos of Tree Selection
    });

    QAction *openInat = menu.addAction(tr("Open on iNaturalist"));
    connect(openInat, &QAction::triggered, this, [inatId] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://www.inaturalist.org/taxa/%1").arg(inatId)));
    });

    menu.exec(m_treeView->viewport()->mapToGlobal(pos));
}

void MainWindow::showMissingListContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_missingList->itemAt(pos);
    if (!item)
        return;
    const qint64 inatId = item->data(Qt::UserRole).toLongLong();
    if (inatId <= 0)
        return;

    QMenu menu(this);
    QAction *viewPhoto = menu.addAction(tr("View Reference Photo"));
    connect(viewPhoto, &QAction::triggered, this, [this, inatId] { viewReferencePhoto(inatId); });
    menu.exec(m_missingList->viewport()->mapToGlobal(pos));
}

void MainWindow::viewReferencePhoto(qint64 inatId)
{
    const auto photo = m_app.taxonomyStore().taxonPhoto(inatId);

    if (!m_referencePhotoDialog)
        m_referencePhotoDialog = new ReferencePhotoDialog(m_app.photoCache(), this);
    // Show before loading the photo: on a freshly constructed, never-shown
    // dialog, showTaxon()'s rescale() would read the image label's stale
    // pre-layout size (see the same fix in openViewer() for ImageViewer).
    m_referencePhotoDialog->show();
    m_referencePhotoDialog->raise();
    m_referencePhotoDialog->activateWindow();
    m_referencePhotoDialog->showTaxon(inatId, photo.name, photo.commonName, photo.photoUrl,
                                      photo.attribution);
}

void MainWindow::importChecklist()
{
    if (m_checklistImporter->isRunning())
        return;
    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Import Checklist"),
                                tr("Select or create a reference tree to import into first."));
        return;
    }

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Checklist CSV"), QDir::homePath(),
        tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Checklist"), tr("Could not read %1.").arg(path));
        return;
    }
    const auto entries = checklist::parseChecklistCsv(file.readAll());
    if (entries.isEmpty()) {
        QMessageBox::information(this, tr("Import Checklist"),
                                tr("No usable rows found in %1.").arg(QFileInfo(path).fileName()));
        return;
    }

    const QString source = QInputDialog::getText(
        this, tr("Import Checklist"), tr("Status source label:"), QLineEdit::Normal,
        QFileInfo(path).completeBaseName());

    checklist::ChecklistImporter::Request request;
    request.projectId = pid;
    request.source = source;
    request.entries = entries;

    QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
    q.prepare(QStringLiteral("SELECT place_inat_id FROM project WHERE id = ?"));
    q.addBindValue(pid);
    if (q.exec() && q.next() && !q.value(0).isNull())
        request.placeInatId = q.value(0).toLongLong();

    m_importChecklistAction->setEnabled(false);
    statusBar()->showMessage(tr("Importing %1 checklist rows…").arg(entries.size()));
    m_checklistImporter->start(request);
}

void MainWindow::reloadProjectList()
{
    const int previous = m_projectCombo->currentData().isValid()
                             ? m_projectCombo->currentData().toInt()
                             : -1;

    QSignalBlocker block(m_projectCombo);
    m_projectCombo->clear();

    if (m_app.database().isOpen()) {
        QSqlQuery q(QSqlDatabase::database(m_app.database().connectionName(), false));
        q.exec(QStringLiteral("SELECT id, name FROM project ORDER BY name"));
        while (q.next())
            m_projectCombo->addItem(q.value(1).toString(), q.value(0).toInt());
    }

    if (m_projectCombo->count() == 0) {
        m_treeModel->setProject(-1);
        refreshCoverage();
        return;
    }

    const int restore = m_projectCombo->findData(previous);
    m_projectCombo->setCurrentIndex(restore >= 0 ? restore : 0);
    m_treeModel->setProject(m_projectCombo->currentData().toInt());
    m_treeView->expandToDepth(1);
    refreshCoverage();
}

void MainWindow::startMatch()
{
    if (m_app.matchService().isRunning())
        return;
    if (m_app.scanService().isRunning()) {
        QMessageBox::information(this, tr("Match Library"),
                                tr("Wait for the current scan to finish first."));
        return;
    }
    if (m_model->captureCount() == 0) {
        QMessageBox::information(this, tr("Match Library"),
                                tr("Add and scan a folder of photos first."));
        return;
    }
    m_app.matchService().start();
}

void MainWindow::newReferenceTree()
{
    if (m_builder->isRunning())
        return;

    NewProjectDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    taxonomy::ProjectBuilder::Request request = dialog.request();

    if (!request.placeQuery.trimmed().isEmpty()) {
        PlaceConfirmDialog placeDialog(m_app.inat(), request.placeQuery, this);
        if (placeDialog.exec() != QDialog::Accepted)
            return;
        request.confirmedPlace = placeDialog.chosenPlace();
    }

    TaxonConfirmDialog taxonDialog(m_app.inat(), request.taxonQuery, request.rank, this);
    if (taxonDialog.exec() != QDialog::Accepted)
        return;
    request.confirmedTaxon = taxonDialog.chosenTaxon();

    m_newTreeAction->setEnabled(false);
    m_builderIsNewTree = true;
    statusBar()->showMessage(tr("Building reference tree…"));
    m_builder->start(request);
}

namespace {

// Opens the platform file manager with `path` pre-selected, where the
// platform/desktop supports it. Falls through a few common Linux file
// managers' own "select" flags before giving up and just opening the
// containing folder.
void revealInFileManager(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
#if defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#elif defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
    if (QProcess::startDetached(QStringLiteral("nautilus"), {QStringLiteral("--select"), path}))
        return;
    if (QProcess::startDetached(QStringLiteral("dolphin"), {QStringLiteral("--select"), path}))
        return;
    if (QProcess::startDetached(QStringLiteral("nemo"), {path}))
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

} // namespace

QListView *MainWindow::makeCaptureGrid(QAbstractItemModel *model)
{
    auto *grid = new QListView(this);
    grid->setModel(model);
    grid->setViewMode(QListView::IconMode);
    grid->setResizeMode(QListView::Adjust);
    grid->setMovement(QListView::Static);
    grid->setUniformItemSizes(true);
    grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    grid->setEditTriggers(QAbstractItemView::NoEditTriggers);   // read-only grid; no renaming
    grid->setIconSize(QSize(192, 192));
    grid->setGridSize(QSize(212, 232));
    grid->setWordWrap(true);
    grid->setSpacing(6);

    grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid, &QWidget::customContextMenuRequested, this, [this, grid](const QPoint &pos) {
        const QModelIndex idx = grid->indexAt(pos);
        if (!idx.isValid())
            return;
        // Right-click doesn't extend a selection: if the clicked photo isn't
        // already selected, act on it alone.
        if (!grid->selectionModel()->isSelected(idx))
            grid->setCurrentIndex(idx);

        const QModelIndexList sel = grid->selectionModel()->selectedIndexes();
        const int selected = sel.size();
        const QString path = idx.data(model::CaptureListModel::PreviewPathRole).toString();

        int nBest = 0, nMatched = 0;
        for (const QModelIndex &s : sel) {
            if (s.data(model::CaptureListModel::IsBestShotRole).toBool())
                ++nBest;
            if (!s.data(model::CaptureListModel::MatchedNameRole).toString().isEmpty())
                ++nMatched;
        }

        QMenu menu(grid);
        QAction *reveal = menu.addAction(tr("Open in File Manager"));
        reveal->setEnabled(!path.isEmpty());
        connect(reveal, &QAction::triggered, this, [path] { revealInFileManager(path); });

        menu.addSeparator();
        QAction *addBest = menu.addAction(
            selected == 1 && nMatched == 1
                ? tr("Add to Best Shots of %1")
                      .arg(idx.data(model::CaptureListModel::MatchedNameRole).toString())
                : tr("Add to Best Shot of Species"));
        addBest->setEnabled(nMatched > nBest);
        connect(addBest, &QAction::triggered, this,
                [this, grid] { setBestShotForSelection(grid, true); });

        QAction *removeBest = menu.addAction(tr("Remove from Best Shot of Species"));
        removeBest->setEnabled(nBest > 0);
        connect(removeBest, &QAction::triggered, this,
                [this, grid] { setBestShotForSelection(grid, false); });

        menu.addSeparator();
        QAction *remove = menu.addAction(
            selected > 1 ? tr("Remove %1 Photos from PhotoLife…").arg(selected)
                         : tr("Remove Photo from PhotoLife…"));
        connect(remove, &QAction::triggered, this, [this, grid] { removeSelectedCaptures(grid); });

        menu.exec(grid->viewport()->mapToGlobal(pos));
    });

    return grid;
}

void MainWindow::setBestShotForSelection(QListView *grid, bool nominate)
{
    QList<int> ids;
    int skipped = 0;
    const QModelIndexList selected = grid->selectionModel()->selectedIndexes();
    for (const QModelIndex &idx : selected) {
        const int id = idx.data(model::CaptureListModel::IdRole).toInt();
        if (id <= 0 || ids.contains(id))
            continue;
        if (nominate
            && idx.data(model::CaptureListModel::MatchedNameRole).toString().isEmpty()) {
            ++skipped;
            continue;
        }
        ids.append(id);
    }
    if (ids.isEmpty()) {
        if (skipped > 0)
            statusBar()->showMessage(
                tr("Identify a photo before adding it to your best shots."), 6000);
        return;
    }

    catalogue::BestShotStore store(m_app.database().connectionName());
    const int changed = store.setBestShots(ids, nominate);
    if (changed < 0) {
        statusBar()->showMessage(
            tr("Could not update best shots: %1").arg(store.error()), 8000);
        return;
    }

    m_model->applyBestShot(ids, nominate);
    m_taxonModel->applyBestShot(ids, nominate);
    m_bestShotModel->reload();
    updateBestShotsTabText();

    if (nominate) {
        QString msg = tr("Added %n photo(s) to your best shots.", nullptr, int(ids.size()));
        if (skipped > 0)
            msg += QLatin1Char(' ')
                   + tr("%n unidentified photo(s) skipped.", nullptr, skipped);
        statusBar()->showMessage(msg, 6000);
    } else {
        statusBar()->showMessage(
            tr("Removed %n photo(s) from your best shots.", nullptr, int(ids.size())), 6000);
    }
}

void MainWindow::removeSelectedCaptures(QListView *grid)
{
    QList<int> ids;
    const QModelIndexList selected = grid->selectionModel()->selectedIndexes();
    for (const QModelIndex &idx : selected) {
        const int id = idx.data(model::CaptureListModel::IdRole).toInt();
        if (id > 0 && !ids.contains(id))
            ids.append(id);
    }
    if (ids.isEmpty())
        return;

    const auto reply = QMessageBox::question(
        this, tr("Remove from PhotoLife"),
        tr("Remove %n photo(s) from PhotoLife's catalogue?\n\n"
           "No files are deleted from your drive — this only drops the catalogue "
           "entry, its thumbnail and any match. A rescan re-imports photos whose "
           "files are still present.",
           nullptr, int(ids.size())),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    scan::CatalogueMaintenance maint(m_app.database().connectionName());
    const int removed = maint.forgetCaptures(ids);
    if (removed <= 0) {
        statusBar()->showMessage(
            tr("Could not remove the selected photos: %1").arg(maint.error()), 8000);
        return;
    }

    m_model->reload();
    m_taxonModel->reload();
    m_bestShotModel->reload();
    m_reviewPane->reload();
    updateEmptyState();
    refreshCoverage();
    updateReviewTabText();
    updateBestShotsTabText();
    statusBar()->showMessage(
        tr("Removed %n photo(s) from the catalogue.", nullptr, removed), 6000);
}

void MainWindow::removeMissingCaptures()
{
    if (m_app.scanService().isRunning()) {
        QMessageBox::information(this, tr("Remove Missing Photos"),
                                tr("Wait for the current scan to finish first."));
        return;
    }

    scan::CatalogueMaintenance maint(m_app.database().connectionName());

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QList<scan::CatalogueMaintenance::MissingCapture> missing =
        maint.capturesWithMissingFiles();
    QApplication::restoreOverrideCursor();

    if (missing.isEmpty()) {
        QMessageBox::information(
            this, tr("Remove Missing Photos"),
            tr("Every catalogued photo's file is still on your drive."));
        return;
    }

    // Show exactly which entries would go; every one is ticked, and the user can
    // untick any they want to keep.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Remove Missing Photos"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *intro = new QLabel(
        tr("%n catalogued photo(s) point at a file that is no longer on your drive "
           "— usually left behind when a file was renamed or moved.\n\n"
           "Ticked entries will be removed from PhotoLife's catalogue. No files are "
           "deleted from your drive.",
           nullptr, int(missing.size())),
        &dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *list = new QListWidget(&dlg);
    list->setAlternatingRowColors(true);
    for (const auto &mc : missing) {
        const QString what = mc.missingPaths.isEmpty()
                                 ? QDir(mc.folderPath).filePath(mc.baseName)
                                 : mc.missingPaths.join(QStringLiteral("   +   "));
        auto *item = new QListWidgetItem(what, list);
        item->setToolTip(what);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(Qt::UserRole, mc.captureId);
    }
    layout->addWidget(list, 1);

    auto *buttons = new QHBoxLayout;
    auto *checkAll = new QPushButton(tr("Tick All"), &dlg);
    auto *checkNone = new QPushButton(tr("Untick All"), &dlg);
    connect(checkAll, &QPushButton::clicked, &dlg, [list] {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setCheckState(Qt::Checked);
    });
    connect(checkNone, &QPushButton::clicked, &dlg, [list] {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setCheckState(Qt::Unchecked);
    });
    buttons->addWidget(checkAll);
    buttons->addWidget(checkNone);
    buttons->addStretch(1);

    auto *box = new QDialogButtonBox(&dlg);
    QPushButton *removeButton =
        box->addButton(tr("Remove Ticked"), QDialogButtonBox::AcceptRole);
    box->addButton(QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto updateRemoveButton = [list, removeButton] {
        int ticked = 0;
        for (int i = 0; i < list->count(); ++i)
            ticked += list->item(i)->checkState() == Qt::Checked ? 1 : 0;
        removeButton->setEnabled(ticked > 0);
        removeButton->setText(tr("Remove %n Ticked", nullptr, ticked));
    };
    connect(list, &QListWidget::itemChanged, &dlg, updateRemoveButton);
    updateRemoveButton();
    buttons->addWidget(box);
    layout->addLayout(buttons);

    dlg.resize(720, 420);
    if (dlg.exec() != QDialog::Accepted)
        return;

    QList<int> ids;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() == Qt::Checked)
            ids.append(list->item(i)->data(Qt::UserRole).toInt());
    }
    if (ids.isEmpty())
        return;

    const int removed = maint.forgetCaptures(ids);
    if (removed <= 0) {
        statusBar()->showMessage(
            tr("Could not remove the missing photos: %1").arg(maint.error()), 8000);
        return;
    }

    m_model->reload();
    m_taxonModel->reload();
    m_bestShotModel->reload();
    m_reviewPane->reload();
    updateEmptyState();
    refreshCoverage();
    updateReviewTabText();
    updateBestShotsTabText();
    statusBar()->showMessage(
        tr("Removed %n photo(s) whose files were missing.", nullptr, removed), 6000);
}

void MainWindow::fixRawGeolocation()
{
    if (m_app.scanService().isRunning()) {
        QMessageBox::information(this, tr("Fix RAW Photo Locations"),
                                 tr("Wait for the current scan to finish first."));
        return;
    }
    if (QMessageBox::question(
            this, tr("Fix RAW Photo Locations"),
            tr("Some RAW photos may have an incorrect GPS location due to a bug that has "
               "since been fixed (their hemisphere sign was sometimes lost, e.g. a Victoria "
               "photo ending up near Japan). This re-reads location data from each RAW file "
               "directly and corrects it in the catalogue. Continue?"))
        != QMessageBox::Yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    scan::CatalogueMaintenance maint(m_app.database().connectionName());
    const int updated = maint.backfillRawGeolocation();
    QApplication::restoreOverrideCursor();

    if (!maint.error().isEmpty()) {
        statusBar()->showMessage(
            tr("Fix RAW Photo Locations failed: %1").arg(maint.error()), 8000);
        return;
    }

    m_model->reload();
    m_taxonModel->reload();
    m_bestShotModel->reload();
    statusBar()->showMessage(
        tr("Updated GPS location for %n RAW photo(s).", nullptr, updated), 6000);
}

void MainWindow::fetchPhotoLocalities()
{
    if (m_localityFetcher->isRunning())
        return;

    m_fetchLocalitiesAction->setEnabled(false);
    statusBar()->showMessage(tr("Fetching photo localities from OpenStreetMap…"));
    m_localityFetcher->start();
}

void MainWindow::buildCentralWidget()
{
    m_grid = makeCaptureGrid(m_model);
    m_grid->setItemDelegate(new CaptureDelegate(m_grid));
    connect(m_grid, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &i) { openViewer(m_model, i); });

    m_emptyHint = new QLabel(this);
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setEnabled(false);

    m_photoStack = new QStackedWidget(this);
    m_photoStack->addWidget(m_emptyHint);   // index 0
    m_photoStack->addWidget(m_grid);        // index 1

    m_reviewPane = new ReviewPane(m_app.database(), m_app.thumbnails(), m_app.inat(),
                                  m_app.photoCache(), this);
    connect(m_reviewPane, &ReviewPane::queueChanged, this, [this] {
        m_model->reload();
        if (m_bestShotModel)
            m_bestShotModel->reload();
        updateEmptyState();
        refreshCoverage();
        updateReviewTabText();
        updateBestShotsTabText();
    });

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildBrowsePage(), tr("Photos of Tree Selection"));
    m_tabs->addTab(buildMapPage(), tr("Map"));
    m_tabs->addTab(buildMissingPage(), tr("Missing Species"));
    m_tabs->addTab(buildReferencePhotosPage(), tr("Reference Photos"));
    m_tabs->addTab(buildInatDownloadPage(), tr("Download from iNaturalist"));
    m_tabs->addTab(buildBestShotsPage(), tr("My Best Shots"));

    m_centralStack = new QStackedWidget(this);
    m_centralStack->addWidget(m_tabs);        // Reference Tree mode
    m_centralStack->addWidget(m_photoStack);  // All Library Photos mode
    m_centralStack->addWidget(m_reviewPane);  // Review Unmatched mode
    setCentralWidget(m_centralStack);

    connect(m_viewTreeAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_centralStack->setCurrentWidget(m_tabs);
    });
    connect(m_viewLibraryAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_centralStack->setCurrentWidget(m_photoStack);
    });
    connect(m_viewReviewAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_centralStack->setCurrentWidget(m_reviewPane);
    });
    m_viewLibraryAction->setChecked(true);   // start on All Photos

    updateReviewTabText();
    updateBestShotsTabText();
    updateReferencePhotoStatus();

    // Filling in missing reference photos is a network round-trip, so only do
    // it when the user actually opens the tab.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int i) {
        if (m_refPhotoPage && m_tabs->widget(i) == m_refPhotoPage)
            maybeFetchReferencePhotos();
    });

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel);
}

QWidget *MainWindow::buildBrowsePage()
{
    m_taxonModel = new model::CaptureListModel(m_app.database(), m_app.thumbnails(), this);

    m_taxonRepImage = new QLabel(this);
    m_taxonRepImage->setFixedSize(120, 120);
    m_taxonRepImage->setAlignment(Qt::AlignCenter);
    m_taxonRepImage->setFrameShape(QFrame::StyledPanel);

    m_taxonInfo = new QLabel(this);
    m_taxonInfo->setTextFormat(Qt::RichText);
    m_taxonInfo->setWordWrap(true);
    m_taxonInfo->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    auto *reassignButton = new QPushButton(tr("Reassign to Taxon…"), this);
    reassignButton->setToolTip(
        tr("Move the selected photos (or, with nothing selected, every photo shown here) "
           "to a different taxon."));
    connect(reassignButton, &QPushButton::clicked, this, &MainWindow::reassignTaxonPhotos);

    auto *header = new QHBoxLayout;
    header->addWidget(m_taxonRepImage);
    header->addWidget(m_taxonInfo, 1);
    header->addWidget(reassignButton, 0, Qt::AlignTop);

    m_taxonGrid = makeCaptureGrid(m_taxonModel);
    m_taxonGrid->setItemDelegate(new CaptureDelegate(m_taxonGrid));
    connect(m_taxonGrid, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &i) { openViewer(m_taxonModel, i); });

    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addLayout(header);
    layout->addWidget(m_taxonGrid, 1);
    return page;
}

QWidget *MainWindow::buildMapPage()
{
    m_mapView = new MapView(*m_taxonModel, m_app.tileCache(), this);
    connect(m_mapView, &MapView::captureActivated, this,
            [this](const QModelIndex &i) { openViewer(m_taxonModel, i); });
    return m_mapView;
}

QWidget *MainWindow::buildBestShotsPage()
{
    m_bestShotModel = new model::CaptureListModel(m_app.database(), m_app.thumbnails(), this);
    m_bestShotModel->setBestShotOnly(true);

    m_bestShotHeader = new QLabel(this);
    m_bestShotHeader->setWordWrap(true);
    m_bestShotHeader->setText(
        tr("Photos you've starred as a best shot of their species. Right-click a "
           "photo in any grid to add or remove it."));

    m_bestShotGrid = makeCaptureGrid(m_bestShotModel);
    m_bestShotGrid->setItemDelegate(new CaptureDelegate(m_bestShotGrid));
    connect(m_bestShotGrid, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &i) { openViewer(m_bestShotModel, i); });

    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_bestShotHeader);
    layout->addWidget(m_bestShotGrid, 1);
    return page;
}

QWidget *MainWindow::buildMissingPage()
{
    m_missingSearch = new QLineEdit(this);
    m_missingSearch->setPlaceholderText(tr("Filter by common or scientific name…"));
    m_missingSearch->setClearButtonEnabled(true);
    connect(m_missingSearch, &QLineEdit::textChanged, this, &MainWindow::updateMissingList);

    m_missingList = new QListWidget(this);
    m_missingList->setAlternatingRowColors(true);
    connect(m_missingList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        selectTaxonInTree(item->data(Qt::UserRole).toLongLong());
        switchToTreeTab(m_tabs->widget(0));   // Browse
    });
    m_missingList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_missingList, &QWidget::customContextMenuRequested, this,
            &MainWindow::showMissingListContextMenu);

    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_missingSearch);
    layout->addWidget(m_missingList, 1);
    return page;
}

QWidget *MainWindow::buildReferencePhotosPage()
{
    m_refPhotoModel = new model::ReferencePhotoModel(m_app.database(), m_app.photoCache(), this);

    m_refPhotoStatus = new QLabel(this);
    m_refPhotoStatus->setWordWrap(true);

    auto *fetchButton = new QPushButton(tr("Fetch Reference Photos"), this);
    fetchButton->setToolTip(
        tr("Download a representative photo for each species in this reference tree "
           "from iNaturalist."));
    connect(fetchButton, &QPushButton::clicked, this, &MainWindow::fetchReferencePhotos);

    auto *header = new QHBoxLayout;
    header->addWidget(m_refPhotoStatus, 1);
    header->addWidget(fetchButton, 0, Qt::AlignTop);

    m_refPhotoGrid = new QListView(this);
    m_refPhotoGrid->setModel(m_refPhotoModel);
    m_refPhotoGrid->setViewMode(QListView::IconMode);
    m_refPhotoGrid->setResizeMode(QListView::Adjust);
    m_refPhotoGrid->setMovement(QListView::Static);
    m_refPhotoGrid->setUniformItemSizes(true);
    m_refPhotoGrid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_refPhotoGrid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_refPhotoGrid->setIconSize(QSize(160, 160));
    m_refPhotoGrid->setGridSize(QSize(188, 220));
    m_refPhotoGrid->setWordWrap(true);
    m_refPhotoGrid->setSpacing(6);
    connect(m_refPhotoGrid, &QAbstractItemView::doubleClicked, this, [](const QModelIndex &idx) {
        const qint64 inatId = idx.data(model::ReferencePhotoModel::InatIdRole).toLongLong();
        if (inatId > 0)
            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://www.inaturalist.org/taxa/%1").arg(inatId)));
    });
    connect(m_refPhotoModel, &QAbstractItemModel::modelReset, this,
            &MainWindow::updateReferencePhotoStatus);

    m_refPhotoPage = new QWidget(this);
    auto *layout = new QVBoxLayout(m_refPhotoPage);
    layout->addLayout(header);
    layout->addWidget(m_refPhotoGrid, 1);
    return m_refPhotoPage;
}

void MainWindow::updateReferencePhotoStatus()
{
    if (!m_refPhotoStatus || !m_refPhotoModel)
        return;

    const int total = m_refPhotoModel->speciesCount();
    const int withPhoto = m_refPhotoModel->withPhotoCount();
    if (total == 0) {
        m_refPhotoStatus->setText(
            tr("Select a reference tree — or a taxon within it — to see its species' "
               "iNaturalist reference photos."));
    } else {
        m_refPhotoStatus->setText(
            tr("%1 of %2 species have a reference photo from iNaturalist. "
               "Double-click a photo to open the taxon on iNaturalist.")
                .arg(withPhoto)
                .arg(total));
    }

    const int tab = m_tabs->indexOf(m_refPhotoPage);
    if (tab >= 0)
        m_tabs->setTabText(tab, total > 0 ? tr("Reference Photos (%1)").arg(total)
                                          : tr("Reference Photos"));
}

void MainWindow::maybeFetchReferencePhotos()
{
    const int pid = currentProjectId();
    if (pid <= 0 || !m_refPhotoFetcher || m_refPhotoFetcher->isRunning())
        return;
    if (m_app.taxonomyStore().projectLeafTaxaMissingPhoto(pid).isEmpty())
        return;

    if (m_fetchRefPhotosAction)
        m_fetchRefPhotosAction->setEnabled(false);
    statusBar()->showMessage(tr("Fetching reference photos from iNaturalist…"));
    m_refPhotoFetcher->start(pid);
}

void MainWindow::fetchReferencePhotos()
{
    if (m_refPhotoFetcher && m_refPhotoFetcher->isRunning())
        return;

    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Fetch Reference Photos"),
                                tr("Select a reference tree first."));
        return;
    }
    if (m_app.taxonomyStore().projectLeafTaxaMissingPhoto(pid).isEmpty()) {
        QMessageBox::information(
            this, tr("Fetch Reference Photos"),
            tr("Every species in \"%1\" already has a reference photo.")
                .arg(m_projectCombo->currentText()));
        return;
    }

    switchToTreeTab(m_refPhotoPage);
    if (m_fetchRefPhotosAction)
        m_fetchRefPhotosAction->setEnabled(false);
    statusBar()->showMessage(tr("Fetching reference photos from iNaturalist…"));
    m_refPhotoFetcher->start(pid);
}

QWidget *MainWindow::buildInatDownloadPage()
{
    m_inatDownloadModel = new inat::InatDownloadModel(m_app.photoCache(), m_app.taxonomyStore(), this);
    m_inatProxyModel = new InatFilterProxyModel(this);
    m_inatProxyModel->setSourceModel(m_inatDownloadModel);
    m_inatProxyModel->setFilterRole(inat::InatDownloadModel::NameRole);
    m_inatProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_inatProxyModel->setDynamicSortFilter(true);
    m_inatProxyModel->setSortRole(inat::InatDownloadModel::NameRole);

    m_inatUsernameEdit = new QLineEdit(this);
    m_inatUsernameEdit->setPlaceholderText(tr("iNaturalist username"));
    m_inatUsernameEdit->setText(m_app.settings().inatUsername());

    m_inatTokenEdit = new QLineEdit(this);
    m_inatTokenEdit->setPlaceholderText(tr("Personal API token (optional)"));
    m_inatTokenEdit->setText(m_app.settings().inatApiToken());
    m_inatTokenEdit->setEchoMode(QLineEdit::Password);
    m_inatTokenEdit->setToolTip(
        tr("Optional. Without one, geoprivacy-obscured species (common for rare orchids) come "
           "back with fuzzed coordinates even for your own observations. Get a token from "
           "inaturalist.org/users/api_token while logged in — it expires after about 24 hours, "
           "so you may need to paste a fresh one each session."));

    m_inatRestrictToLocality = new QCheckBox(tr("Restrict to this tree's locality"), this);
    m_inatRestrictToLocality->setChecked(true);
    m_inatRestrictToLocality->setToolTip(
        tr("When checked, only observations within this reference tree's own locality count "
           "— a species that also occurs elsewhere won't pull in observations from outside "
           "the tree's region. Trees with no locality set search worldwide either way."));

    m_inatSearchButton = new QPushButton(tr("Search iNaturalist"), this);
    connect(m_inatSearchButton, &QPushButton::clicked, this, &MainWindow::searchInatObservations);

    m_inatCancelSearchButton = new QPushButton(tr("Stop"), this);
    m_inatCancelSearchButton->setEnabled(false);
    m_inatCancelSearchButton->setToolTip(
        tr("A search covers every species in this tree one at a time; a prolific "
           "observer or a broad tree can mean a lot of iNaturalist pages to page "
           "through. Stop if it's taking too long."));
    connect(m_inatCancelSearchButton, &QPushButton::clicked, this,
            [this] { m_inatObservationFetcher->cancel(); });

    auto *header = new QHBoxLayout;
    header->addWidget(m_inatUsernameEdit, 1);
    header->addWidget(m_inatTokenEdit, 1);
    header->addWidget(m_inatRestrictToLocality);
    header->addWidget(m_inatSearchButton);
    header->addWidget(m_inatCancelSearchButton);

    m_inatFilterEdit = new QLineEdit(this);
    m_inatFilterEdit->setPlaceholderText(tr("Filter by species name…"));
    m_inatFilterEdit->setClearButtonEnabled(true);
    connect(m_inatFilterEdit, &QLineEdit::textChanged, this,
            [this](const QString &text) { m_inatProxyModel->setFilterFixedString(text); });

    m_inatSortCombo = new QComboBox(this);
    m_inatSortCombo->addItem(tr("Name (A → Z)"), int(Qt::AscendingOrder));
    m_inatSortCombo->addItem(tr("Name (Z → A)"), int(Qt::DescendingOrder));
    connect(m_inatSortCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_inatProxyModel->sort(0, Qt::SortOrder(m_inatSortCombo->itemData(i).toInt()));
    });

    m_inatHideFaded = new QCheckBox(tr("Hide faded photos"), this);
    m_inatHideFaded->setToolTip(
        tr("Hides candidates that look like something already in your library."));
    connect(m_inatHideFaded, &QCheckBox::toggled, this,
            [this](bool on) { m_inatProxyModel->setHideFaded(on); });

    auto *browseRow = new QHBoxLayout;
    browseRow->addWidget(m_inatFilterEdit, 1);
    browseRow->addWidget(m_inatSortCombo);
    browseRow->addWidget(m_inatHideFaded);

    m_inatStatus = new QLabel(this);
    m_inatStatus->setWordWrap(true);
    m_inatStatus->setText(
        tr("Enter your iNaturalist username and search to find your own observations of "
           "species in this reference tree that aren't in your library yet."));

    m_inatGrid = new QListView(this);
    m_inatGrid->setModel(m_inatProxyModel);
    m_inatGrid->setViewMode(QListView::IconMode);
    m_inatGrid->setResizeMode(QListView::Adjust);
    m_inatGrid->setMovement(QListView::Static);
    m_inatGrid->setUniformItemSizes(true);
    m_inatGrid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_inatGrid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_inatGrid->setIconSize(QSize(192, 192));
    m_inatGrid->setGridSize(QSize(212, 232));
    m_inatGrid->setWordWrap(true);
    m_inatGrid->setSpacing(6);
    m_inatGrid->setItemDelegate(new InatCandidateDelegate(m_inatGrid));

    auto *selectAllButton = new QPushButton(tr("Select All"), this);
    selectAllButton->setToolTip(
        tr("Selects every photo currently shown except the faded ones — those look like "
           "something already in your library. Click a faded photo directly if you want it "
           "included anyway."));
    connect(selectAllButton, &QPushButton::clicked, this, [this] {
        QItemSelection selection;
        for (int r = 0; r < m_inatProxyModel->rowCount(); ++r) {
            const QModelIndex idx = m_inatProxyModel->index(r, 0);
            if (!idx.data(inat::InatDownloadModel::LikelyDuplicateRole).toBool())
                selection.select(idx, idx);
        }
        m_inatGrid->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    });

    m_inatDownloadButton = new QPushButton(tr("Download Selected…"), this);
    connect(m_inatDownloadButton, &QPushButton::clicked, this,
            &MainWindow::downloadSelectedInatPhotos);

    auto *footer = new QHBoxLayout;
    footer->addWidget(m_inatStatus, 1);
    footer->addWidget(selectAllButton);
    footer->addWidget(m_inatDownloadButton);

    connect(m_inatObservationFetcher, &inat::InatObservationFetcher::progress, this,
            [this](int done, int total, int found) {
                statusBar()->showMessage(
                    tr("Searching iNaturalist — %1 / %2 taxa checked, %3 observation(s) found")
                        .arg(done)
                        .arg(total)
                        .arg(found));
            });
    connect(m_inatObservationFetcher, &inat::InatObservationFetcher::finished, this,
            &MainWindow::onInatSearchFinished);

    connect(m_inatImportService, &inat::InatImportService::progress, this,
            [this](int done, int total) {
                statusBar()->showMessage(tr("Downloading photos — %1 / %2").arg(done).arg(total));
            });
    connect(m_inatImportService, &inat::InatImportService::finished, this,
            &MainWindow::onInatImportFinished);

    m_inatDownloadPage = new QWidget(this);
    auto *layout = new QVBoxLayout(m_inatDownloadPage);
    layout->addLayout(header);
    layout->addLayout(browseRow);
    layout->addWidget(m_inatGrid, 1);
    layout->addLayout(footer);
    return m_inatDownloadPage;
}

void MainWindow::searchInatObservations()
{
    if (m_inatObservationFetcher->isRunning())
        return;

    const int pid = currentProjectId();
    if (pid <= 0) {
        QMessageBox::information(this, tr("Search iNaturalist"),
                                 tr("Select a reference tree first."));
        return;
    }
    const QString username = m_inatUsernameEdit->text().trimmed();
    if (username.isEmpty()) {
        QMessageBox::information(this, tr("Search iNaturalist"),
                                 tr("Enter an iNaturalist username first."));
        return;
    }
    m_app.settings().setInatUsername(username);

    const QString token = m_inatTokenEdit->text().trimmed();
    m_app.settings().setInatApiToken(token);
    m_app.inat().setAccessToken(token);

    // Leaf-rank taxa only (species and below) -- projectTaxonInatIds() returns
    // every taxon in the tree, including higher-rank nodes and the synthetic
    // "Life" root every tree carries for structure. iNat's taxon_id filter
    // matches a taxon's descendants too, so including a broad ancestor like
    // that would turn one "batch" into a search over the user's entire
    // observation history -- exactly the kind of single search that can take
    // a very long time while the batch counter sits still.
    QList<qint64> leafTaxonIds;
    for (const auto &leaf : m_app.taxonomyStore().projectLeafPhotos(pid))
        leafTaxonIds << leaf.inatId;

    // Confine the search to the tree's own locality too, not just its taxa --
    // otherwise a species that also occurs elsewhere would pull in the user's
    // observations of it from anywhere in the world. Trees built without a
    // place (e.g. from a checklist with no region) simply search worldwide
    // either way; the checkbox lets the user opt out of the restriction too.
    const qint64 placeId = m_inatRestrictToLocality->isChecked()
                               ? m_app.taxonomyStore().projectPlaceInatId(pid).value_or(0)
                               : 0;

    m_inatSearchButton->setEnabled(false);
    m_inatCancelSearchButton->setEnabled(true);
    m_inatStatus->setText(
        placeId > 0 ? tr("Searching…")
                    : tr("Searching — results aren't restricted by location…"));
    statusBar()->showMessage(tr("Searching iNaturalist for %1's observations…").arg(username));
    m_inatObservationFetcher->start(username, leafTaxonIds, placeId);
}

void MainWindow::onInatSearchFinished(bool ok, const QString &error,
                                      QList<inat::Candidate> candidates)
{
    m_inatSearchButton->setEnabled(true);
    m_inatCancelSearchButton->setEnabled(false);
    if (!ok) {
        if (error == QLatin1String("cancelled")) {
            m_inatStatus->setText(tr("Search stopped."));
            statusBar()->showMessage(tr("Search stopped."), 4000);
        } else {
            m_inatStatus->setText(tr("Search failed: %1").arg(error));
            statusBar()->showMessage(tr("Searching iNaturalist failed: %1").arg(error), 10000);
        }
        return;
    }

    m_inatDownloadModel->setCandidates(candidates);
    const int total = m_inatDownloadModel->rowCountTotal();
    m_inatStatus->setText(
        total > 0 ? tr("Found %n candidate photo(s). Faded ones look like something you "
                      "already have — check before including them.", nullptr, total)
                  : tr("No candidate photos found — either everything is already in your "
                      "library, or this user has no observations of species in this tree."));
    statusBar()->showMessage(tr("Found %n candidate photo(s).", nullptr, total), 6000);
}

void MainWindow::downloadSelectedInatPhotos()
{
    if (m_inatImportService->isRunning())
        return;

    const QModelIndexList selected = m_inatGrid->selectionModel()->selectedIndexes();
    if (selected.isEmpty()) {
        QMessageBox::information(this, tr("Download Selected"),
                                 tr("Select at least one photo first."));
        return;
    }

    const QString destParent = QFileDialog::getExistingDirectory(
        this, tr("Choose a Folder for Downloaded Photos"), QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (destParent.isEmpty())
        return;

    // A dedicated, timestamped subfolder, so this run's downloads are clearly
    // grouped and never collide with anything already at the chosen location.
    const QString destFolder = QDir(destParent).filePath(
        QStringLiteral("iNaturalist-%1")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    if (!QDir().mkpath(destFolder)) {
        QMessageBox::warning(this, tr("Download Selected"),
                             tr("Could not create %1.").arg(destFolder));
        return;
    }

    QList<inat::ImportItem> items;
    for (const QModelIndex &idx : selected) {
        inat::ImportItem item;
        item.observationId = idx.data(inat::InatDownloadModel::ObservationIdRole).toLongLong();
        item.photoId = idx.data(inat::InatDownloadModel::PhotoIdRole).toLongLong();
        item.downloadUrl = idx.data(inat::InatDownloadModel::DownloadUrlRole).toString();
        item.observedOn = idx.data(inat::InatDownloadModel::ObservedOnRole).toString();
        if (idx.data(inat::InatDownloadModel::HasGpsRole).toBool()) {
            item.latitude = idx.data(inat::InatDownloadModel::LatitudeRole).toDouble();
            item.longitude = idx.data(inat::InatDownloadModel::LongitudeRole).toDouble();
        }
        item.taxonName = idx.data(inat::InatDownloadModel::NameRole).toString();
        item.placeGuess = idx.data(inat::InatDownloadModel::PlaceGuessRole).toString();
        items.append(item);
    }

    m_inatPendingDestFolder = destFolder;
    m_inatDownloadButton->setEnabled(false);
    statusBar()->showMessage(tr("Downloading %n photo(s)…", nullptr, items.size()));
    m_inatImportService->start(items, destFolder);
}

void MainWindow::onInatImportFinished(bool ok, const QString &error, QList<inat::SavedFile> saved)
{
    m_inatDownloadButton->setEnabled(true);
    if (!ok) {
        statusBar()->showMessage(tr("Downloading photos failed: %1").arg(error), 10000);
        return;
    }

    statusBar()->showMessage(
        tr("Downloaded %n photo(s) — adding \"%1\" to the library…", nullptr, saved.size())
            .arg(m_inatPendingDestFolder),
        6000);

    // Reuses addWatchedFolder()'s own tail: register the new folder, then
    // scan it so its files become real capture rows.
    QStringList roots = m_app.settings().watchedRoots();
    const QString clean = QDir::cleanPath(m_inatPendingDestFolder);
    if (!roots.contains(clean)) {
        roots.append(clean);
        m_app.settings().setWatchedRoots(roots);
        m_app.libraryWatcher().setRoots(roots);
    }
    updateEmptyState();

    // Provenance can only be stamped once the scan above actually creates the
    // capture rows for these new files, so do it once that settles rather
    // than as part of this handler.
    const QString connectionName = m_app.database().connectionName();
    const QString destFolder = m_inatPendingDestFolder;
    connect(&m_app.scanService(), &scan::ScanService::finished, this,
            [connectionName, saved](const scan::ScanSummary &) {
                inat::InatImportService::stampProvenance(connectionName, saved);
            },
            Qt::SingleShotConnection);
    startScan();
}

void MainWindow::selectTaxonInTree(qint64 inatId)
{
    QModelIndex idx = m_treeModel->indexForTaxon(inatId);
    if (!idx.isValid() && m_photographedOnly->isChecked()) {
        // A missing (unphotographed) taxon is hidden by the filter — lift it,
        // keeping the tree's position, then try again.
        m_photographedOnly->setChecked(false);
        idx = m_treeModel->indexForTaxon(inatId);
    }
    if (!idx.isValid())
        return;
    m_treeView->setCurrentIndex(idx);
    m_treeView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
}

void MainWindow::onTreeSearchChanged(const QString &text)
{
    m_treeSearchHits = m_treeModel->findTaxa(text);
    m_treeSearchPos = 0;
    if (!m_treeSearchHits.isEmpty())
        selectTaxonInTree(m_treeSearchHits.first());
}

void MainWindow::onTreeSearchNext()
{
    if (m_treeSearchHits.isEmpty())
        return;
    m_treeSearchPos = (m_treeSearchPos + 1) % m_treeSearchHits.size();
    selectTaxonInTree(m_treeSearchHits.at(m_treeSearchPos));
}

void MainWindow::updateEmptyState()
{
    QStackedWidget *stack = m_photoStack;
    if (!stack)
        return;

    const int count = m_model->captureCount();
    const bool filtered = !m_model->statusFilter().isEmpty();
    if (count > 0) {
        stack->setCurrentIndex(1);
    } else if (filtered) {
        m_emptyHint->setText(tr("No captures match this filter."));
        stack->setCurrentIndex(0);
    } else {
        const bool haveRoots = !m_app.settings().watchedRoots().isEmpty();
        m_emptyHint->setText(haveRoots
                                 ? tr("No photos catalogued yet. Rescan the library to import them.")
                                 : tr("Add a folder of photos to start building your library."));
        stack->setCurrentIndex(0);
    }

    m_statusLabel->setText(count > 0
                               ? tr("%n capture(s)%1", nullptr, count)
                                     .arg(filtered ? tr(" (filtered)") : QString())
                               : QString());
}

void MainWindow::addWatchedFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Add Folder to Library"),
        QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty())
        return;

    QStringList roots = m_app.settings().watchedRoots();
    const QString clean = QDir::cleanPath(dir);
    if (!roots.contains(clean)) {
        roots.append(clean);
        m_app.settings().setWatchedRoots(roots);
        m_app.libraryWatcher().setRoots(roots);
    }

    updateEmptyState();
    startScan();
}

void MainWindow::startScan()
{
    const QStringList roots = m_app.settings().watchedRoots();
    if (roots.isEmpty()) {
        QMessageBox::information(this, tr("Rescan Library"),
                                tr("Add a folder to the library first."));
        return;
    }
    if (m_app.scanService().isRunning())
        return;

    m_app.scanService().start(roots);
}

void MainWindow::setScanUiRunning(bool running)
{
    m_addFolderAction->setEnabled(!running);
    m_scanAction->setEnabled(!running);
    m_cancelAction->setEnabled(running);
    if (running)
        statusBar()->showMessage(tr("Scanning…"));
}

void MainWindow::onScanProgress(const scan::ScanProgress &progress)
{
    statusBar()->showMessage(tr("Scanning %1 — %2 folders, %3 photos")
                                 .arg(progress.currentDir)
                                 .arg(progress.foldersSeen)
                                 .arg(progress.filesSeen));
}

void MainWindow::onScanFinished(const scan::ScanSummary &summary)
{
    setScanUiRunning(false);
    m_model->reload();
    updateEmptyState();
    m_app.libraryWatcher().refresh();

    if (!summary.ok()) {
        statusBar()->showMessage(tr("Scan failed: %1").arg(summary.error), 10000);
    } else if (summary.cancelled) {
        statusBar()->showMessage(tr("Scan stopped."), 5000);
    } else {
        statusBar()->showMessage(
            tr("Scan complete — %1 new, %2 updated, %3 unchanged")
                .arg(summary.capturesAdded)
                .arg(summary.capturesUpdated + summary.renditionsUpdated)
                .arg(summary.renditionsUnchanged),
            8000);
    }
}

void MainWindow::restoreLayout()
{
    const QByteArray geometry = m_app.settings().mainWindowGeometry();
    if (geometry.isEmpty())
        resize(1100, 760);
    else
        restoreGeometry(geometry);

    const QByteArray state = m_app.settings().mainWindowState();
    if (!state.isEmpty())
        restoreState(state);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_app.scanService().cancel();
    m_app.settings().setMainWindowGeometry(saveGeometry());
    m_app.settings().setMainWindowState(saveState());
    QMainWindow::closeEvent(event);
}

void MainWindow::showHelp()
{
    if (!m_helpWindow)
        m_helpWindow = new HelpWindow(this);
    m_helpWindow->showPage();
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this,
        tr("About %1").arg(QString::fromLatin1(kAppName)),
        tr("<h3>%1 %2</h3><p>Catalogue nature photos against a taxonomic tree.</p>"
           "<p style='color:gray'>Build %3 · Qt %4</p>")
            .arg(QString::fromLatin1(kAppName), QString::fromLatin1(kAppVersion),
                 QString::fromLatin1(kAppVersionFull), QString::fromLatin1(qVersion())));
}

void MainWindow::showStorageUsage()
{
    const QString dbPath = m_app.settings().databasePath();
    qint64 dbBytes = QFileInfo(dbPath).size();
    dbBytes += QFileInfo(dbPath + QStringLiteral("-wal")).size();
    dbBytes += QFileInfo(dbPath + QStringLiteral("-shm")).size();

    const qint64 thumbBytes = app::directoryBytes(m_app.thumbnails().cacheDir());
    const qint64 photoBytes = app::directoryBytes(m_app.photoCache().cacheDir());
    const qint64 tileBytes = app::directoryBytes(m_app.tileCache().cacheDir());
    const qint64 total = dbBytes + thumbBytes + photoBytes + tileBytes;

    QLocale locale;
    const QString html =
        tr("<table cellspacing='6'>"
           "<tr><td>Catalogue database</td><td align='right'>%1</td></tr>"
           "<tr><td>Photo thumbnails</td><td align='right'>%2</td></tr>"
           "<tr><td>Reference photos (iNaturalist)</td><td align='right'>%3</td></tr>"
           "<tr><td>Map tiles (OpenStreetMap)</td><td align='right'>%4</td></tr>"
           "<tr><td><b>Total</b></td><td align='right'><b>%5</b></td></tr>"
           "</table>")
            .arg(locale.formattedDataSize(dbBytes), locale.formattedDataSize(thumbBytes),
                 locale.formattedDataSize(photoBytes), locale.formattedDataSize(tileBytes),
                 locale.formattedDataSize(total));

    QMessageBox::information(this, tr("Storage Usage"), html);
}

} // namespace pl
