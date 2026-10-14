#include <QtTest>

#include "libsync/S3Store.h"
#include "libsync/SigV4.h"

using namespace pl::libsync;

namespace {

const QByteArray kEmptyHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

QDateTime utc(const QString &iso)
{
    return QDateTime::fromString(iso, Qt::ISODate).toUTC();
}

} // namespace

// Checked against the examples AWS publishes for Signature Version 4.
class TestSigV4 : public QObject
{
    Q_OBJECT

private slots:
    void encoding()
    {
        QCOMPARE(sigV4Encode(QStringLiteral("a b+c/d~e")), QByteArray("a%20b%2Bc%2Fd~e"));
        QCOMPARE(sigV4Encode(QStringLiteral("dir/x(1).jpg"), true), QByteArray("dir/x%281%29.jpg"));
        QCOMPARE(sigV4Encode(QStringLiteral("é")), QByteArray("%C3%A9"));
    }

    void canonicalQuerySorts()
    {
        QCOMPARE(sigV4CanonicalQuery({{QStringLiteral("prefix"), QStringLiteral("J")},
                                      {QStringLiteral("max-keys"), QStringLiteral("2")}}),
                 QByteArray("max-keys=2&prefix=J"));
    }

    // AWS SigV4 test suite: get-vanilla.
    void getVanilla()
    {
        const SigV4Credentials creds{QStringLiteral("AKIDEXAMPLE"),
                                     QStringLiteral("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"),
                                     QStringLiteral("us-east-1"), QStringLiteral("service")};
        const QDateTime when = utc(QStringLiteral("2015-08-30T12:36:00Z"));
        const SigV4Headers headers{{"Host", "example.amazonaws.com"}, {"X-Amz-Date", sigV4AmzDate(when)}};
        QCOMPARE(sigV4Authorization(creds, "GET", "/", {}, headers, kEmptyHash, when),
                 QByteArray("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/"
                            "aws4_request, SignedHeaders=host;x-amz-date, Signature="
                            "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31"));
    }

    // The S3 documentation's examples (examplebucket, 24 May 2013).
    void s3Examples_data()
    {
        QTest::addColumn<QByteArray>("method");
        QTest::addColumn<QByteArray>("path");
        QTest::addColumn<SigV4Query>("query");
        QTest::addColumn<SigV4Headers>("extra");
        QTest::addColumn<QByteArray>("payloadHash");
        QTest::addColumn<QByteArray>("signature");

        QTest::newRow("GET object") << QByteArray("GET") << QByteArray("/test.txt") << SigV4Query()
                                    << SigV4Headers{{"Range", "bytes=0-9"}} << kEmptyHash
                                    << QByteArray("f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");

        const QByteArray putHash = "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072";
        QTest::newRow("PUT object") << QByteArray("PUT")
                                    << sigV4Encode(QStringLiteral("/test$file.text"), true)
                                    << SigV4Query()
                                    << SigV4Headers{{"Date", "Fri, 24 May 2013 00:00:00 GMT"},
                                                    {"x-amz-storage-class", "REDUCED_REDUNDANCY"}}
                                    << putHash
                                    << QByteArray("98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");

        QTest::newRow("list objects")
            << QByteArray("GET") << QByteArray("/")
            << SigV4Query{{QStringLiteral("max-keys"), QStringLiteral("2")},
                          {QStringLiteral("prefix"), QStringLiteral("J")}}
            << SigV4Headers() << kEmptyHash
            << QByteArray("34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7");
    }

    void s3Examples()
    {
        QFETCH(QByteArray, method);
        QFETCH(QByteArray, path);
        QFETCH(SigV4Query, query);
        QFETCH(SigV4Headers, extra);
        QFETCH(QByteArray, payloadHash);
        QFETCH(QByteArray, signature);

        const SigV4Credentials creds{QStringLiteral("AKIAIOSFODNN7EXAMPLE"),
                                     QStringLiteral("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"),
                                     QStringLiteral("us-east-1")};
        const QDateTime when = utc(QStringLiteral("2013-05-24T00:00:00Z"));
        SigV4Headers headers{{"Host", "examplebucket.s3.amazonaws.com"},
                             {"x-amz-date", sigV4AmzDate(when)},
                             {"x-amz-content-sha256", payloadHash}};
        headers += extra;
        const QByteArray auth = sigV4Authorization(creds, method, path, query, headers, payloadHash, when);
        QVERIFY2(auth.endsWith("Signature=" + signature), auth.constData());
    }

    // QUrl must send the path exactly as it was signed.
    void urlKeepsSignedEncoding()
    {
        S3Config config;
        config.endpoint = QStringLiteral("http://localhost:9000");
        config.bucket = QStringLiteral("photos");
        config.prefix = QStringLiteral("/team/");
        S3Store store(config);

        const QString key = store.objectKey(QStringLiteral("Orchids/Caladenia (sp) é+1 [a]&b=c.jpg"));
        QCOMPARE(key, QStringLiteral("team/library/Orchids/Caladenia (sp) é+1 [a]&b=c.jpg"));

        const SigV4Query query{{QStringLiteral("prefix"), QStringLiteral("team/library/")},
                               {QStringLiteral("continuation-token"), QStringLiteral("a+b/c=")}};
        const auto t = store.target(key, query);
        QCOMPARE(t.host, QByteArray("localhost:9000"));
        QCOMPARE(t.url.toEncoded(QUrl::FullyEncoded),
                 "http://localhost:9000" + t.encodedPath + '?' + sigV4CanonicalQuery(query));
    }
};

QTEST_APPLESS_MAIN(TestSigV4)
#include "tst_sigv4.moc"
