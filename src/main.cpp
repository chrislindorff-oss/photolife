#include <QApplication>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

#include "app/Application.h"
#include "scan/ScanService.h"
#include "scan/ScanTypes.h"

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
    parser.process(qtApp);

    pl::Application app;
    if (!app.initialize())
        return 1;

    if (parser.isSet(scanOption))
        return runHeadlessScan(app, parser.values(scanOption));

    app.showMainWindow();
    return QApplication::exec();
}
