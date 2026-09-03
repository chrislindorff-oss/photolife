#include <QApplication>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

#include "app/Application.h"
#include "db/Database.h"
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

    app.showMainWindow();
    return QApplication::exec();
}
