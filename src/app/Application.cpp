#include "app/Application.h"

#include "app/Logging.h"
#include "db/Database.h"
#include "pl/Version.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "match/MatchService.h"
#include "net/QtNetworkTransport.h"
#include "net/UpdateChecker.h"
#include "scan/LibraryWatcher.h"
#include "scan/ScanService.h"
#include "settings/Settings.h"
#include "taxonomy/TaxonomyStore.h"
#include "thumb/ThumbnailCache.h"
#include "ui/MainWindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QLoggingCategory>
#include <QStandardPaths>

namespace pl {

Application::Application() = default;

Application::~Application() = default;

bool Application::initialize()
{
    QCoreApplication::setApplicationName(QString::fromLatin1(kAppName));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(kAppVersion));
    QCoreApplication::setOrganizationName(QString::fromLatin1(kOrgName));
    QCoreApplication::setOrganizationDomain(QString::fromLatin1(kOrgDomain));

    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);

    logging::install(QDir(dataDir).filePath(QStringLiteral("logs")));
    qInfo("%s %s starting", kAppName, kAppVersion);
    qInfo() << "Data directory:" << dataDir;

    m_settings = std::make_unique<Settings>();

    m_database = std::make_unique<Database>();
    const QString dbPath = m_settings->databasePath();
    if (!m_database->open(dbPath)) {
        qCritical() << "Could not open catalogue database at" << dbPath << ":"
                    << m_database->error();
        return false;
    }
    qInfo() << "Catalogue open:" << dbPath
            << "(schema v" << m_database->schemaVersion() << ")";

    const QString thumbDir = QDir(dataDir).filePath(QStringLiteral("thumbnails"));
    m_thumbnails = std::make_unique<thumb::ThumbnailCache>(thumbDir);

    m_scanService = std::make_unique<scan::ScanService>(dbPath);

    m_libraryWatcher = std::make_unique<scan::LibraryWatcher>(*m_database);
    m_libraryWatcher->setRoots(m_settings->watchedRoots());

    m_matchService = std::make_unique<match::MatchService>(dbPath);

    m_taxonomyStore = std::make_unique<taxonomy::TaxonomyStore>(m_database->connectionName());
    m_http = std::make_unique<net::HttpClient>(
        std::make_unique<net::QtNetworkTransport>(), m_taxonomyStore.get());
    m_http->setUserAgent(QStringLiteral("%1/%2 (+%3)")
                             .arg(QString::fromLatin1(kAppName),
                                  QString::fromLatin1(kAppVersion),
                                  QString::fromLatin1(kOrgDomain))
                             .toUtf8());
    m_inat = std::make_unique<net::INatClient>(*m_http);

    m_updateChecker = std::make_unique<net::UpdateChecker>(*m_http);
    m_updateChecker->setRepo(QString::fromLatin1(kReleasesRepo));
    m_updateChecker->setCurrentVersion(QString::fromLatin1(kAppVersionFull));

    return true;
}

void Application::showMainWindow()
{
    if (!m_mainWindow)
        m_mainWindow = std::make_unique<MainWindow>(*this);
    m_mainWindow->show();
}

Database &Application::database()
{
    return *m_database;
}

Settings &Application::settings()
{
    return *m_settings;
}

scan::ScanService &Application::scanService()
{
    return *m_scanService;
}

scan::LibraryWatcher &Application::libraryWatcher()
{
    return *m_libraryWatcher;
}

match::MatchService &Application::matchService()
{
    return *m_matchService;
}

thumb::ThumbnailCache &Application::thumbnails()
{
    return *m_thumbnails;
}

taxonomy::TaxonomyStore &Application::taxonomyStore()
{
    return *m_taxonomyStore;
}

net::INatClient &Application::inat()
{
    return *m_inat;
}

net::UpdateChecker &Application::updateChecker()
{
    return *m_updateChecker;
}

} // namespace pl
