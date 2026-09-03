#include "app/Application.h"

#include "app/Logging.h"
#include "db/Database.h"
#include "pl/Version.h"
#include "settings/Settings.h"
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

} // namespace pl
