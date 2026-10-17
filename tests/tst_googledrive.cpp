#include <QtTest>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>

#include "libsync/GoogleAuth.h"
#include "libsync/GoogleDriveStore.h"
#include "libsync/LibraryState.h"
#include "libsync/LibrarySync.h"

using namespace pl::libsync;

namespace {

const QString kFolderMime = QStringLiteral("application/vnd.google-apps.folder");

// Just enough of Google's token endpoint and the Drive v3 API, served from
// this process, to drive GoogleAuth and GoogleDriveStore as Google would.
class FakeDrive : public QObject
{
public:
    struct File
    {
        QString id;
        QString name;
        QString mimeType;
        QString parent;
        QByteArray data;
        int revision = 0;
        QJsonObject appProperties;
    };

    QHash<QString, File> files;
    int tokenRequests = 0;
    int pageSize = 2;          // small, so paging is exercised
    int failNext = 0;          // answer this many API requests with 503
    int listings = 0;          // files.list requests
    int resumableUploads = 0;  // uploads that took a session (two requests)
    int multipartUploads = 0;  // uploads in one request
    QString lastAuthCode;
    QString lastVerifier;

    FakeDrive()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, this, [this, s] { onData(s); });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        files.insert(QStringLiteral("ROOT"),
                     File{QStringLiteral("ROOT"), QStringLiteral("Club photos"), kFolderMime, {}, {}, 0, {}});
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    DriveConfig config() const
    {
        DriveConfig c;
        c.clientId = QStringLiteral("client.apps.googleusercontent.com");
        c.clientSecret = QStringLiteral("not-secret");
        c.folderId = QStringLiteral("https://drive.google.com/drive/folders/ROOT?usp=sharing");
        c.refreshToken = QStringLiteral("good-refresh");
        c.apiBase = base();
        c.tokenUrl = base() + QStringLiteral("/token");
        c.authUrl = base() + QStringLiteral("/auth");
        return c;
    }

    QString pathOf(const QString &id) const
    {
        QStringList parts;
        for (QString cur = id; !cur.isEmpty() && cur != QLatin1String("ROOT"); cur = files.value(cur).parent)
            parts.prepend(files.value(cur).name);
        return parts.join(QLatin1Char('/'));
    }
    const File *byPath(const QString &path) const
    {
        for (const File &f : files) {
            if (f.mimeType != kFolderMime && pathOf(f.id) == path)
                return &f;
        }
        return nullptr;
    }

private:
    struct Session
    {
        QString existingId;
        QJsonObject meta;
    };
    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QString, Session> m_sessions;
    int m_nextId = 1;

    static QString md5(const QByteArray &d)
    {
        return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Md5).toHex());
    }
    QJsonObject resource(const File &f) const
    {
        QJsonObject o{{QStringLiteral("id"), f.id},
                      {QStringLiteral("name"), f.name},
                      {QStringLiteral("mimeType"), f.mimeType},
                      {QStringLiteral("parents"), QJsonArray{f.parent}}};
        if (f.mimeType != kFolderMime) {
            o.insert(QStringLiteral("size"), QString::number(f.data.size()));
            o.insert(QStringLiteral("md5Checksum"), md5(f.data));
            o.insert(QStringLiteral("headRevisionId"), QStringLiteral("rev%1").arg(f.revision));
            o.insert(QStringLiteral("lastModifyingUser"),
                     QJsonObject{{QStringLiteral("displayName"), QStringLiteral("Drive User")}});
            if (!f.appProperties.isEmpty())
                o.insert(QStringLiteral("appProperties"), f.appProperties);
        }
        return o;
    }

    void reply(QTcpSocket *s, int status, const QByteArray &body,
               const QByteArray &type = "application/json", const QByteArray &extra = {})
    {
        s->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: " + type
                 + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n" + extra
                 + "Connection: close\r\n\r\n" + body);
        s->disconnectFromHost();
    }
    void replyJson(QTcpSocket *s, int status, const QJsonObject &o, const QByteArray &extra = {})
    {
        reply(s, status, QJsonDocument(o).toJson(QJsonDocument::Compact), "application/json", extra);
    }

    void onData(QTcpSocket *s)
    {
        QByteArray &buf = m_buffers[s];
        buf += s->readAll();
        const int end = buf.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        const QList<QByteArray> lines = buf.left(end).split('\n');
        qint64 length = 0;
        QByteArray auth;
        QByteArray contentType;
        for (const QByteArray &line : lines.mid(1)) {
            const int colon = line.indexOf(':');
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (name == "content-length")
                length = value.toLongLong();
            else if (name == "authorization")
                auth = value;
            else if (name == "content-type")
                contentType = value;
        }
        if (buf.size() < end + 4 + length)
            return;
        const QByteArray body = buf.mid(end + 4, length);
        const QList<QByteArray> request = lines.first().trimmed().split(' ');
        m_buffers.remove(s);
        handle(s, request.value(0), QUrl::fromEncoded(request.value(1)), auth, contentType, body);
    }

    // The JSON metadata and the content of a multipart/related upload.
    static std::pair<QJsonObject, QByteArray> multipart(const QByteArray &contentType,
                                                        const QByteArray &body)
    {
        const QByteArray boundary = "--" + contentType.mid(contentType.indexOf("boundary=") + 9);
        QList<QByteArray> parts;
        for (qsizetype at = body.indexOf(boundary); at >= 0;) {
            const qsizetype next = body.indexOf(boundary, at + boundary.size());
            if (next < 0)
                break;
            QByteArray part = body.mid(at + boundary.size(), next - at - boundary.size());
            part = part.mid(part.indexOf("\r\n\r\n") + 4);
            part.chop(2);   // the CRLF before the next boundary
            parts << part;
            at = next;
        }
        return {QJsonDocument::fromJson(parts.value(0)).object(), parts.value(1)};
    }

    void store(QTcpSocket *s, const QString &existingId, const QJsonObject &meta, const QByteArray &data)
    {
        File f;
        if (!existingId.isEmpty()) {
            f = files.value(existingId);
        } else {
            f.id = QStringLiteral("F%1").arg(m_nextId++, 4, 10, QLatin1Char('0'));
            f.name = meta.value(QStringLiteral("name")).toString();
            f.mimeType = QStringLiteral("image/jpeg");
            f.parent = meta.value(QStringLiteral("parents")).toArray().first().toString();
        }
        f.data = data;
        ++f.revision;
        if (meta.contains(QStringLiteral("appProperties")))
            f.appProperties = meta.value(QStringLiteral("appProperties")).toObject();
        files.insert(f.id, f);
        replyJson(s, 200, resource(f));
    }

    void handle(QTcpSocket *s, const QByteArray &verb, const QUrl &url, const QByteArray &auth,
                const QByteArray &contentType, const QByteArray &body)
    {
        const QString path = url.path();
        const QUrlQuery query(url);
        auto q = [&](const char *name) {
            return query.queryItemValue(QLatin1String(name), QUrl::FullyDecoded);
        };

        if (path == QLatin1String("/token")) {
            ++tokenRequests;
            const QUrlQuery form(QString::fromUtf8(body));
            auto f = [&](const char *name) {
                return form.queryItemValue(QLatin1String(name), QUrl::FullyDecoded);
            };
            if (f("grant_type") == QLatin1String("authorization_code")) {
                lastAuthCode = f("code");
                lastVerifier = f("code_verifier");
                replyJson(s, 200, {{QStringLiteral("access_token"), QStringLiteral("tok")},
                                   {QStringLiteral("refresh_token"), QStringLiteral("good-refresh")},
                                   {QStringLiteral("expires_in"), 3600}});
            } else if (f("refresh_token") == QLatin1String("good-refresh")) {
                replyJson(s, 200, {{QStringLiteral("access_token"), QStringLiteral("tok")},
                                   {QStringLiteral("expires_in"), 3600}});
            } else {
                replyJson(s, 400, {{QStringLiteral("error"), QStringLiteral("invalid_grant")}});
            }
            return;
        }
        if (auth != "Bearer tok") {
            replyJson(s, 401, {{QStringLiteral("error"),
                                QJsonObject{{QStringLiteral("message"), QStringLiteral("bad token")}}}});
            return;
        }
        if (failNext > 0) {
            --failNext;
            replyJson(s, 503, {{QStringLiteral("error"),
                                QJsonObject{{QStringLiteral("message"), QStringLiteral("try later")}}}});
            return;
        }
        if (path == QLatin1String("/drive/v3/about")) {
            replyJson(s, 200, {{QStringLiteral("user"),
                                QJsonObject{{QStringLiteral("emailAddress"), QStringLiteral("bob@example.com")}}}});
            return;
        }

        static const QRegularExpression fileRe(QStringLiteral("^/drive/v3/files/([^/]+)$"));
        static const QRegularExpression uploadRe(QStringLiteral("^/upload/drive/v3/files(?:/([^/]+))?$"));
        static const QRegularExpression sessionRe(QStringLiteral("^/upload-session/(\\w+)$"));

        if (verb == "GET" && path == QLatin1String("/drive/v3/files")) {
            // "'<id>' in parents and trashed = false[ and name = '<n>' and mimeType = '<m>']"
            static const QRegularExpression term(QStringLiteral("and (\\w+) = '((?:[^'\\\\]|\\\\.)*)'"));
            QString name;
            QString mime;
            for (auto it = term.globalMatch(q("q")); it.hasNext();) {
                const auto m = it.next();
                QString value = m.captured(2);
                value.replace(QLatin1String("\\'"), QLatin1String("'"));
                value.replace(QLatin1String("\\\\"), QLatin1String("\\"));
                if (m.captured(1) == QLatin1String("name"))
                    name = value;
                else if (m.captured(1) == QLatin1String("mimeType"))
                    mime = value;
            }
            ++listings;
            // "('<a>' in parents or '<b>' in parents ...) and ..."
            static const QRegularExpression parentRe(QStringLiteral("'([^']+)' in parents"));
            QSet<QString> parents;
            for (auto it = parentRe.globalMatch(q("q")); it.hasNext();)
                parents.insert(it.next().captured(1));
            QList<File> hits;
            for (const File &f : std::as_const(files)) {
                if (parents.contains(f.parent) && (name.isEmpty() || f.name == name)
                    && (mime.isEmpty() || f.mimeType == mime))
                    hits << f;
            }
            std::sort(hits.begin(), hits.end(), [](const File &a, const File &b) { return a.id < b.id; });
            const int start = q("pageToken").toInt();
            QJsonArray out;
            for (int i = start; i < hits.size() && i < start + pageSize; ++i)
                out.append(resource(hits.at(i)));
            QJsonObject o{{QStringLiteral("files"), out}};
            if (start + pageSize < hits.size())
                o.insert(QStringLiteral("nextPageToken"), QString::number(start + pageSize));
            replyJson(s, 200, o);
            return;
        }
        if (const auto m = fileRe.match(path); m.hasMatch() && verb == "GET") {
            if (!files.contains(m.captured(1))) {
                replyJson(s, 404, {{QStringLiteral("error"),
                                    QJsonObject{{QStringLiteral("message"), QStringLiteral("File not found")}}}});
                return;
            }
            const File &f = files[m.captured(1)];
            if (q("alt") == QLatin1String("media"))
                reply(s, 200, f.data, "application/octet-stream");
            else
                replyJson(s, 200, resource(f));
            return;
        }
        if (verb == "POST" && path == QLatin1String("/drive/v3/files")) {
            const QJsonObject meta = QJsonDocument::fromJson(body).object();
            File f{QStringLiteral("F%1").arg(m_nextId++, 4, 10, QLatin1Char('0')),
                   meta.value(QStringLiteral("name")).toString(),
                   meta.value(QStringLiteral("mimeType")).toString(),
                   meta.value(QStringLiteral("parents")).toArray().first().toString(), {}, 0, {}};
            files.insert(f.id, f);
            replyJson(s, 200, resource(f));
            return;
        }
        if (const auto m = uploadRe.match(path);
            m.hasMatch() && (verb == "POST" || verb == "PATCH") && q("uploadType") == QLatin1String("multipart")) {
            ++multipartUploads;
            const auto [meta, data] = multipart(contentType, body);
            store(s, m.captured(1), meta, data);
            return;
        }
        if (const auto m = uploadRe.match(path); m.hasMatch() && (verb == "POST" || verb == "PATCH")) {
            ++resumableUploads;
            const QString sid = QString::number(m_nextId++);
            m_sessions.insert(sid, Session{m.captured(1), QJsonDocument::fromJson(body).object()});
            replyJson(s, 200, {}, "Location: " + base().toUtf8() + "/upload-session/" + sid.toUtf8() + "\r\n");
            return;
        }
        if (const auto m = sessionRe.match(path); m.hasMatch() && verb == "PUT") {
            const Session session = m_sessions.take(m.captured(1));
            store(s, session.existingId, session.meta, body);
            return;
        }
        replyJson(s, 404, {{QStringLiteral("error"),
                            QJsonObject{{QStringLiteral("message"), QStringLiteral("no route")}}}});
    }
};

QString md5Hex(const QByteArray &d)
{
    return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Md5).toHex());
}
QString shaHex(const QByteArray &d)
{
    return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex());
}

QString writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(data);
    return path;
}

} // namespace

class TestGoogleDrive : public QObject
{
    Q_OBJECT

private slots:
    void pkceMatchesRfc7636()
    {
        // RFC 7636, appendix B.
        QCOMPARE(GoogleAuth::pkceChallenge(QStringLiteral("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")),
                 QStringLiteral("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
    }

    void folderIdFromLinks()
    {
        QCOMPARE(DriveConfig::folderIdFrom(QStringLiteral("https://drive.google.com/drive/folders/1AbC-d_9?usp=sharing")),
                 QStringLiteral("1AbC-d_9"));
        QCOMPARE(DriveConfig::folderIdFrom(QStringLiteral("https://drive.google.com/open?id=XYZ123")),
                 QStringLiteral("XYZ123"));
        QCOMPARE(DriveConfig::folderIdFrom(QStringLiteral("  1AbC  ")), QStringLiteral("1AbC"));
    }

    // The browser part played by a QNetworkAccessManager.
    void signInThroughTheBrowser()
    {
        FakeDrive drive;
        GoogleAuth auth;
        QNetworkAccessManager browser;
        QUrl shown;
        auth.setOpenUrl([&](const QUrl &url) {
            shown = url;
            const QUrlQuery q(url);
            QUrl back(q.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
            back.setQuery(QStringLiteral("state=%1&code=4/0Ab%2Fcode").arg(q.queryItemValue(QStringLiteral("state"))));
            browser.get(QNetworkRequest(back));
            return true;
        });
        QSignalSpy done(&auth, &GoogleAuth::finished);
        QVERIFY(auth.start(drive.config()));
        QVERIFY(done.wait(5000));
        const auto result = done.first().first().value<GoogleAuth::Result>();
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.refreshToken, QStringLiteral("good-refresh"));
        QCOMPARE(result.account, QStringLiteral("bob@example.com"));

        const QUrlQuery q(shown);
        QCOMPARE(q.queryItemValue(QStringLiteral("code_challenge_method")), QStringLiteral("S256"));
        QCOMPARE(q.queryItemValue(QStringLiteral("access_type")), QStringLiteral("offline"));
        QVERIFY(q.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded).startsWith(QLatin1String("http://127.0.0.1:")));
        QCOMPARE(drive.lastAuthCode, QStringLiteral("4/0Ab/code"));
        QCOMPARE(GoogleAuth::pkceChallenge(drive.lastVerifier),
                 q.queryItemValue(QStringLiteral("code_challenge")));
    }

    void signInRejectsAForgedReply()
    {
        FakeDrive drive;
        GoogleAuth auth;
        QNetworkAccessManager browser;
        auth.setOpenUrl([&](const QUrl &url) {
            QUrl back(QUrlQuery(url).queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
            back.setQuery(QStringLiteral("state=wrong&code=abc"));
            browser.get(QNetworkRequest(back));
            return true;
        });
        QSignalSpy done(&auth, &GoogleAuth::finished);
        QVERIFY(auth.start(drive.config()));
        QVERIFY(done.wait(5000));
        QVERIFY(!done.first().first().value<GoogleAuth::Result>().ok);
        QCOMPARE(drive.tokenRequests, 0);
    }

    void storeRoundTrip()
    {
        FakeDrive drive;
        GoogleDriveStore store(drive.config());
        store.setRetryDelayMs(0);
        QVERIFY2(store.testConnection(), qPrintable(store.error()));
        QVERIFY(store.list()->isEmpty());

        QTemporaryDir dir;
        const QByteArray a = "orchid bytes";
        const QByteArray b = "robin bytes";
        const QString nameA = QStringLiteral("Orchids/Caladenia/it's (1).jpg");
        QVERIFY2(store.upload(writeFile(dir.filePath(QStringLiteral("a")), a), nameA, md5Hex(a),
                              shaHex(a), QStringLiteral("Alice")),
                 qPrintable(store.error()));
        for (int i = 0; i < 3; ++i) {   // more than a page in one folder
            const QByteArray d = "bird " + QByteArray::number(i);
            QVERIFY(store.upload(writeFile(dir.filePath(QStringLiteral("b")), d),
                                 QStringLiteral("Birds/b%1.jpg").arg(i), md5Hex(d), shaHex(d),
                                 QStringLiteral("Alice")));
        }
        QVERIFY(drive.byPath(QStringLiteral("library/") + nameA));

        GoogleDriveStore fresh(drive.config());
        const auto listed = fresh.list();
        QVERIFY2(listed, qPrintable(fresh.error()));
        QCOMPARE(listed->size(), 4);
        RemoteEntry entry;
        for (const RemoteEntry &e : *listed) {
            if (e.relPath == nameA)
                entry = e;
        }
        QCOMPARE(entry.md5, md5Hex(a));
        QCOMPARE(entry.sha256, shaHex(a));
        QCOMPARE(entry.uploadedBy, QStringLiteral("Alice"));
        QCOMPARE(entry.size, qint64(a.size()));

        // Replacing it keeps the one file, with a new version.
        const QString before = entry.version;
        QVERIFY(fresh.upload(writeFile(dir.filePath(QStringLiteral("a")), b), nameA, md5Hex(b),
                             shaHex(b), QStringLiteral("Bob")));
        QVERIFY(fresh.describe(entry));
        QVERIFY(entry.version != before);
        QCOMPARE(entry.uploadedBy, QStringLiteral("Bob"));
        QCOMPARE(fresh.list()->size(), 4);

        QVERIFY(fresh.download(entry, dir.filePath(QStringLiteral("down"))));
        QFile f(dir.filePath(QStringLiteral("down")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), b);

        QCOMPARE(drive.tokenRequests, 2);   // once per store
    }

    // A tree of folders is listed a level at a time, many folders a
    // request; small files go up in one request, big ones in two.
    void listsManyFoldersPerRequestAndUploadsSmallFilesInOne()
    {
        FakeDrive drive;
        drive.pageSize = 1000;
        QTemporaryDir dir;
        {
            GoogleDriveStore store(drive.config());
            store.setRetryDelayMs(0);
            QVERIFY(store.list());
            for (int i = 0; i < 12; ++i) {
                const QByteArray d = "photo " + QByteArray::number(i);
                QVERIFY2(store.upload(writeFile(dir.filePath(QStringLiteral("p")), d),
                                      QStringLiteral("Family %1/Genus/p%2.jpg").arg(i % 6).arg(i),
                                      md5Hex(d), shaHex(d), QStringLiteral("Alice")),
                         qPrintable(store.error()));
            }
            QCOMPARE(drive.multipartUploads, 12);
            QCOMPARE(drive.resumableUploads, 0);
            const QByteArray big(int(GoogleDriveStore::kSingleRequestUploadBytes) + 1, 'x');
            QVERIFY2(store.upload(writeFile(dir.filePath(QStringLiteral("big")), big),
                                  QStringLiteral("big.jpg"), md5Hex(big), shaHex(big),
                                  QStringLiteral("Alice")),
                     qPrintable(store.error()));
            QCOMPARE(drive.resumableUploads, 1);
        }

        GoogleDriveStore fresh(drive.config());
        fresh.setFoldersPerListing(4);
        drive.listings = 0;
        const auto listed = fresh.list();
        QVERIFY2(listed, qPrintable(fresh.error()));
        QCOMPARE(listed->size(), 13);
        // Finding library/ (1), listing it (1), its 6 families (2, at 4 a
        // request) and their 6 genera (2) -- not one request per folder.
        QCOMPARE(drive.listings, 6);
        QSet<QString> paths;
        for (const RemoteEntry &e : *listed)
            paths.insert(e.relPath);
        QVERIFY(paths.contains(QStringLiteral("Family 5/Genus/p11.jpg")));
        QVERIFY(paths.contains(QStringLiteral("big.jpg")));
        // Listed, so missing folders are made without asking first: one
        // request each, parents before children, counted as they go.
        drive.listings = 0;
        QList<int> progress;
        QVERIFY2(fresh.prepareUploads({QStringLiteral("New/Deep/x.jpg"), QStringLiteral("New/y.jpg"),
                                       QStringLiteral("Family 1/Genus/p1.jpg")},
                                      [&](int done, int) { progress << done; }),
                 qPrintable(fresh.error()));
        QCOMPARE(drive.listings, 0);
        QCOMPARE(progress, (QList<int>{0, 1}));
        int made = 0;
        for (const FakeDrive::File &f : std::as_const(drive.files)) {
            if (f.name == QLatin1String("New") || f.name == QLatin1String("Deep"))
                ++made;
        }
        QCOMPARE(made, 2);
        const QByteArray d = "deep";
        QVERIFY(fresh.upload(writeFile(dir.filePath(QStringLiteral("d")), d), QStringLiteral("New/Deep/x.jpg"),
                             md5Hex(d), shaHex(d), QStringLiteral("Alice")));
        QCOMPARE(drive.listings, 0);
        QVERIFY(drive.byPath(QStringLiteral("library/New/Deep/x.jpg")));
    }

    void thumbnails()
    {
        FakeDrive drive;
        GoogleDriveStore store(drive.config());
        QVERIFY(store.listThumbnails()->isEmpty());
        QVERIFY2(store.putThumbnail(QStringLiteral("abc_256"), "jpeg"), qPrintable(store.error()));
        QVERIFY(store.putThumbnail(QStringLiteral("abc_256"), "jpeg 2"));   // replaced, not doubled
        GoogleDriveStore other(drive.config());
        QCOMPARE(*other.listThumbnails(), QSet<QString>{QStringLiteral("abc_256")});
        QCOMPARE(other.getThumbnail(QStringLiteral("abc_256")).value_or(QByteArray()), QByteArray("jpeg 2"));
        QVERIFY(other.list()->isEmpty());   // thumbnails aren't photos
    }

    void retriesWhenDriveIsBusy()
    {
        FakeDrive drive;
        GoogleDriveStore store(drive.config());
        store.setRetryDelayMs(0);
        drive.failNext = 2;
        QVERIFY2(store.testConnection(), qPrintable(store.error()));
        drive.failNext = 10;
        QVERIFY(!store.testConnection());
        QVERIFY(store.error().contains(QLatin1String("try later")));
    }

    void expiredSignInSaysSo()
    {
        FakeDrive drive;
        DriveConfig config = drive.config();
        config.refreshToken = QStringLiteral("revoked");
        GoogleDriveStore store(config);
        QVERIFY(!store.testConnection());
        QVERIFY2(store.error().contains(QLatin1String("sign in again")), qPrintable(store.error()));
    }

    void twoComputersSyncThroughDrive()
    {
        FakeDrive drive;
        QTemporaryDir alice;
        QTemporaryDir bob;
        writeFile(alice.filePath(QStringLiteral("Birds/robin.jpg")), "robin");
        writeFile(alice.filePath(QStringLiteral("Birds/robin.nef")), "robin raw");
        // Enough for every worker, in folders that don't exist yet.
        for (int i = 0; i < 20; ++i)
            writeFile(alice.filePath(QStringLiteral("Frogs/Family %1/f%2.jpg").arg(i % 3).arg(i)),
                      "frog " + QByteArray::number(i));
        {
            GoogleDriveStore store(drive.config());
            LibraryState state(QStringLiteral(":memory:"));
            QVERIFY(state.open());
            LibrarySync sync(store, state, alice.path());
            const auto r = sync.run({true, true, QStringLiteral("Alice"), {}});
            QVERIFY2(r, qPrintable(sync.error()));
            QVERIFY2(r->failures.isEmpty(), qPrintable(r->failures.join(QLatin1Char('\n'))));
            QCOMPARE(r->uploaded, 22);
        }
        // Each folder made once, though four workers uploaded into them.
        int frogFamilies = 0;
        for (const FakeDrive::File &f : std::as_const(drive.files)) {
            if (f.mimeType == kFolderMime && f.name.startsWith(QLatin1String("Family ")))
                ++frogFamilies;
        }
        QCOMPARE(frogFamilies, 3);
        // Someone adds one through the Drive website.
        drive.files.insert(QStringLiteral("W1"),
                           FakeDrive::File{QStringLiteral("W1"), QStringLiteral("wren.jpg"),
                                           QStringLiteral("image/jpeg"),
                                           drive.files.value(drive.byPath(QStringLiteral("library/Birds/robin.jpg"))->id).parent,
                                           "wren", 1, {}});
        {
            GoogleDriveStore store(drive.config());
            LibraryState state(QStringLiteral(":memory:"));
            QVERIFY(state.open());
            LibrarySync sync(store, state, bob.path());
            const auto r = sync.run({true, true, QStringLiteral("Bob"), {}});
            QVERIFY2(r, qPrintable(sync.error()));
            QVERIFY2(r->failures.isEmpty(), qPrintable(r->failures.join(QLatin1Char('\n'))));
            QCOMPARE(r->downloaded, 23);
            QFile f(bob.filePath(QStringLiteral("Birds/wren.jpg")));
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), QByteArray("wren"));
            const auto again = sync.run({true, true, QStringLiteral("Bob"), {}});
            QVERIFY(again && again->plan.nothingToDo());
        }
    }
};

QTEST_GUILESS_MAIN(TestGoogleDrive)
#include "tst_googledrive.moc"
