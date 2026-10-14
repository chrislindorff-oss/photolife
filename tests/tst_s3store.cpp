#include <QtTest>

#include <QCryptographicHash>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUuid>

#include "libsync/LibraryState.h"
#include "libsync/LibrarySync.h"
#include "libsync/S3Store.h"

using namespace pl::libsync;

namespace {

// S3 cases are opt-in: they need a real S3-compatible server, so they're
// skipped unless PHOTOLIFE_TEST_S3 is set to
//   "<endpoint> <bucket> <access key> <secret>"
// e.g. a local one:
//   rclone serve s3 --auth-key test,testsecret --addr 127.0.0.1:9000 /tmp/s3root
//   (with a /tmp/s3root/photos folder as the bucket)
//   PHOTOLIFE_TEST_S3="http://127.0.0.1:9000 photos test testsecret"
std::optional<S3Config> s3ConfigFromEnv()
{
    const QStringList parts = QProcessEnvironment::systemEnvironment()
                                  .value(QStringLiteral("PHOTOLIFE_TEST_S3"))
                                  .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 4)
        return std::nullopt;
    S3Config config;
    config.endpoint = parts.at(0);
    config.bucket = parts.at(1);
    config.accessKey = parts.at(2);
    config.secretKey = parts.at(3);
    config.region = QStringLiteral("us-east-1");
    // A fresh folder in the bucket for every run.
    config.prefix = QStringLiteral("tst-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    return config;
}

QString hex(const QByteArray &data, QCryptographicHash::Algorithm algorithm)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, algorithm).toHex());
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

} // namespace

class TestS3Store : public QObject
{
    Q_OBJECT

private:
    std::optional<S3Config> m_config;

private slots:
    void initTestCase()
    {
        m_config = s3ConfigFromEnv();
        if (!m_config)
            QSKIP("PHOTOLIFE_TEST_S3 not set");
    }

    void badSecretIsRefused()
    {
        S3Config wrong = *m_config;
        wrong.secretKey += QLatin1Char('x');
        S3Store store(wrong);
        QVERIFY(!store.testConnection());
        QVERIFY(!store.error().isEmpty());
    }

    void roundTrip()
    {
        S3Store store(*m_config);
        QVERIFY2(store.testConnection(), qPrintable(store.error()));

        QTemporaryDir dir;
        const QString rel = QStringLiteral("Orchids/Caladenia (sp) é+1 & co.jpg");
        const QByteArray data = QByteArray("not really a jpeg ").repeated(5000);
        writeFile(dir.filePath(QStringLiteral("up.jpg")), data);

        const QString md5 = hex(data, QCryptographicHash::Md5);
        const QString sha = hex(data, QCryptographicHash::Sha256);
        const auto uploaded = store.upload(dir.filePath(QStringLiteral("up.jpg")), rel, md5, sha,
                                           QStringLiteral("Zoë"));
        QVERIFY2(uploaded, qPrintable(store.error()));

        const auto listed = store.list();
        QVERIFY2(listed, qPrintable(store.error()));
        QCOMPARE(listed->size(), 1);
        RemoteEntry entry = listed->first();
        QCOMPARE(entry.relPath, rel);
        QCOMPARE(entry.size, qint64(data.size()));
        QCOMPARE(entry.md5, md5);

        QVERIFY2(store.describe(entry), qPrintable(store.error()));
        QCOMPARE(entry.sha256, sha);
        QCOMPARE(entry.uploadedBy, QStringLiteral("Zoë"));

        const QString down = dir.filePath(QStringLiteral("down.jpg"));
        QVERIFY2(store.download(entry, down), qPrintable(store.error()));
        QFile f(down);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), data);

        // Thumbnails live beside the photos, not among them.
        QVERIFY2(store.putThumbnail(sha + QStringLiteral("_256"), "jpeg bytes"), qPrintable(store.error()));
        const auto thumbs = store.listThumbnails();
        QVERIFY2(thumbs, qPrintable(store.error()));
        QCOMPARE(*thumbs, QSet<QString>{sha + QStringLiteral("_256")});
        QCOMPARE(store.getThumbnail(sha + QStringLiteral("_256")).value_or(QByteArray()), QByteArray("jpeg bytes"));
        QCOMPARE(store.list()->size(), 1);
    }

    // Two computers sharing one bucket through LibrarySync.
    void twoMachines()
    {
        S3Config config = *m_config;
        config.prefix += QStringLiteral("-sync");
        QTemporaryDir alice;
        QTemporaryDir bob;
        writeFile(alice.filePath(QStringLiteral("Birds/robin.jpg")), "robin");
        writeFile(alice.filePath(QStringLiteral("Birds/robin.nef")), "robin raw");

        {
            S3Store store(config);
            LibraryState state(QStringLiteral(":memory:"));
            QVERIFY(state.open());
            LibrarySync sync(store, state, alice.path());
            const auto r = sync.run({true, true, QStringLiteral("Alice"), {}});
            QVERIFY2(r, qPrintable(sync.error()));
            QVERIFY2(r->failures.isEmpty(), qPrintable(r->failures.join(QLatin1Char('\n'))));
            QCOMPARE(r->uploaded, 2);
        }
        {
            S3Store store(config);
            LibraryState state(QStringLiteral(":memory:"));
            QVERIFY(state.open());
            LibrarySync sync(store, state, bob.path());
            const auto r = sync.run({true, true, QStringLiteral("Bob"), {}});
            QVERIFY2(r, qPrintable(sync.error()));
            QVERIFY2(r->failures.isEmpty(), qPrintable(r->failures.join(QLatin1Char('\n'))));
            QCOMPARE(r->downloaded, 2);
            QFile f(bob.filePath(QStringLiteral("Birds/robin.nef")));
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), QByteArray("robin raw"));

            // A second run has nothing to do.
            const auto again = sync.run({true, true, QStringLiteral("Bob"), {}});
            QVERIFY(again && again->plan.nothingToDo());
        }
    }
};

QTEST_GUILESS_MAIN(TestS3Store)
#include "tst_s3store.moc"
