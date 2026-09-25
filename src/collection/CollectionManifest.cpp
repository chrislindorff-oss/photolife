#include "collection/CollectionManifest.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace pl::collection {

namespace {
constexpr int kFormat = 1;
}

std::optional<CollectionManifest> CollectionManifest::read(const QString &collectionRoot)
{
    QFile file(QDir(collectionRoot).filePath(QLatin1String(kManifestFileName)));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return std::nullopt;
    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("format")).toInt() != kFormat
        || !obj.value(QStringLiteral("projectId")).isDouble())
        return std::nullopt;

    CollectionManifest m;
    m.projectId = obj.value(QStringLiteral("projectId")).toInt();
    m.projectName = obj.value(QStringLiteral("projectName")).toString();
    m.includeCommonName = obj.value(QStringLiteral("includeCommonName")).toBool();
    m.createdAt = QDateTime::fromString(obj.value(QStringLiteral("createdAt")).toString(),
                                        Qt::ISODate);
    m.updatedAt = QDateTime::fromString(obj.value(QStringLiteral("updatedAt")).toString(),
                                        Qt::ISODate);
    return m;
}

bool CollectionManifest::write(const QString &collectionRoot) const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("format"), kFormat);
    obj.insert(QStringLiteral("projectId"), projectId);
    obj.insert(QStringLiteral("projectName"), projectName);
    obj.insert(QStringLiteral("includeCommonName"), includeCommonName);
    obj.insert(QStringLiteral("createdAt"), createdAt.toUTC().toString(Qt::ISODate));
    obj.insert(QStringLiteral("updatedAt"), updatedAt.toUTC().toString(Qt::ISODate));

    QSaveFile file(QDir(collectionRoot).filePath(QLatin1String(kManifestFileName)));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace pl::collection
