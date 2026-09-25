#include <QApplication>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QPushButton>
#include <QtConcurrent>
#include <QTimer>
#include <QWidget>
#include <QAction>
#include <QTabWidget>
#include <QTreeView>

#include <cstdio>
#include <memory>
#include <optional>

#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "app/Application.h"
#include "checklist/ChecklistImporter.h"
#include "checklist/ChecklistParser.h"
#include "db/Database.h"
#include "lightroom/LightroomImporter.h"
#include "match/MatchService.h"
#include "scan/ScanService.h"
#include "scan/ScanTypes.h"
#include "settings/Settings.h"
#include "taxonomy/ProjectBuilder.h"
#include "taxonomy/TaxonomyStore.h"
#include "ui/CatalogueConnectDialog.h"
#include "ui/Theme.h"
#include "pl/Version.h"

namespace {

// Headless "scan these folders and exit" mode, for development and CI.
int runHeadlessScan(pl::Application &app, const QStringList &roots)
{
    QEventLoop loop;
    int exitCode = 0;

    auto &scanner = app.scanService();
    QObject::connect(&scanner, &pl::scan::ScanService::progress,
                     [](const pl::scan::ScanProgress &p) {
                         std::fprintf(stderr, "\r%d folders, %d photos, %d captures   ",
                                      p.foldersSeen, p.filesSeen, p.capturesSeen);
                     });
    QObject::connect(&scanner, &pl::scan::ScanService::finished,
                     [&](const pl::scan::ScanSummary &s) {
                         std::fprintf(stderr, "\n");
                         qInfo().noquote()
                             << QStringLiteral("captures +%1 ~%2 | renditions +%3 ~%4 =%5 | "
                                               "hashed %6 | folders %7 | %8")
                                    .arg(s.capturesAdded)
                                    .arg(s.capturesUpdated)
                                    .arg(s.renditionsAdded)
                                    .arg(s.renditionsUpdated)
                                    .arg(s.renditionsUnchanged)
                                    .arg(s.filesHashed)
                                    .arg(s.foldersUpserted)
                                    .arg(s.ok() ? (s.cancelled ? QStringLiteral("cancelled")
                                                               : QStringLiteral("ok"))
                                                : QStringLiteral("error: ") + s.error);
                         exitCode = s.ok() ? 0 : 1;
                         loop.quit();
                     });

    QTimer::singleShot(0, [&] { scanner.start(roots); });
    loop.exec();
    return exitCode;
}

// Headless "build a reference tree and exit" mode.
int runHeadlessBuild(pl::Application &app, const pl::taxonomy::ProjectBuilder::Request &request)
{
    QEventLoop loop;
    int exitCode = 0;

    pl::taxonomy::TaxonomyStore store(app.database().connectionName());
    pl::taxonomy::ProjectBuilder builder(app.inat(), store);

    // No interactive user in a headless build — keep pulling species past every
    // checkpoint.
    QObject::connect(&builder, &pl::taxonomy::ProjectBuilder::confirmMoreSpecies,
                     &builder, [&builder](int, int) { builder.continueFetching(); });

    QObject::connect(&builder, &pl::taxonomy::ProjectBuilder::progress,
                     [](const QString &phase, int done, int total) {
                         if (total > 0)
                             std::fprintf(stderr, "\r%-28s %d / %d      ",
                                          phase.toUtf8().constData(), done, total);
                         else
                             std::fprintf(stderr, "\r%-28s            ",
                                          phase.toUtf8().constData());
                     });
    QObject::connect(&builder, &pl::taxonomy::ProjectBuilder::finished,
                     [&](bool ok, const QString &error, int projectId) {
                         std::fprintf(stderr, "\n");
                         if (ok) {
                             qInfo().noquote()
                                 << QStringLiteral("project %1 built: %2 taxa cached")
                                        .arg(projectId)
                                        .arg(store.projectTaxonInatIds(projectId).size());
                         } else {
                             qWarning().noquote() << "build failed:" << error;
                             exitCode = 1;
                         }
                         loop.quit();
                     });

    QTimer::singleShot(0, [&] { builder.start(request); });
    loop.exec();
    return exitCode;
}

// Headless "match the catalogue against the taxonomy and exit" mode.
int runHeadlessMatch(pl::Application &app)
{
    QEventLoop loop;
    int exitCode = 0;

    auto &matcher = app.matchService();
    QObject::connect(&matcher, &pl::match::MatchService::progress, [](int done, int total) {
        std::fprintf(stderr, "\rmatching %d / %d   ", done, total);
    });
    QObject::connect(&matcher, &pl::match::MatchService::finished,
                     [&](pl::match::MatchEngine::Stats s) {
                         std::fprintf(stderr, "\n");
                         if (!s.ok()) {
                             qWarning().noquote() << "match failed:" << s.error;
                             exitCode = 1;
                         } else {
                             qInfo().noquote()
                                 << QStringLiteral("%1 captures | %2 auto | %3 pending | "
                                                   "%4 unmatched%5")
                                        .arg(s.captures)
                                        .arg(s.autoApplied)
                                        .arg(s.pending)
                                        .arg(s.unmatched)
                                        .arg(s.cancelled ? QStringLiteral(" (cancelled)")
                                                         : QString());
                         }
                         loop.quit();
                     });

    QTimer::singleShot(0, [&] { matcher.start(); });
    loop.exec();
    return exitCode;
}

// Headless "import a checklist CSV into a project and exit" mode.
int runHeadlessChecklist(pl::Application &app, const QString &path, const QString &projectName,
                         const QString &source)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << "cannot read" << path;
        return 1;
    }
    const auto entries = pl::checklist::parseChecklistCsv(file.readAll());
    qInfo().noquote() << QStringLiteral("%1: %2 rows").arg(path).arg(entries.size());

    auto db = QSqlDatabase::database(app.database().connectionName(), false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT id, place_inat_id FROM project WHERE name = ?"));
    q.addBindValue(projectName);
    if (!q.exec() || !q.next()) {
        qWarning().noquote() << "no project named" << projectName;
        return 1;
    }

    pl::checklist::ChecklistImporter::Request request;
    request.projectId = q.value(0).toInt();
    request.source = source.isEmpty() ? QStringLiteral("checklist") : source;
    if (!q.value(1).isNull())
        request.placeInatId = q.value(1).toLongLong();
    request.entries = entries;

    pl::taxonomy::TaxonomyStore store(app.database().connectionName());
    pl::checklist::ChecklistImporter importer(app.inat(), store);

    QEventLoop loop;
    int exitCode = 0;
    QObject::connect(&importer, &pl::checklist::ChecklistImporter::progress,
                     [](int done, int total) {
                         std::fprintf(stderr, "\rimporting %d / %d   ", done, total);
                     });
    QObject::connect(&importer, &pl::checklist::ChecklistImporter::finished,
                     [&](bool ok, const QString &error, int imported, int skipped, int unresolved) {
                         std::fprintf(stderr, "\n");
                         if (!ok) {
                             qWarning().noquote() << "import failed:" << error;
                             exitCode = 1;
                         } else {
                             qInfo().noquote()
                                 << QStringLiteral("%1 imported | %2 skipped | %3 unresolved")
                                        .arg(imported).arg(skipped).arg(unresolved);
                         }
                         loop.quit();
                     });
    QTimer::singleShot(0, [&] { importer.start(request); });
    loop.exec();
    return exitCode;
}

// Headless "import a Lightroom catalog's keywords and exit" mode. --into only
// gates this the same way the interactive action does (the catalog import
// itself isn't project-scoped -- see LightroomImporter) so a script gets the
// same "no project, no import" guard as the GUI.
int runHeadlessLightroomImport(pl::Application &app, const QString &lrcatPath,
                               const QString &projectName)
{
    if (!projectName.isEmpty()) {
        auto db = QSqlDatabase::database(app.database().connectionName(), false);
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT id FROM project WHERE name = ?"));
        q.addBindValue(projectName);
        if (!q.exec() || !q.next()) {
            qWarning().noquote() << "no project named" << projectName;
            return 1;
        }
    }

    auto &importer = app.lightroomImporter();
    QEventLoop loop;
    int exitCode = 0;
    QObject::connect(&importer, &pl::lightroom::LightroomImporter::progress,
                     [](int done, int total) {
                         std::fprintf(stderr, "\rimporting %d / %d   ", done, total);
                     });
    QObject::connect(&importer, &pl::lightroom::LightroomImporter::finished,
                     [&](pl::lightroom::LightroomImportEngine::Stats s) {
                         std::fprintf(stderr, "\n");
                         if (!s.ok()) {
                             qWarning().noquote() << "import failed:" << s.error;
                             exitCode = 1;
                         } else {
                             qInfo().noquote()
                                 << QStringLiteral("%1 photos matched | %2 unmatched | "
                                                   "%3 keywords resolved | %4 hints written")
                                        .arg(s.photosMatched)
                                        .arg(s.photosUnmatched)
                                        .arg(s.keywordsResolved)
                                        .arg(s.hintsWritten);
                         }
                         loop.quit();
                     });
    QTimer::singleShot(0, [&] { importer.start(lrcatPath); });
    loop.exec();
    return exitCode;
}

// What resolveStartupCatalogue() decided: either quit outright (the user
// declined to fall back after a failed shared-catalogue connect), or open
// `descriptorOverride` instead of the caller's descriptor if it's set, or
// proceed with the caller's own descriptor unchanged if it's not.
struct StartupCatalogueChoice
{
    bool quit = false;
    std::optional<pl::CatalogueDescriptor> descriptorOverride;
};

// Settles which catalogue an interactive launch should actually open when
// Settings names a shared Postgres one. Reachability is probed on a
// background thread (a throwaway connection, see Database::probeReachable())
// purely so `dialog`'s "Use Local Catalogue" button stays clickable the
// whole time -- otherwise the GUI thread would sit blocked inside the real,
// synchronous connect attempt with no way to respond to it. Once resolved,
// the caller performs the real, single Database::open() on the GUI thread as
// usual; this never touches the app's own long-lived connection.
StartupCatalogueChoice resolveStartupCatalogue(const pl::CatalogueDescriptor &descriptor,
                                               pl::CatalogueConnectDialog &dialog)
{
    struct ProbeResult
    {
        bool ok = false;
        QString error;
    };

    dialog.show();

    auto *watcher = new QFutureWatcher<ProbeResult>();
    QEventLoop waitLoop;
    bool useLocalClicked = false;
    ProbeResult result;

    QMetaObject::Connection localConn = QObject::connect(
        &dialog, &pl::CatalogueConnectDialog::useLocalRequested, &waitLoop, [&] {
            useLocalClicked = true;
            waitLoop.quit();
        });
    QMetaObject::Connection finishedConn =
        QObject::connect(watcher, &QFutureWatcher<ProbeResult>::finished, &waitLoop, [&] {
            result = watcher->result();
            waitLoop.quit();
        });
    // Self-cleanup, independent of the connections above -- if the user
    // bails out early, this watcher (and the probe still running on a
    // thread-pool thread behind it) must stay alive and valid until the
    // probe genuinely finishes, just with nothing left listening by then.
    QObject::connect(watcher, &QFutureWatcher<ProbeResult>::finished, watcher,
                      &QObject::deleteLater);

    watcher->setFuture(QtConcurrent::run([descriptor] {
        ProbeResult r;
        r.ok = pl::Database::probeReachable(descriptor, &r.error);
        return r;
    }));

    waitLoop.exec();
    // The decision is made either way now -- disconnect so a probe that's
    // still running in the background can't later touch waitLoop/result
    // after this function has returned and they've gone out of scope.
    QObject::disconnect(localConn);
    QObject::disconnect(finishedConn);

    if (useLocalClicked) {
        dialog.hideUseLocalButton();
        return {false, pl::CatalogueDescriptor::sqlite(pl::Settings().databasePath())};
    }

    if (!result.ok) {
        dialog.hide();
        QMessageBox box(QMessageBox::Warning, QObject::tr("Can't Reach Shared Catalogue"),
                        QObject::tr("Could not connect to the shared catalogue:\n\n%1")
                            .arg(result.error));
        QPushButton *useLocalButton =
            box.addButton(QObject::tr("Use Local Catalogue"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Close);
        box.setDefaultButton(useLocalButton);
        box.exec();
        if (box.clickedButton() == useLocalButton)
            return {false, pl::CatalogueDescriptor::sqlite(pl::Settings().databasePath())};
        return {true, std::nullopt};
    }

    dialog.hideUseLocalButton();
    return {false, std::nullopt};
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication qtApp(argc, argv);

    // Set before any QSettings is constructed (including the pre-init
    // catalogueDescriptor() peek below): QSettings' default constructor
    // resolves its file from QCoreApplication::organizationName()/
    // applicationName(), which are otherwise still unset at this point --
    // that peek would silently read a different, nonexistent settings file
    // (Application::initialize() sets these again below, harmlessly).
    QCoreApplication::setApplicationName(QString::fromLatin1(pl::kAppName));
    QCoreApplication::setOrganizationName(QString::fromLatin1(pl::kOrgName));

    // Cheap, local Settings peek (same pattern as the catalogueDescriptor()
    // peek below) to pick the persisted light/dark preference before any
    // window is built.
    pl::applyTheme(qtApp, pl::Settings().darkModeEnabled() ? pl::ThemeVariant::Dark
                                                            : pl::ThemeVariant::Light);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("PhotoLife — catalogue nature photos against a taxonomic tree."));
    parser.addHelpOption();
    const QCommandLineOption scanOption(
        QStringLiteral("scan"),
        QStringLiteral("Scan <dir> into the catalogue and exit (repeatable)."),
        QStringLiteral("dir"));
    parser.addOption(scanOption);

    const QCommandLineOption buildOption(
        QStringLiteral("build-project"),
        QStringLiteral("Build a reference tree named <name> from iNaturalist and exit."),
        QStringLiteral("name"));
    const QCommandLineOption taxonOption(
        QStringLiteral("taxon"), QStringLiteral("Root taxon for --build-project."),
        QStringLiteral("query"));
    const QCommandLineOption rankOption(
        QStringLiteral("rank"), QStringLiteral("Optional rank filter for --taxon."),
        QStringLiteral("rank"));
    const QCommandLineOption placeOption(
        QStringLiteral("place"), QStringLiteral("Region for --build-project (blank = global)."),
        QStringLiteral("query"));
    parser.addOption(buildOption);
    parser.addOption(taxonOption);
    parser.addOption(rankOption);
    parser.addOption(placeOption);

    const QCommandLineOption matchOption(
        QStringLiteral("match"),
        QStringLiteral("Match the catalogue against the cached taxonomy and exit."));
    parser.addOption(matchOption);

    const QCommandLineOption checklistOption(
        QStringLiteral("import-checklist"),
        QStringLiteral("Import a checklist CSV <file> into --into and exit."),
        QStringLiteral("file"));
    const QCommandLineOption intoOption(
        QStringLiteral("into"),
        QStringLiteral("Target project name for --import-checklist / --import-lightroom."),
        QStringLiteral("project"));
    const QCommandLineOption sourceOption(
        QStringLiteral("source"), QStringLiteral("Status source label for --import-checklist."),
        QStringLiteral("label"));
    parser.addOption(checklistOption);
    parser.addOption(intoOption);
    parser.addOption(sourceOption);

    const QCommandLineOption lightroomOption(
        QStringLiteral("import-lightroom"),
        QStringLiteral("Import keyword hints from a Lightroom catalog <file> and exit."),
        QStringLiteral("file"));
    parser.addOption(lightroomOption);

    const QCommandLineOption screenshotOption(
        QStringLiteral("screenshot"),
        QStringLiteral("Open the window, save a PNG to <file>, and exit."),
        QStringLiteral("file"));
    const QCommandLineOption tabOption(
        QStringLiteral("tab"), QStringLiteral("Tab index for --screenshot."),
        QStringLiteral("n"), QStringLiteral("1"));
    parser.addOption(screenshotOption);
    parser.addOption(tabOption);

    parser.process(qtApp);

    // Only the plain interactive launch below (no headless/screenshot flag)
    // can benefit from a "connecting" indicator -- the CLI modes already
    // report their own progress on stderr/the log file as they run.
    const bool interactiveGui = !parser.isSet(scanOption) && !parser.isSet(buildOption)
                              && !parser.isSet(matchOption) && !parser.isSet(checklistOption)
                              && !parser.isSet(lightroomOption) && !parser.isSet(screenshotOption);

    // Opening a shared Postgres catalogue can take several seconds (a
    // network round-trip, or waking a suspended database), with nothing on
    // screen the whole time otherwise -- that looks exactly like PhotoLife
    // has hung. A quick peek at Settings (cheap, local, no network) decides
    // whether a status window -- with an escape hatch to the local catalogue
    // if it's actually stalled or dead -- is worth showing while it connects.
    std::unique_ptr<pl::CatalogueConnectDialog> connecting;
    std::optional<pl::CatalogueDescriptor> descriptorOverride;
    if (interactiveGui) {
        const pl::CatalogueDescriptor descriptor = pl::Settings().catalogueDescriptor();
        if (descriptor.backend == pl::CatalogueDescriptor::Backend::Postgres) {
            connecting = std::make_unique<pl::CatalogueConnectDialog>();
            const StartupCatalogueChoice choice = resolveStartupCatalogue(descriptor, *connecting);
            if (choice.quit)
                return 0;
            descriptorOverride = choice.descriptorOverride;
        }
    }

    pl::Application app;
    // Each callback fires between real steps (the connect attempt, then one
    // per migration) so the dialog reflects actual progress and stays
    // painted/responsive instead of freezing as a static "please wait" image
    // for the whole connect+migrate duration.
    const bool initialized =
        app.initialize(
            [&connecting, &qtApp](const QString &status) {
                if (connecting) {
                    connecting->setStatus(status);
                    qtApp.processEvents();
                }
            },
            descriptorOverride);
    if (connecting)
        connecting->close();
    if (!initialized)
        return 1;

    if (parser.isSet(scanOption))
        return runHeadlessScan(app, parser.values(scanOption));

    if (parser.isSet(buildOption)) {
        pl::taxonomy::ProjectBuilder::Request request;
        request.projectName = parser.value(buildOption);
        request.taxonQuery = parser.value(taxonOption);
        request.rank = parser.value(rankOption);
        request.placeQuery = parser.value(placeOption);
        return runHeadlessBuild(app, request);
    }

    if (parser.isSet(screenshotOption)) {
        app.showMainWindow();
        const QString out = parser.value(screenshotOption);
        const int tab = parser.value(tabOption).toInt();
        QTimer::singleShot(1000, [&] {
            // --tab numbering matches the nine logical screens (kept stable across the
            // Reference Tree / All Library Photos / Review Unmatched view-mode split, and
            // across the Reference Tree mode's own tab order/insertions):
            //   0 Photos of Tree Selection, 1 All Library Photos, 2 Review Unmatched,
            //   3 Unphotographed Taxa, 4 Reference Photos, 5 My Best Shots, 6 Map,
            //   7 Download from iNaturalist, 8 Not in Any Tree.
            // Values are indices within the Reference Tree mode's QTabWidget.
            static const int kTreeTabIndex[] = {0, -1, -1, 3, 5, 2, 1, 6, 4};
            for (QWidget *w : QApplication::topLevelWidgets()) {
                auto *treeAction = w->findChild<QAction *>(QStringLiteral("viewTreeAction"));
                auto *libraryAction = w->findChild<QAction *>(QStringLiteral("viewLibraryAction"));
                auto *reviewAction = w->findChild<QAction *>(QStringLiteral("viewReviewAction"));
                auto *tabs = w->findChild<QTabWidget *>();
                if (tab == 1) {
                    if (libraryAction)
                        libraryAction->setChecked(true);
                } else if (tab == 2) {
                    if (reviewAction)
                        reviewAction->setChecked(true);
                } else {
                    if (treeAction)
                        treeAction->setChecked(true);
                    if (tabs && tab >= 0 && tab < 9 && kTreeTabIndex[tab] >= 0)
                        tabs->setCurrentIndex(kTreeTabIndex[tab]);
                }
                if (tab == 0) {
                    if (auto *tree = w->findChild<QTreeView *>()) {
                        tree->expandAll();
                        // select the last visible row (a species, deep in the tree)
                        QModelIndex idx = tree->model()->index(0, 0);
                        while (tree->model()->rowCount(idx) > 0)
                            idx = tree->model()->index(tree->model()->rowCount(idx) - 1, 0, idx);
                        if (idx.isValid())
                            tree->setCurrentIndex(idx);
                    }
                }
            }
            QTimer::singleShot(700, [&] {
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    if (w->findChild<QTabWidget *>()) {
                        w->grab().save(out);
                        break;
                    }
                }
                qtApp.quit();
            });
        });
        return QApplication::exec();
    }

    if (parser.isSet(matchOption))
        return runHeadlessMatch(app);

    if (parser.isSet(checklistOption))
        return runHeadlessChecklist(app, parser.value(checklistOption), parser.value(intoOption),
                                    parser.value(sourceOption));

    if (parser.isSet(lightroomOption))
        return runHeadlessLightroomImport(app, parser.value(lightroomOption),
                                          parser.value(intoOption));

    app.showMainWindow();
    return QApplication::exec();
}
