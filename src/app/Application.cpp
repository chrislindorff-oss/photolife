#include "app/Application.h"

#include "app/Logging.h"
#include "collection/ExportService.h"
#include "db/CatalogueDescriptor.h"
#include "db/Database.h"
#include "pl/Version.h"
#include "inat/InatPhotoDownloader.h"
#include "lightroom/LightroomImporter.h"
#include "net/GeocodeClient.h"
#include "net/HttpCache.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "net/PhotoCache.h"
#include "net/TileCache.h"
#include "match/MatchService.h"
#include "net/QtNetworkTransport.h"
#include "net/UpdateChecker.h"
#include "raw/RawPreview.h"
#include "scan/LibraryWatcher.h"
#include "scan/ScanService.h"
#include "settings/Settings.h"
#include "sync/SyncService.h"
#include "taxonomy/TaxonomyStore.h"
#include "thumb/ThumbnailCache.h"
#include "ui/MainWindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QLoggingCategory>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSysInfo>

namespace pl {

Application::Application() = default;

Application::~Application() = default;

bool Application::initialize(const Database::ProgressCallback &onProgress,
                              std::optional<CatalogueDescriptor> descriptorOverride)
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
    if (m_settings->migrateProfiles())
        qInfo() << "Settings moved to catalogue profiles; active:" << m_settings->activeProfile().name;
    // A server's working copy lives in its own folder (data/servers/<id>/).
    QDir().mkpath(QFileInfo(m_settings->databasePath()).absolutePath());
    qInfo() << "Catalogue:" << m_settings->activeProfile().name;

    m_syncService = std::make_unique<sync::SyncService>();
    configureSync();
    // A catalogue staged last session -- downloaded from the shared server,
    // a discard, or a restored backup -- replaces this one now, before
    // anything opens it. The one it replaces is kept as a backup.
    if (sync::SyncService::applyPendingPull(m_settings->databasePath()))
        qInfo() << "Switched to the catalogue staged last session (previous one kept as a backup)";

    m_database = std::make_unique<Database>();
    const CatalogueDescriptor descriptor =
        descriptorOverride ? *descriptorOverride : m_settings->catalogueDescriptor();
    if (!m_database->open(descriptor, onProgress)) {
        qCritical() << "Could not open catalogue database:" << m_database->error();
        return false;
    }
    qInfo() << "Catalogue open:"
            << (descriptor.backend == CatalogueDescriptor::Backend::Postgres
                    ? QStringLiteral("postgres://%1@%2:%3/%4")
                          .arg(descriptor.pgUser, descriptor.pgHost)
                          .arg(descriptor.pgPort)
                          .arg(descriptor.pgDbName)
                    : descriptor.sqlitePath)
            << "(schema v" << m_database->schemaVersion() << ")";

    const QString thumbDir = QDir(dataDir).filePath(QStringLiteral("thumbnails"));
    m_thumbnails = std::make_unique<thumb::ThumbnailCache>(thumbDir);
    raw::installRawLoader(*m_thumbnails);
    qInfo() << "RAW previews:" << (raw::isAvailable() ? "enabled (LibRaw)" : "unavailable");

    m_scanService = std::make_unique<scan::ScanService>(descriptor);

    m_libraryWatcher = std::make_unique<scan::LibraryWatcher>(*m_database);
    m_libraryWatcher->setRoots(m_settings->watchedRoots());

    m_matchService = std::make_unique<match::MatchService>(descriptor);

    m_exportService = std::make_unique<collection::ExportService>(descriptor);

    m_lightroomImporter = std::make_unique<lightroom::LightroomImporter>(descriptor);

    m_taxonomyStore = std::make_unique<taxonomy::TaxonomyStore>(m_database->connectionName());
    // Kept out of the catalogue: it's disposable, and inside the catalogue it
    // grew to gigabytes of API responses that were never revalidated.
    m_httpCache = std::make_unique<net::HttpCache>();
    m_httpCache->open(
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("http-cache.db")));
    m_http = std::make_unique<net::HttpClient>(std::make_unique<net::QtNetworkTransport>(),
                                               m_httpCache.get());
    const QByteArray userAgent = QStringLiteral("%1/%2 (+%3)")
                                     .arg(QString::fromLatin1(kAppName),
                                          QString::fromLatin1(kAppVersion),
                                          QString::fromLatin1(kOrgDomain))
                                     .toUtf8();
    m_http->setUserAgent(userAgent);
    m_inat = std::make_unique<net::INatClient>(*m_http);

    // A second, independently-throttled HttpClient for Nominatim: its usage
    // policy (max 1 request/second) is stricter than -- and unrelated to --
    // iNaturalist's, so it gets its own queue rather than sharing m_http's.
    // No conditional-GET cache needed here: geocode_cache already dedupes by
    // rounded coordinate before a request is ever made.
    m_geocodeHttp =
        std::make_unique<net::HttpClient>(std::make_unique<net::QtNetworkTransport>(), nullptr);
    m_geocodeHttp->setUserAgent(userAgent);
    m_geocodeHttp->setMinRequestIntervalMs(1000);
    m_geocoder = std::make_unique<net::GeocodeClient>(*m_geocodeHttp);

    m_photoCache = std::make_unique<net::PhotoCache>(
        QDir(dataDir).filePath(QStringLiteral("reference-photos")), userAgent);

    m_tileCache = std::make_unique<net::TileCache>(
        QDir(dataDir).filePath(QStringLiteral("tiles")), userAgent);

    m_inatPhotoDownloader = std::make_unique<inat::InatPhotoDownloader>(userAgent);

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

collection::ExportService &Application::exportService()
{
    return *m_exportService;
}

lightroom::LightroomImporter &Application::lightroomImporter()
{
    return *m_lightroomImporter;
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

net::HttpClient &Application::http()
{
    return *m_http;
}

net::GeocodeClient &Application::geocoder()
{
    return *m_geocoder;
}

net::PhotoCache &Application::photoCache()
{
    return *m_photoCache;
}

net::TileCache &Application::tileCache()
{
    return *m_tileCache;
}

net::UpdateChecker &Application::updateChecker()
{
    return *m_updateChecker;
}

inat::InatPhotoDownloader &Application::inatPhotoDownloader()
{
    return *m_inatPhotoDownloader;
}

sync::SyncService &Application::syncService()
{
    return *m_syncService;
}

bool Application::catalogueEditable() const
{
    return !m_settings->syncEnabled() || m_settings->syncEditing();
}

void Application::configureSync()
{
    sync::SyncService::Config config;
    config.databasePath = m_settings->databasePath();
    config.server = m_settings->syncServerDescriptor();
    config.identity = {m_settings->syncUserName(), QSysInfo::machineHostName()};
    config.libraryRoot = m_settings->libraryRoot();
    m_syncService->setConfig(config);
}

} // namespace pl
