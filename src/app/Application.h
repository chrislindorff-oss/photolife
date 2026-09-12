#pragma once

#include <QObject>

#include <memory>

namespace pl {

class Database;
class Settings;
class MainWindow;

namespace scan {
class ScanService;
class LibraryWatcher;
}
namespace match {
class MatchService;
}
namespace thumb {
class ThumbnailCache;
}
namespace net {
class HttpClient;
class INatClient;
class GeocodeClient;
class PhotoCache;
class TileCache;
class UpdateChecker;
}
namespace taxonomy {
class TaxonomyStore;
}
namespace inat {
class InatPhotoDownloader;
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

    // Sets application metadata, installs logging, loads settings, opens and
    // migrates the catalogue database. Returns false (and logs) on failure.
    bool initialize();

    void showMainWindow();

    Database &database();
    Settings &settings();
    scan::ScanService &scanService();
    scan::LibraryWatcher &libraryWatcher();
    match::MatchService &matchService();
    thumb::ThumbnailCache &thumbnails();
    taxonomy::TaxonomyStore &taxonomyStore();
    net::INatClient &inat();
    net::GeocodeClient &geocoder();
    net::PhotoCache &photoCache();
    net::TileCache &tileCache();
    net::UpdateChecker &updateChecker();
    inat::InatPhotoDownloader &inatPhotoDownloader();

private:
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<Database> m_database;
    std::unique_ptr<thumb::ThumbnailCache> m_thumbnails;
    std::unique_ptr<scan::ScanService> m_scanService;
    std::unique_ptr<scan::LibraryWatcher> m_libraryWatcher;
    std::unique_ptr<match::MatchService> m_matchService;
    std::unique_ptr<taxonomy::TaxonomyStore> m_taxonomyStore;
    std::unique_ptr<net::HttpClient> m_http;
    std::unique_ptr<net::INatClient> m_inat;
    std::unique_ptr<net::HttpClient> m_geocodeHttp;
    std::unique_ptr<net::GeocodeClient> m_geocoder;
    std::unique_ptr<net::PhotoCache> m_photoCache;
    std::unique_ptr<net::TileCache> m_tileCache;
    std::unique_ptr<net::UpdateChecker> m_updateChecker;
    std::unique_ptr<inat::InatPhotoDownloader> m_inatPhotoDownloader;
    std::unique_ptr<MainWindow> m_mainWindow;
};

} // namespace pl
