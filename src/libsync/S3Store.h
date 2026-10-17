#pragma once

#include "libsync/BlockingHttp.h"
#include "libsync/LibraryStore.h"
#include "libsync/S3Config.h"
#include "libsync/SigV4.h"

#include <QUrl>

namespace pl::libsync {

// A photo library in an S3-compatible bucket (Cloudflare R2, Backblaze B2,
// MinIO, AWS S3). Photos live at "<prefix>/library/<relative path>", path
// style, each uploaded in one PUT so its ETag is the MD5 of its content; the
// SHA-256 and uploader's name ride along as object metadata.
class S3Store : public LibraryStore
{
public:
    explicit S3Store(S3Config config);

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
    bool remove(const RemoteEntry &entry) override;
    int parallelTransfers() const override { return 6; }
    std::unique_ptr<LibraryStore> clone() const override
    {
        auto copy = std::make_unique<S3Store>(m_config);
        copy->setCancel(cancelFn());
        return copy;
    }

    // The object key a library path is stored under.
    QString objectKey(const QString &relPath) const;

    // The request URL for `key` (empty: the bucket itself) and its encoded
    // path, which is what gets signed. Exposed so tests can check QUrl keeps
    // the encoding exactly as signed.
    struct Target
    {
        QUrl url;
        QByteArray encodedPath;
        QByteArray host;
    };
    Target target(const QString &key, const SigV4Query &query = {}) const;

private:
    BlockingHttp::Response call(const QByteArray &method, const QString &key,
                                const SigV4Query &query, SigV4Headers headers,
                                QIODevice *body, const QByteArray &payloadHash,
                                QIODevice *sink = nullptr, const ByteProgress &progress = {});
    bool fail(const QString &what, const BlockingHttp::Response &response);
    QString rootPrefix() const;      // "<prefix>/", or empty
    QString libraryPrefix() const;   // "<prefix>/library/"
    QString thumbsPrefix() const;    // "<prefix>/thumbs/"
    QString trashPrefix() const;     // "<prefix>/trash/"
    struct Listed
    {
        QString key;
        qint64 size = 0;
        QString etag;
    };
    std::optional<QList<Listed>> listUnder(const QString &prefix);

    S3Config m_config;
    BlockingHttp m_http;
    QString m_error;
};

} // namespace pl::libsync
