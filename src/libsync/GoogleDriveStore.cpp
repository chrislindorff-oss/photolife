#include "libsync/GoogleDriveStore.h"

#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

namespace pl::libsync {

namespace {

const QString kFolderMime = QStringLiteral("application/vnd.google-apps.folder");
const QString kFileFields = QStringLiteral(
    "id,name,mimeType,size,md5Checksum,headRevisionId,appProperties,lastModifyingUser(displayName)");

QJsonObject json(const QByteArray &body)
{
    return QJsonDocument::fromJson(body).object();
}

// Drive's query language: names are quoted with ', escaped with \.
QString quoted(QString value)
{
    value.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    value.replace(QLatin1Char('\''), QLatin1String("\\'"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QByteArray form(const QList<QPair<QString, QString>> &fields)
{
    QByteArray out;
    for (const auto &[name, value] : fields) {
        if (!out.isEmpty())
            out += '&';
        out += QUrl::toPercentEncoding(name) + '=' + QUrl::toPercentEncoding(value);
    }
    return out;
}

bool retryable(const BlockingHttp::Response &r)
{
    if (r.cancelled)
        return false;
    if (r.status == 0 || r.status == 429 || (r.status >= 500 && r.status <= 504))
        return true;
    return r.status == 403 && r.body.contains("ateLimitExceeded");   // rate/userRate
}

} // namespace

GoogleDriveStore::GoogleDriveStore(DriveConfig config) : m_config(std::move(config))
{
    m_config.folderId = DriveConfig::folderIdFrom(m_config.folderId);
}

QUrl GoogleDriveStore::apiUrl(const QString &path, const QList<QPair<QString, QString>> &query) const
{
    QByteArray encoded = m_config.apiBase.toUtf8() + path.toUtf8();
    if (!query.isEmpty())
        encoded += '?' + form(query);
    return QUrl::fromEncoded(encoded);
}

bool GoogleDriveStore::fail(const QString &what, const BlockingHttp::Response &response)
{
    QString detail = response.error;
    if (detail.isEmpty()) {
        const QJsonObject o = json(response.body);
        detail = o.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        if (detail.isEmpty())
            detail = o.value(QStringLiteral("error_description")).toString();
    }
    if (detail.isEmpty())
        detail = QStringLiteral("HTTP %1").arg(response.status);
    m_error = QStringLiteral("%1: %2").arg(what, detail);
    return false;
}

bool GoogleDriveStore::ensureToken(bool force)
{
    if (!force && !m_accessToken.isEmpty()
        && QDateTime::currentDateTimeUtc().secsTo(m_tokenExpiry) > 60)
        return true;
    if (!m_config.isComplete()) {
        m_error = QStringLiteral("Sign in with Google and choose the shared folder in Server Settings");
        return false;
    }
    QNetworkRequest request{QUrl(m_config.tokenUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    const auto response = m_http.send(
        request, "POST", nullptr,
        form({{QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
              {QStringLiteral("refresh_token"), m_config.refreshToken},
              {QStringLiteral("client_id"), m_config.clientId.trimmed()},
              {QStringLiteral("client_secret"), m_config.clientSecret.trimmed()}}),
        nullptr, {}, [this] { return cancelled(); });
    const QJsonObject o = json(response.body);
    if (!response.ok() || !o.contains(QStringLiteral("access_token"))) {
        if (o.value(QStringLiteral("error")).toString() == QLatin1String("invalid_grant")) {
            m_error = QStringLiteral("The Google sign-in has expired or was withdrawn — sign in "
                                     "again in Server Settings");
            return false;
        }
        return fail(QStringLiteral("Couldn't sign in to Google"), response);
    }
    m_accessToken = o.value(QStringLiteral("access_token")).toString();
    m_tokenExpiry = QDateTime::currentDateTimeUtc().addSecs(o.value(QStringLiteral("expires_in")).toInt(3600));
    return true;
}

BlockingHttp::Response GoogleDriveStore::send(const Call &call, const ByteProgress &progress)
{
    BlockingHttp::Response response;
    bool refreshed = false;
    for (int attempt = 0;; ++attempt) {
        if (!ensureToken()) {
            response = {};
            response.error = m_error;
            return response;
        }
        QNetworkRequest request(call.url);
        for (const auto &[name, value] : call.headers)
            request.setRawHeader(name, value);
        request.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
        if (call.device) {
            call.device->seek(0);
            request.setHeader(QNetworkRequest::ContentLengthHeader, call.device->size());
        }
        if (call.sink) {   // a retry starts the reply over
            call.sink->seek(0);
            if (auto *file = qobject_cast<QFile *>(call.sink))
                file->resize(0);
            else if (auto *buffer = qobject_cast<QBuffer *>(call.sink))
                buffer->buffer().clear();
        }
        response = m_http.send(request, call.verb, call.device, call.body, call.sink, progress,
                               [this] { return cancelled(); });
        if (response.status == 401 && !refreshed) {
            refreshed = true;
            if (!ensureToken(true))
                return response;
            continue;
        }
        if (attempt < 4 && retryable(response) && (!call.device || !call.device->isSequential())) {
            if (m_retryDelayMs > 0)
                QThread::msleep(ulong(m_retryDelayMs) << attempt);
            continue;
        }
        return response;
    }
}

bool GoogleDriveStore::testConnection()
{
    if (!m_config.canSignIn() || m_config.refreshToken.isEmpty()) {
        m_error = QStringLiteral("Sign in with Google first");
        return false;
    }
    if (m_config.folderId.isEmpty()) {
        m_error = QStringLiteral("Choose the shared Drive folder first");
        return false;
    }
    Call call;
    call.url = apiUrl(QStringLiteral("/drive/v3/files/") + m_config.folderId,
                      {{QStringLiteral("fields"), QStringLiteral("id,name,mimeType")},
                       {QStringLiteral("supportsAllDrives"), QStringLiteral("true")}});
    const auto response = send(call);
    if (!response.ok())
        return fail(QStringLiteral("Couldn't open the shared folder"), response);
    if (json(response.body).value(QStringLiteral("mimeType")).toString() != kFolderMime) {
        m_error = QStringLiteral("That's a file, not a folder");
        return false;
    }
    return true;
}

std::optional<QList<QJsonObject>> GoogleDriveStore::children(const QString &folderId,
                                                             const QString &filter)
{
    QList<QJsonObject> out;
    QString pageToken;
    do {
        QString q = QStringLiteral("%1 in parents and trashed = false").arg(quoted(folderId));
        if (!filter.isEmpty())
            q += QStringLiteral(" and ") + filter;
        QList<QPair<QString, QString>> query{
            {QStringLiteral("q"), q},
            {QStringLiteral("fields"), QStringLiteral("nextPageToken,files(%1)").arg(kFileFields)},
            {QStringLiteral("pageSize"), QStringLiteral("1000")},
            {QStringLiteral("supportsAllDrives"), QStringLiteral("true")},
            {QStringLiteral("includeItemsFromAllDrives"), QStringLiteral("true")}};
        if (!pageToken.isEmpty())
            query.append({QStringLiteral("pageToken"), pageToken});
        Call call;
        call.url = apiUrl(QStringLiteral("/drive/v3/files"), query);
        const auto response = send(call);
        if (!response.ok()) {
            fail(QStringLiteral("Couldn't list the Drive folder"), response);
            return std::nullopt;
        }
        const QJsonObject o = json(response.body);
        for (const QJsonValue &file : o.value(QStringLiteral("files")).toArray())
            out.append(file.toObject());
        pageToken = o.value(QStringLiteral("nextPageToken")).toString();
        if (cancelled()) {
            m_error = QStringLiteral("Cancelled");
            return std::nullopt;
        }
    } while (!pageToken.isEmpty());
    return out;
}

std::optional<QString> GoogleDriveStore::subfolder(const QString &parentId, const QString &name,
                                                   bool create)
{
    const auto found = children(parentId, QStringLiteral("name = %1 and mimeType = %2")
                                              .arg(quoted(name), quoted(kFolderMime)));
    if (!found)
        return std::nullopt;
    if (!found->isEmpty())
        return found->first().value(QStringLiteral("id")).toString();
    if (!create)
        return QString();

    Call call;
    call.verb = "POST";
    call.url = apiUrl(QStringLiteral("/drive/v3/files"),
                      {{QStringLiteral("fields"), QStringLiteral("id")},
                       {QStringLiteral("supportsAllDrives"), QStringLiteral("true")}});
    call.headers = {{"Content-Type", "application/json; charset=UTF-8"}};
    call.body = QJsonDocument(QJsonObject{{QStringLiteral("name"), name},
                                          {QStringLiteral("mimeType"), kFolderMime},
                                          {QStringLiteral("parents"), QJsonArray{parentId}}})
                    .toJson(QJsonDocument::Compact);
    const auto response = send(call);
    const QString id = json(response.body).value(QStringLiteral("id")).toString();
    if (!response.ok() || id.isEmpty()) {
        fail(QStringLiteral("Couldn't create the folder %1").arg(name), response);
        return std::nullopt;
    }
    return id;
}

// The id of `relDir` under library/ ("" for library/ itself); empty if it
// doesn't exist and `create` is false.
std::optional<QString> GoogleDriveStore::folderFor(const QString &relDir, bool create)
{
    if (const auto it = m_folders.constFind(relDir); it != m_folders.constEnd())
        return *it;
    QString parentId;
    if (relDir.isEmpty()) {
        parentId = m_config.folderId;
    } else {
        const int slash = relDir.lastIndexOf(QLatin1Char('/'));
        const auto parent = folderFor(slash < 0 ? QString() : relDir.left(slash), create);
        if (!parent || parent->isEmpty())
            return parent;
        parentId = *parent;
    }
    const QString name = relDir.isEmpty() ? QStringLiteral("library")
                                          : relDir.mid(relDir.lastIndexOf(QLatin1Char('/')) + 1);
    const auto id = subfolder(parentId, name, create);
    if (id && !id->isEmpty())
        m_folders.insert(relDir, *id);
    return id;
}

RemoteEntry GoogleDriveStore::entryFrom(const QJsonObject &file, const QString &relPath) const
{
    RemoteEntry e;
    e.relPath = relPath;
    e.remoteId = file.value(QStringLiteral("id")).toString();
    e.size = file.value(QStringLiteral("size")).toString().toLongLong();
    e.md5 = file.value(QStringLiteral("md5Checksum")).toString().toLower();
    e.version = file.value(QStringLiteral("headRevisionId")).toString();
    if (e.version.isEmpty())
        e.version = e.md5;
    const QJsonObject props = file.value(QStringLiteral("appProperties")).toObject();
    e.sha256 = props.value(QStringLiteral("sha256")).toString();
    e.uploadedBy = props.value(QStringLiteral("uploadedBy")).toString();
    if (e.uploadedBy.isEmpty())
        e.uploadedBy = file.value(QStringLiteral("lastModifyingUser"))
                           .toObject()
                           .value(QStringLiteral("displayName"))
                           .toString();
    return e;
}

std::optional<QList<RemoteEntry>> GoogleDriveStore::list()
{
    m_folders.clear();
    m_files.clear();
    QList<RemoteEntry> out;
    const auto root = folderFor(QString(), false);
    if (!root)
        return std::nullopt;
    if (root->isEmpty())
        return out;   // nothing uploaded yet

    QList<QString> pending{QString()};
    while (!pending.isEmpty()) {
        const QString dir = pending.takeFirst();
        const auto files = children(m_folders.value(dir));
        if (!files)
            return std::nullopt;
        for (const QJsonObject &file : *files) {
            const QString name = file.value(QStringLiteral("name")).toString();
            if (name.isEmpty() || name.contains(QLatin1Char('/')))
                continue;
            const QString rel = dir.isEmpty() ? name : dir + QLatin1Char('/') + name;
            if (file.value(QStringLiteral("mimeType")).toString() == kFolderMime) {
                if (!m_folders.contains(rel)) {
                    m_folders.insert(rel, file.value(QStringLiteral("id")).toString());
                    pending.append(rel);
                }
            } else if (!m_files.contains(rel)) {
                const RemoteEntry entry = entryFrom(file, rel);
                m_files.insert(rel, entry.remoteId);
                out.append(entry);
            }
        }
    }
    return out;
}

bool GoogleDriveStore::describe(RemoteEntry &entry)
{
    const QString id = entry.remoteId.isEmpty() ? m_files.value(entry.relPath) : entry.remoteId;
    if (id.isEmpty()) {
        m_error = QStringLiteral("%1 isn't in the Drive folder").arg(entry.relPath);
        return false;
    }
    Call call;
    call.url = apiUrl(QStringLiteral("/drive/v3/files/") + id,
                      {{QStringLiteral("fields"), kFileFields},
                       {QStringLiteral("supportsAllDrives"), QStringLiteral("true")}});
    const auto response = send(call);
    if (!response.ok())
        return fail(QStringLiteral("Couldn't look up %1").arg(entry.relPath), response);
    const RemoteEntry fresh = entryFrom(json(response.body), entry.relPath);
    entry.version = fresh.version;
    entry.md5 = fresh.md5;
    entry.sha256 = fresh.sha256;
    entry.uploadedBy = fresh.uploadedBy;
    entry.remoteId = fresh.remoteId;
    return true;
}

bool GoogleDriveStore::download(const RemoteEntry &entry, const QString &localPath,
                                const ByteProgress &progress)
{
    const QString id = entry.remoteId.isEmpty() ? m_files.value(entry.relPath) : entry.remoteId;
    QFile file(localPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_error = QStringLiteral("Couldn't write %1: %2").arg(localPath, file.errorString());
        return false;
    }
    Call call;
    call.url = apiUrl(QStringLiteral("/drive/v3/files/") + id,
                      {{QStringLiteral("alt"), QStringLiteral("media")},
                       {QStringLiteral("supportsAllDrives"), QStringLiteral("true")}});
    call.sink = &file;
    const auto response = send(call, progress);
    file.close();
    if (!response.ok())
        return fail(QStringLiteral("Couldn't download %1").arg(entry.relPath), response);
    return true;
}

// A resumable upload: the metadata first, then the content to the session
// URL Drive hands back -- one request for any size of file.
std::optional<QJsonObject> GoogleDriveStore::sendContent(const QString &existingId,
                                                         const QString &parentId,
                                                         const QString &name, QIODevice *content,
                                                         const QJsonObject &appProperties,
                                                         const ByteProgress &progress)
{
    const QList<QPair<QString, QString>> query{
        {QStringLiteral("uploadType"), QStringLiteral("resumable")},
        {QStringLiteral("supportsAllDrives"), QStringLiteral("true")},
        {QStringLiteral("fields"), QStringLiteral("id,size,md5Checksum,headRevisionId")}};
    QJsonObject meta{{QStringLiteral("appProperties"), appProperties}};
    Call start;
    if (existingId.isEmpty()) {
        start.verb = "POST";
        start.url = apiUrl(QStringLiteral("/upload/drive/v3/files"), query);
        meta.insert(QStringLiteral("name"), name);
        meta.insert(QStringLiteral("parents"), QJsonArray{parentId});
    } else {
        start.verb = "PATCH";
        start.url = apiUrl(QStringLiteral("/upload/drive/v3/files/") + existingId, query);
    }
    start.headers = {{"Content-Type", "application/json; charset=UTF-8"},
                     {"X-Upload-Content-Type", "application/octet-stream"},
                     {"X-Upload-Content-Length", QByteArray::number(content->size())}};
    start.body = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    const auto started = send(start);
    const QByteArray location = started.header("Location");
    if (!started.ok() || location.isEmpty()) {
        fail(QStringLiteral("Couldn't start uploading %1").arg(name), started);
        return std::nullopt;
    }

    Call put;
    put.verb = "PUT";
    put.url = QUrl::fromEncoded(location);
    put.headers = {{"Content-Type", "application/octet-stream"}};
    put.device = content;
    const auto done = send(put, progress);
    const QJsonObject file = json(done.body);
    if (!done.ok() || file.value(QStringLiteral("id")).toString().isEmpty()) {
        fail(QStringLiteral("Couldn't upload %1").arg(name), done);
        return std::nullopt;
    }
    return file;
}

std::optional<RemoteEntry> GoogleDriveStore::upload(const QString &localPath, const QString &relPath,
                                                    const QString &md5, const QString &sha256,
                                                    const QString &uploadedBy,
                                                    const ByteProgress &progress)
{
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("Couldn't read %1: %2").arg(localPath, file.errorString());
        return std::nullopt;
    }
    const int slash = relPath.lastIndexOf(QLatin1Char('/'));
    const QString dir = slash < 0 ? QString() : relPath.left(slash);
    const QString name = relPath.mid(slash + 1);
    const QString existing = m_files.value(relPath);
    QString parent;
    if (existing.isEmpty()) {
        const auto folder = folderFor(dir, true);
        if (!folder)
            return std::nullopt;
        parent = *folder;
    }
    const auto sent = sendContent(existing, parent, name, &file,
                                  {{QStringLiteral("sha256"), sha256},
                                   {QStringLiteral("uploadedBy"), uploadedBy}},
                                  progress);
    if (!sent)
        return std::nullopt;
    RemoteEntry entry = entryFrom(*sent, relPath);
    if (!entry.md5.isEmpty() && entry.md5 != md5.toLower()) {
        m_error = QStringLiteral("Couldn't upload %1: Drive received something different").arg(relPath);
        return std::nullopt;
    }
    entry.md5 = md5.toLower();
    entry.sha256 = sha256.toLower();
    entry.uploadedBy = uploadedBy;
    entry.size = file.size();
    m_files.insert(relPath, entry.remoteId);
    return entry;
}

std::optional<QString> GoogleDriveStore::thumbsFolder(bool create)
{
    if (m_thumbsFolder && (!m_thumbsFolder->isEmpty() || !create))
        return m_thumbsFolder;
    const auto id = subfolder(m_config.folderId, QStringLiteral("thumbs"), create);
    if (id)
        m_thumbsFolder = id;
    return id;
}

std::optional<QSet<QString>> GoogleDriveStore::listThumbnails()
{
    m_thumbs.clear();
    QSet<QString> out;
    const auto folder = thumbsFolder(false);
    if (!folder)
        return std::nullopt;
    m_thumbsListed = true;
    if (folder->isEmpty())
        return out;
    const auto files = children(*folder);
    if (!files)
        return std::nullopt;
    for (const QJsonObject &file : *files) {
        QString name = file.value(QStringLiteral("name")).toString();
        if (!name.endsWith(QLatin1String(".jpg")))
            continue;
        name.chop(4);
        if (!m_thumbs.contains(name))
            m_thumbs.insert(name, file.value(QStringLiteral("id")).toString());
        out.insert(name);
    }
    return out;
}

bool GoogleDriveStore::putThumbnail(const QString &name, const QByteArray &jpeg)
{
    if (!m_thumbsListed && !listThumbnails())
        return false;
    const auto folder = thumbsFolder(true);
    if (!folder)
        return false;
    QBuffer body;
    body.setData(jpeg);
    body.open(QIODevice::ReadOnly);
    const auto sent = sendContent(m_thumbs.value(name), *folder, name + QStringLiteral(".jpg"),
                                  &body, {}, {});
    if (!sent)
        return false;
    m_thumbs.insert(name, sent->value(QStringLiteral("id")).toString());
    return true;
}

std::optional<QByteArray> GoogleDriveStore::getThumbnail(const QString &name)
{
    if (!m_thumbsListed && !listThumbnails())
        return std::nullopt;
    const QString id = m_thumbs.value(name);
    if (id.isEmpty()) {
        m_error = QStringLiteral("No thumbnail %1 online").arg(name);
        return std::nullopt;
    }
    QBuffer sink;
    sink.open(QIODevice::WriteOnly);
    Call call;
    call.url = apiUrl(QStringLiteral("/drive/v3/files/") + id,
                      {{QStringLiteral("alt"), QStringLiteral("media")},
                       {QStringLiteral("supportsAllDrives"), QStringLiteral("true")}});
    call.sink = &sink;
    const auto response = send(call);
    if (!response.ok()) {
        fail(QStringLiteral("Couldn't download a thumbnail"), response);
        return std::nullopt;
    }
    return sink.data();
}

} // namespace pl::libsync
