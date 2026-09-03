#include "ui/MainWindow.h"

#include "app/Application.h"
#include "db/Database.h"
#include "pl/Version.h"
#include "settings/Settings.h"

#include <QAction>
#include <QCloseEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

namespace pl {

MainWindow::MainWindow(Application &app, QWidget *parent)
    : QMainWindow(parent)
    , m_app(app)
{
    setWindowTitle(QString::fromLatin1(kAppName));

    buildMenus();
    buildCentralWidget();
    restoreLayout();

    statusBar()->showMessage(
        tr("Catalogue schema v%1").arg(m_app.database().schemaVersion()));
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    QAction *quit = fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *about = helpMenu->addAction(
        tr("&About %1").arg(QString::fromLatin1(kAppName)), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);
}

void MainWindow::buildCentralWidget()
{
    // Placeholder until the library view lands; watched roots come from Settings.
    auto *placeholder = new QLabel(tr("No folders are being watched yet."), this);
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setEnabled(false);
    setCentralWidget(placeholder);
}

void MainWindow::restoreLayout()
{
    const QByteArray geometry = m_app.settings().mainWindowGeometry();
    if (geometry.isEmpty())
        resize(1024, 720);
    else
        restoreGeometry(geometry);

    const QByteArray state = m_app.settings().mainWindowState();
    if (!state.isEmpty())
        restoreState(state);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
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
