#pragma once

#include <QObject>

#include <memory>

namespace pl {

class Database;
class Settings;
class MainWindow;

// Owns process-wide state: settings, the open database, the main window.
// One instance, created in main() after QApplication.
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

private:
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<Database> m_database;
    std::unique_ptr<MainWindow> m_mainWindow;
};

} // namespace pl
