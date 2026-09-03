#include "db/Database.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>

namespace pl {
namespace {

// Migrations live at :/migrations/NNN_description.sql and are applied in
// ascending NNN order. NNN is the schema version the script produces.
struct Migration
{
    int version;
    QString resourcePath;
};

QList<Migration> discoverMigrations()
{
    QList<Migration> migrations;
    const QRegularExpression pattern(QStringLiteral("^(\\d+)_.*\\.sql$"));

    QDir dir(QStringLiteral(":/migrations"));
    const QStringList entries = dir.entryList(QStringList{QStringLiteral("*.sql")},
                                              QDir::Files, QDir::Name);
    for (const QString &name : entries) {
        const QRegularExpressionMatch m = pattern.match(name);
        if (!m.hasMatch())
            continue;
        migrations.push_back({m.captured(1).toInt(), dir.filePath(name)});
    }

    std::sort(migrations.begin(), migrations.end(),
              [](const Migration &a, const Migration &b) { return a.version < b.version; });
    return migrations;
}

// Splits a migration script into individual statements. Our migration SQL is
// authored without semicolons inside string literals or triggers, so a plain
// split on ';' terminators is sufficient and predictable.
QStringList splitStatements(const QString &sql)
{
    QStringList statements;
    QString current;
    for (const QString &rawLine : sql.split(QLatin1Char('\n'))) {
        QString line = rawLine;
        const int comment = line.indexOf(QStringLiteral("--"));
        if (comment >= 0)
            line.truncate(comment);
        current += line;
        current += QLatin1Char('\n');
        if (line.trimmed().endsWith(QLatin1Char(';'))) {
            const QString trimmed = current.trimmed();
            if (!trimmed.isEmpty())
                statements << trimmed;
            current.clear();
        }
    }
    if (!current.trimmed().isEmpty())
        statements << current.trimmed();
    return statements;
}

int nextConnectionSuffix()
{
    static std::atomic_int counter{0};
    return counter.fetch_add(1);
}

} // namespace

Database::Database()
    : m_connectionName(QStringLiteral("photolife-catalogue-%1").arg(nextConnectionSuffix()))
{
}

Database::~Database()
{
    close();
}

int Database::targetSchemaVersion()
{
    const QList<Migration> migrations = discoverMigrations();
    return migrations.isEmpty() ? 0 : migrations.last().version;
}

bool Database::fail(const QString &context, const QString &detail)
{
    m_error = detail.isEmpty() ? context : QStringLiteral("%1: %2").arg(context, detail);
    return false;
}

bool Database::open(const QString &path)
{
    close();
    m_error.clear();
    m_path = path;

    if (!path.isEmpty() && path != QStringLiteral(":memory:")) {
        const QFileInfo info(path);
        if (!QDir().mkpath(info.absolutePath()))
            return fail(QStringLiteral("Cannot create %1").arg(info.absolutePath()), {});
    }

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    m_connectionName);
        db.setDatabaseName(path);
        if (!db.open())
            return fail(QStringLiteral("Cannot open %1").arg(path), db.lastError().text());

        QSqlQuery pragma(db);
        pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
        pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
        pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    }

    if (!applyPendingMigrations()) {
        close();
        return false;
    }

    return true;
}

void Database::close()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            if (db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool Database::isOpen() const
{
    return QSqlDatabase::contains(m_connectionName)
           && QSqlDatabase::database(m_connectionName, false).isOpen();
}

int Database::schemaVersion() const
{
    if (!isOpen())
        return -1;
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next())
        return -1;
    return query.value(0).toInt();
}

bool Database::runScript(const QString &sql)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    for (const QString &statement : splitStatements(sql)) {
        QSqlQuery query(db);
        if (!query.exec(statement)) {
            return fail(QStringLiteral("Statement failed"),
                        QStringLiteral("%1\n%2").arg(query.lastError().text(), statement));
        }
    }
    return true;
}

bool Database::applyPendingMigrations()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    const int current = schemaVersion();
    if (current < 0)
        return fail(QStringLiteral("Cannot read schema version"), {});

    const QList<Migration> migrations = discoverMigrations();
    for (const Migration &migration : migrations) {
        if (migration.version <= current)
            continue;

        QFile file(migration.resourcePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return fail(QStringLiteral("Cannot read migration %1").arg(migration.resourcePath),
                        {});
        }
        const QString sql = QString::fromUtf8(file.readAll());

        if (!db.transaction())
            return fail(QStringLiteral("Cannot begin transaction"), db.lastError().text());

        if (!runScript(sql)) {
            db.rollback();
            return false;
        }

        QSqlQuery bump(db);
        if (!bump.exec(QStringLiteral("PRAGMA user_version = %1").arg(migration.version))) {
            db.rollback();
            return fail(QStringLiteral("Cannot set user_version"), bump.lastError().text());
        }

        if (!db.commit())
            return fail(QStringLiteral("Cannot commit migration %1").arg(migration.version),
                        db.lastError().text());
    }

    return true;
}

} // namespace pl
