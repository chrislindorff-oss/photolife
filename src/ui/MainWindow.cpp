#include "ui/MainWindow.h"

#include "app/Application.h"
#include "db/Database.h"
#include "model/CaptureListModel.h"
#include "pl/Version.h"
#include "scan/ScanService.h"
#include "settings/Settings.h"
#include "thumb/ThumbnailCache.h"

#include <QAction>
#include <QCloseEvent>
#include <QFileDialog>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>

namespace pl {

MainWindow::MainWindow(Application &app, QWidget *parent)
    : QMainWindow(parent), m_app(app)
{
    setWindowTitle(QString::fromLatin1(kAppName));

    m_model = new model::CaptureListModel(m_app.database(), m_app.thumbnails(), this);

    buildMenus();
    buildCentralWidget();
    restoreLayout();

    auto &scanner = m_app.scanService();
    connect(&scanner, &scan::ScanService::started, this, [this] { setScanUiRunning(true); });
    connect(&scanner, &scan::ScanService::progress, this, &MainWindow::onScanProgress);
    connect(&scanner, &scan::ScanService::finished, this, &MainWindow::onScanFinished);

    m_model->reload();
    updateEmptyState();
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
}

void MainWindow::buildCentralWidget()
{
    m_grid = new QListView(this);
    m_grid->setModel(m_model);
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setUniformItemSizes(true);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setIconSize(QSize(192, 192));
    m_grid->setGridSize(QSize(212, 232));
    m_grid->setWordWrap(true);
    m_grid->setSpacing(6);

    m_emptyHint = new QLabel(this);
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setEnabled(false);

    auto *stack = new QStackedWidget(this);
    stack->addWidget(m_emptyHint);   // index 0
    stack->addWidget(m_grid);        // index 1
    setCentralWidget(stack);

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel);
}

void MainWindow::updateEmptyState()
{
    auto *stack = qobject_cast<QStackedWidget *>(centralWidget());
    if (!stack)
        return;

    const int count = m_model->captureCount();
    if (count > 0) {
        stack->setCurrentIndex(1);
    } else {
        const bool haveRoots = !m_app.settings().watchedRoots().isEmpty();
        m_emptyHint->setText(haveRoots
                                 ? tr("No photos catalogued yet. Rescan the library to import them.")
                                 : tr("Add a folder of photos to start building your library."));
        stack->setCurrentIndex(0);
    }

    m_statusLabel->setText(count > 0 ? tr("%n capture(s)", nullptr, count) : QString());
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
