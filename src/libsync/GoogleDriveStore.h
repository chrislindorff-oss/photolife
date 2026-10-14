#pragma once

#include "libsync/BlockingHttp.h"
#include "libsync/DriveConfig.h"
#include "libsync/LibraryStore.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>

namespace pl::libsync {

// A photo library in a shared Google Drive folder, through the Drive v3 API.
// Laid out like the S3 store: photos under "library/" (as ordinary folders
// and files, so the Drive website shows them as they are) and thumbnails
// under "thumbs/". Drive reports each file's MD5, so listing needs no
// per-file requests; a file's version is its head revision, which changes
// only when its content does. The SHA-256 and uploader's name go in the
// file's appProperties.
//
// Folders are walked one listing per folder. Names containing '/' (which
// Drive allows) are skipped, and of two files with the same name in one
// folder only the first is used.
class GoogleDriveStore : public LibraryStore
{
public:
    explicit GoogleDriveStore(DriveConfig config);

    QString error() const override { return m_error; }
    bool testConnection() override;
    std::optional<QList<RemoteEntry>> list() override;
    bool describe(RemoteEntry &entry) override;
    bool download(const RemoteEntry &entry, const QString &localPath,
                  const ByteProgress &progress = {}) override;
    std::optional<RemoteEntry> upload(const QString &localPath, const QString &relPath,
                                      const QString &md5, const QString &sha256,
                                      const QString &uploadedBy,
                                      const ByteProgress &progress = {}) override;
    std::optional<QSet<QString>> listThumbnails() override;
    bool putThumbnail(const QString &name, const QByteArray &jpeg) override;
    std::optional<QByteArray> getThumbnail(const QString &name) override;

    // Waits between retries of a rate-limited or failed request (tests set 0).
    void setRetryDelayMs(int ms) { m_retryDelayMs = ms; }

private:
    struct Call
    {
        QByteArray verb = "GET";
        QUrl url;
        QList<QPair<QByteArray, QByteArray>> headers;
        QByteArray body;
        QIODevice *device = nullptr;   // the body, instead of `body`
        QIODevice *sink = nullptr;     // stream the reply here
    };
    BlockingHttp::Response send(const Call &call, const ByteProgress &progress = {});
    bool ensureToken(bool force = false);
    bool fail(const QString &what, const BlockingHttp::Response &response);
    QUrl apiUrl(const QString &path, const QList<QPair<QString, QString>> &query = {}) const;
    std::optional<QList<QJsonObject>> children(const QString &folderId, const QString &filter = {});
    std::optional<QString> subfolder(const QString &parentId, const QString &name, bool create);
    std::optional<QString> folderFor(const QString &relDir, bool create);
    std::optional<QString> thumbsFolder(bool create);
    std::optional<QJsonObject> sendContent(const QString &existingId, const QString &parentId,
                                           const QString &name, QIODevice *content,
                                           const QJsonObject &appProperties,
                                           const ByteProgress &progress);
    RemoteEntry entryFrom(const QJsonObject &file, const QString &relPath) const;

    DriveConfig m_config;
    BlockingHttp m_http;
    QString m_error;
    QString m_accessToken;
    QDateTime m_tokenExpiry;
    int m_retryDelayMs = 1000;
    QHash<QString, QString> m_folders;   // folder under library/ ("" = library itself) -> id
    QHash<QString, QString> m_files;     // relative path -> file id
    std::optional<QString> m_thumbsFolder;
    QHash<QString, QString> m_thumbs;    // thumbnail name -> file id
    bool m_thumbsListed = false;
};

} // namespace pl::libsync
