#include "ui/MainWindow.h"

#include "app/Application.h"
#include "db/Database.h"
#include "model/CaptureListModel.h"
#include "model/TaxonomyTreeModel.h"
#include "pl/Version.h"
#include "checklist/ChecklistImporter.h"
#include "checklist/ChecklistParser.h"
#include "coverage/CoverageCalculator.h"
#include "match/MatchService.h"
#include "scan/LibraryWatcher.h"
#include "scan/ScanService.h"
#include "settings/Settings.h"
#include "taxonomy/ProjectBuilder.h"
#include "taxonomy/TaxonomyStore.h"
#include "thumb/ThumbnailCache.h"
#include "ui/AliasEditorDialog.h"
#include "ui/CoveragePanel.h"
#include "ui/ImageViewer.h"
#include "ui/NewProjectDialog.h"
#include "ui/ReviewPane.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
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
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QPainter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeView>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

#include "db/Database.h"

namespace pl {
namespace {

// Draws the base thumbnail plus a small status pip in the corner.
class CaptureDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);

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
    }
};

} // namespace

MainWindow::MainWindow(Application &app, QWidget *parent)
    : QMainWindow(parent), m_app(app)
{
    setWindowTitle(QString::fromLatin1(kAppName));

    m_model = new model::CaptureListModel(m_app.database(), m_app.thumbnails(), this);
    m_treeModel = new model::TaxonomyTreeModel(m_app.database(), this);
    m_builder = new taxonomy::ProjectBuilder(m_app.inat(), m_app.taxonomyStore(), this);
    m_checklistImporter =
        new checklist::ChecklistImporter(m_app.inat(), m_app.taxonomyStore(), this);

    buildMenus();
    buildCentralWidget();
    buildReferenceTreeDock();
    restoreLayout();

    connect(m_builder, &taxonomy::ProjectBuilder::progress, this,
            [this](const QString &phase, int done, int total) {
                statusBar()->showMessage(total > 0
                                             ? tr("%1 — %2 / %3").arg(phase).arg(done).arg(total)
                                             : phase);
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
                updateEmptyState();
                refreshCoverage();
                m_reviewPane->reload();
                updateReviewTabText();
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

    fileMenu->addSeparator();
    m_newTreeAction = fileMenu->addAction(tr("&New Reference Tree…"),
                                          this, &MainWindow::newReferenceTree);
    m_refreshTreeAction = fileMenu->addAction(tr("Re&fresh Reference Tree"),
                                              this, &MainWindow::refreshReferenceTree);
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

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *about = helpMenu->addAction(
        tr("&About %1").arg(QString::fromLatin1(kAppName)), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);

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
    });

    m_treeView = new QTreeView(panel);
    m_treeView->setModel(m_treeModel);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setAlternatingRowColors(true);
    m_treeView->header()->setStretchLastSection(false);
    m_treeView->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_treeView->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_treeView->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_treeView->setColumnWidth(0, 240);
    m_treeView->setAutoScroll(false);   // keep shallow names visible when selecting deep nodes
    connect(m_treeView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &MainWindow::onTreeSelectionChanged);

    m_coveragePanel = new CoveragePanel(panel);

    auto *split = new QSplitter(Qt::Vertical, panel);
    split->addWidget(m_treeView);
    split->addWidget(m_coveragePanel);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);

    layout->addWidget(m_projectCombo);
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
    if (pid <= 0 || !m_app.database().isOpen()) {
        m_coveragePanel->clear();
        m_treeModel->setCoverage({});
        return;
    }
    m_coverage = coverage::computeCoverage(m_app.database().connectionName(), pid);
    coverage::pickRepresentatives(m_app.database().connectionName(), pid);
    m_coveragePanel->setCoverage(m_coverage);
    m_treeModel->setCoverage(m_coverage);
    updateMissingList();
    onTreeSelectionChanged();
}

void MainWindow::onTreeSelectionChanged()
{
    const QModelIndex idx = m_treeView->currentIndex();
    m_selectedTaxon = idx.isValid()
                          ? idx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong()
                          : 0;
    m_taxonModel->setTaxonScope(m_selectedTaxon);

    if (m_selectedTaxon <= 0) {
        m_taxonInfo->setText(tr("<i>Select a taxon in the tree to see its photos.</i>"));
        m_taxonRepImage->clear();
        return;
    }

    const auto cov = m_coverage.byTaxon.value(m_selectedTaxon);
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
        if (tc.rank.compare(QLatin1String("species"), Qt::CaseInsensitive) == 0
            && !tc.subtreeHasPhotos) {
            missing.append(tc);
        }
    }
    std::sort(missing.begin(), missing.end(),
              [](const coverage::TaxonCoverage &a, const coverage::TaxonCoverage &b) {
                  return a.name < b.name;
              });

    for (const auto &tc : missing) {
        QString label = tc.name;
        if (!tc.status.isEmpty())
            label += QStringLiteral("   — %1").arg(tc.status);
        auto *item = new QListWidgetItem(label, m_missingList);
        item->setData(Qt::UserRole, tc.inatId);
        if (!tc.status.isEmpty())
            item->setForeground(QColor(0xB0, 0x50, 0x00));
    }

    const int missingTab = m_tabs->indexOf(m_missingList);
    if (missingTab >= 0)
        m_tabs->setTabText(missingTab, missing.isEmpty()
                                          ? tr("Missing Species")
                                          : tr("Missing Species (%1)").arg(missing.size()));
}

void MainWindow::updateReviewTabText()
{
    const int reviewTab = m_tabs->indexOf(m_reviewPane);
    if (reviewTab < 0)
        return;
    const int n = m_reviewPane->queueCount();
    m_tabs->setTabText(reviewTab, n > 0 ? tr("Review (%1)").arg(n) : tr("Review"));
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
        item.caption = idx.data(Qt::DisplayRole).toString();
        if (const QString matched = idx.data(model::CaptureListModel::MatchedNameRole).toString();
            !matched.isEmpty())
            item.caption += QStringLiteral("  ·  ") + matched;
        if (item.path.isEmpty())
            continue;
        if (r == clicked.row())
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

    m_newTreeAction->setEnabled(false);
    statusBar()->showMessage(tr("Building reference tree…"));
    m_builder->start(dialog.request());
}

namespace {

QListView *makeCaptureGrid(QWidget *parent, QAbstractItemModel *model)
{
    auto *grid = new QListView(parent);
    grid->setModel(model);
    grid->setViewMode(QListView::IconMode);
    grid->setResizeMode(QListView::Adjust);
    grid->setMovement(QListView::Static);
    grid->setUniformItemSizes(true);
    grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    grid->setIconSize(QSize(192, 192));
    grid->setGridSize(QSize(212, 232));
    grid->setWordWrap(true);
    grid->setSpacing(6);
    return grid;
}

} // namespace

void MainWindow::buildCentralWidget()
{
    m_grid = makeCaptureGrid(this, m_model);
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

    m_reviewPane = new ReviewPane(m_app.database(), m_app.thumbnails(), this);
    connect(m_reviewPane, &ReviewPane::queueChanged, this, [this] {
        m_model->reload();
        updateEmptyState();
        refreshCoverage();
        updateReviewTabText();
    });

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildBrowsePage(), tr("Browse"));
    m_tabs->addTab(m_photoStack, tr("All Photos"));
    m_tabs->addTab(m_reviewPane, tr("Review"));
    m_tabs->addTab(buildMissingPage(), tr("Missing Species"));
    m_tabs->setCurrentIndex(1);   // start on All Photos
    setCentralWidget(m_tabs);
    updateReviewTabText();

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

    auto *header = new QHBoxLayout;
    header->addWidget(m_taxonRepImage);
    header->addWidget(m_taxonInfo, 1);

    m_taxonGrid = makeCaptureGrid(this, m_taxonModel);
    m_taxonGrid->setItemDelegate(new CaptureDelegate(m_taxonGrid));
    connect(m_taxonGrid, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &i) { openViewer(m_taxonModel, i); });

    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addLayout(header);
    layout->addWidget(m_taxonGrid, 1);
    return page;
}

QWidget *MainWindow::buildMissingPage()
{
    m_missingList = new QListWidget(this);
    m_missingList->setAlternatingRowColors(true);
    connect(m_missingList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const qint64 inatId = item->data(Qt::UserRole).toLongLong();
        std::function<bool(const QModelIndex &)> findAndSelect = [&](const QModelIndex &parent) {
            for (int r = 0; r < m_treeModel->rowCount(parent); ++r) {
                const QModelIndex idx = m_treeModel->index(r, 0, parent);
                if (idx.data(model::TaxonomyTreeModel::InatIdRole).toLongLong() == inatId) {
                    m_treeView->setCurrentIndex(idx);
                    m_treeView->scrollTo(idx);
                    return true;
                }
                if (findAndSelect(idx))
                    return true;
            }
            return false;
        };
        if (findAndSelect({}))
            m_tabs->setCurrentIndex(0);
    });
    return m_missingList;
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

void MainWindow::showAbout()
{
    QMessageBox::about(
        this,
        tr("About %1").arg(QString::fromLatin1(kAppName)),
        tr("<h3>%1 %2</h3><p>Catalogue nature photos against a taxonomic tree.</p>")
            .arg(QString::fromLatin1(kAppName), QString::fromLatin1(kAppVersion)));
}

} // namespace pl
