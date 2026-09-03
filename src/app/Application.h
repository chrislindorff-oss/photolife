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
namespace thumb {
class ThumbnailCache;
}
namespace net {
class HttpClient;
class INatClient;
}
namespace taxonomy {
class TaxonomyStore;
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
    thumb::ThumbnailCache &thumbnails();
    taxonomy::TaxonomyStore &taxonomyStore();
    net::INatClient &inat();

private:
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<Database> m_database;
    std::unique_ptr<thumb::ThumbnailCache> m_thumbnails;
    std::unique_ptr<scan::ScanService> m_scanService;
    std::unique_ptr<scan::LibraryWatcher> m_libraryWatcher;
    std::unique_ptr<taxonomy::TaxonomyStore> m_taxonomyStore;
    std::unique_ptr<net::HttpClient> m_http;
    std::unique_ptr<net::INatClient> m_inat;
    std::unique_ptr<MainWindow> m_mainWindow;
};

} // namespace pl
