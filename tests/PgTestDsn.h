#pragma once

#include "db/CatalogueDescriptor.h"

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <optional>

namespace pl::test {

// Postgres-backed test cases are opt-in: they need a real server, so they're
// skipped unless PHOTOLIFE_TEST_PG_DSN is set. Format is libpq-style
// space-separated key=value pairs, e.g.
//   PHOTOLIFE_TEST_PG_DSN="host=localhost port=5432 dbname=photolife_test user=photolife_test password=photolife_test"
inline std::optional<CatalogueDescriptor> pgTestDescriptorFromEnv()
{
    const QString dsn = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("PHOTOLIFE_TEST_PG_DSN"));
    if (dsn.trimmed().isEmpty())
        return std::nullopt;

    CatalogueDescriptor descriptor;
    descriptor.backend = CatalogueDescriptor::Backend::Postgres;
    descriptor.pgSslMode = QStringLiteral("prefer");

    for (const QString &token : dsn.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const int eq = token.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString key = token.left(eq);
        const QString value = token.mid(eq + 1);
        if (key == QStringLiteral("host"))
            descriptor.pgHost = value;
        else if (key == QStringLiteral("port"))
            descriptor.pgPort = value.toInt();
        else if (key == QStringLiteral("dbname"))
            descriptor.pgDbName = value;
        else if (key == QStringLiteral("user"))
            descriptor.pgUser = value;
        else if (key == QStringLiteral("password"))
            descriptor.pgPassword = value;
        else if (key == QStringLiteral("sslmode"))
            descriptor.pgSslMode = value;
    }
    return descriptor;
}

} // namespace pl::test
