#pragma once

#include "db/CatalogueDescriptor.h"
#include "db/Database.h"

#include <QObject>

#include <memory>
#include <optional>

namespace pl {

class Database;
class Settings;
class SecretStore;
class MainWindow;

namespace scan {
class ScanService;
class LibraryWatcher;
}
namespace match {
class MatchService;
}
namespace collection {
class ExportService;
}
namespace lightroom {
class LightroomImporter;
}
namespace thumb {
class ThumbnailCache;
}
namespace net {
class HttpClient;
class INatClient;
class GeocodeClient;
class HttpCache;
class PhotoCache;
class TileCache;
class UpdateChecker;
}
namespace taxonomy {
class TaxonomyStore;
}
namespace inat {
class InatPhotoDownloader;
class PhotoIdentifier;
}
namespace sync {
class SyncService;
}
namespace libsync {
class PhotoSyncService;
}

// Owns process-wide state: settings, the open database, the thumbnail cache,
// the scan service, the main window. One instance, created in main() after
// QApplication.
class Application : public QObject
{
    Q_OBJECT

public:
    Application();
    ~Application() override;

    // The system keychain (nullptr: built without it, or a test run), and
    // what was moved into it from the settings file at this start-up.
    SecretStore *secretStore() const { return m_secrets.get(); }
    int secretsMoved() const { return m_secretsMoved; }
    QString secretsBackup() const { return m_secretsBackup; }

    // Sets application metadata, installs logging, loads settings, opens and
    // migrates the catalogue database. Returns false (and logs) on failure.
    // `onProgress`, if given, is called with a status line while connecting
    // to and migrating a shared (Postgres) catalogue, so a caller can show a
    // "please wait" UI that reflects real progress instead of a frozen splash.
    // `descriptorOverride`, if given, is opened instead of Settings's own
    // catalogueDescriptor() -- e.g. a caller that bailed out of a stalled
    // Postgres connect to open the local catalogue instead.
    // Whether start-up may ask things (where to keep a catalogue backup, what
    // to do when one fails): not for the command-line runs.
    void setInteractive(bool interactive) { m_interactive = interactive; }

    bool initialize(const Database::ProgressCallback &onProgress = {},
                     std::optional<CatalogueDescriptor> descriptorOverride = {});

    void showMainWindow();
    MainWindow *mainWindow() const { return m_mainWindow.get(); }

    Database &database();
    Settings &settings();
    scan::ScanService &scanService();
    scan::LibraryWatcher &libraryWatcher();
    match::MatchService &matchService();
    collection::ExportService &exportService();
    lightroom::LightroomImporter &lightroomImporter();
    thumb::ThumbnailCache &thumbnails();
    taxonomy::TaxonomyStore &taxonomyStore();
    net::INatClient &inat();
    // The same API at background priority, for quiet housekeeping (origins,
    // place details) that must never hold up a user's tree build or import.
    net::INatClient &inatBackground();
    // The iNaturalist API token (Settings::inatApiToken()): saved, given to
    // inat() (not the background client: authenticated calls skip the HTTP
    // cache), and announced.
    void setInatApiToken(const QString &token);
    // iNaturalist's computer vision, over inat(), with the session's answers.
    inat::PhotoIdentifier &photoIdentifier();
    // The rate-limited, caching HTTP client iNatClient uses -- for other
    // JSON APIs (e.g. the Atlas of Living Australia).
    net::HttpClient &http();
    net::GeocodeClient &geocoder();
    net::PhotoCache &photoCache();
    net::TileCache &tileCache();            // the street map (OpenStreetMap)
    net::TileCache &satelliteTileCache();   // aerial photos (Esri World Imagery)
    net::UpdateChecker &updateChecker();
    inat::InatPhotoDownloader &inatPhotoDownloader();

    // Shared-catalogue sync (Settings::syncEnabled()).
    sync::SyncService &syncService();
    // Whether this machine may change the catalogue: always for a local-only
    // catalogue; in sync mode only while holding the server's edit lock.
    bool catalogueEditable() const;
    // Points syncService() and photoSync() at the current settings (after
    // they change).
    void configureSync();
    // Photo sync for a Server catalogue whose photos PhotoLife keeps online
    // (Settings::CatalogueProfile::syncsPhotos()).
    libsync::PhotoSyncService &photoSync();

signals:
    void inatApiTokenChanged(const QString &token);

private:
    std::unique_ptr<SecretStore> m_secrets;   // before m_settings: it outlives it
    int m_secretsMoved = 0;                   // into the keychain at this start-up
    bool m_interactive = false;
    // Database::BeforeUpgrade for the Local catalogue: a backup first.
    bool backupBeforeUpgrade(const QString &connectionName, int fromVersion, int toVersion);
    QString m_secretsBackup;
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<Database> m_database;
    std::unique_ptr<thumb::ThumbnailCache> m_thumbnails;
    std::unique_ptr<scan::ScanService> m_scanService;
    std::unique_ptr<scan::LibraryWatcher> m_libraryWatcher;
    std::unique_ptr<match::MatchService> m_matchService;
    std::unique_ptr<collection::ExportService> m_exportService;
    std::unique_ptr<lightroom::LightroomImporter> m_lightroomImporter;
    std::unique_ptr<taxonomy::TaxonomyStore> m_taxonomyStore;
    std::unique_ptr<net::HttpCache> m_httpCache;
    std::unique_ptr<net::HttpClient> m_http;
    std::unique_ptr<net::INatClient> m_inat;
    std::unique_ptr<net::INatClient> m_inatBackground;
    std::unique_ptr<net::HttpClient> m_geocodeHttp;
    std::unique_ptr<net::GeocodeClient> m_geocoder;
    std::unique_ptr<net::PhotoCache> m_photoCache;
    std::unique_ptr<net::TileCache> m_tileCache;
    std::unique_ptr<net::TileCache> m_satelliteTileCache;
    std::unique_ptr<inat::PhotoIdentifier> m_photoIdentifier;
    std::unique_ptr<net::UpdateChecker> m_updateChecker;
    std::unique_ptr<inat::InatPhotoDownloader> m_inatPhotoDownloader;
    std::unique_ptr<sync::SyncService> m_syncService;
    std::unique_ptr<libsync::PhotoSyncService> m_photoSync;
    std::unique_ptr<MainWindow> m_mainWindow;
};

} // namespace pl
