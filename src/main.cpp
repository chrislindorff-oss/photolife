#include <QApplication>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QTimer>
#include <QWidget>
#include <QTabWidget>
#include <QTreeView>

#include <cstdio>

#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "app/Application.h"
#include "checklist/ChecklistImporter.h"
#include "checklist/ChecklistParser.h"
#include "db/Database.h"
#include "match/MatchService.h"
#include "scan/ScanService.h"
#include "scan/ScanTypes.h"
#include "taxonomy/ProjectBuilder.h"
#include "taxonomy/TaxonomyStore.h"

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

} // namespace

int main(int argc, char *argv[])
{
    QApplication qtApp(argc, argv);

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
        QStringLiteral("into"), QStringLiteral("Target project name for --import-checklist."),
        QStringLiteral("project"));
    const QCommandLineOption sourceOption(
        QStringLiteral("source"), QStringLiteral("Status source label for --import-checklist."),
        QStringLiteral("label"));
    parser.addOption(checklistOption);
    parser.addOption(intoOption);
    parser.addOption(sourceOption);

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

    pl::Application app;
    if (!app.initialize())
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
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (auto *tabs = w->findChild<QTabWidget *>())
                    tabs->setCurrentIndex(tab);
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

    app.showMainWindow();
    return QApplication::exec();
}
