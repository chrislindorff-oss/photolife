#include <QtTest>

#include <QSignalSpy>

#include "FakeTransport.h"
#include "net/HttpClient.h"
#include "net/UpdateChecker.h"

using namespace pl;
using namespace pl::net;

class TestUpdateChecker : public QObject
{
    Q_OBJECT

private slots:
    void isNewerComparesNumerically();
    void isNewerHandlesBareHashAndDevSuffix();
    void reportsUpdateAvailable();
    void reportsUpToDate();
    void reportsFailureWhenNoReleases();

private:
    std::unique_ptr<HttpClient> m_http;
    std::unique_ptr<UpdateChecker> m_checker;
    void wire(std::function<Transport::Reply(const Transport::Request &, int)> r);
};

void TestUpdateChecker::wire(std::function<Transport::Reply(const Transport::Request &, int)> r)
{
    auto owned = std::make_unique<FakeTransport>();
    owned->responder = std::move(r);
    m_http = std::make_unique<HttpClient>(std::move(owned), nullptr);
    m_http->setMinRequestIntervalMs(0);
    m_checker = std::make_unique<UpdateChecker>(*m_http);
    m_checker->setRepo(QStringLiteral("chrisl/photolife"));
}

void TestUpdateChecker::isNewerComparesNumerically()
{
    QVERIFY(UpdateChecker::isNewer(QStringLiteral("v0.2.0"), QStringLiteral("0.1.0")));
    QVERIFY(UpdateChecker::isNewer(QStringLiteral("1.0.0"), QStringLiteral("0.9.9")));
    QVERIFY(!UpdateChecker::isNewer(QStringLiteral("0.1.0"), QStringLiteral("0.1.0")));
    QVERIFY(!UpdateChecker::isNewer(QStringLiteral("0.1.0"), QStringLiteral("0.2.0")));
    QVERIFY(UpdateChecker::isNewer(QStringLiteral("0.10.0"), QStringLiteral("0.9.0")));
}

void TestUpdateChecker::isNewerHandlesBareHashAndDevSuffix()
{
    QVERIFY(UpdateChecker::isNewer(QStringLiteral("v0.1.0"), QStringLiteral("8dd5183-dirty")));
    // A published 0.1.0 beats a local dev build of it.
    QVERIFY(UpdateChecker::isNewer(QStringLiteral("v0.1.0"), QStringLiteral("0.1.0-5-g8dd5183")));
    QVERIFY(!UpdateChecker::isNewer(QStringLiteral("garbage"), QStringLiteral("0.1.0")));
}

void TestUpdateChecker::reportsUpdateAvailable()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(
            R"({"tag_name":"v0.5.0","html_url":"https://github.com/chrisl/photolife/releases/tag/v0.5.0"})");
    });
    m_checker->setCurrentVersion(QStringLiteral("0.1.0"));

    QSignalSpy avail(m_checker.get(), &UpdateChecker::updateAvailable);
    m_checker->check();
    QVERIFY(avail.wait(3000));
    QCOMPARE(avail.at(0).at(0).toString(), QStringLiteral("v0.5.0"));
    QVERIFY(avail.at(0).at(1).toString().contains(QStringLiteral("releases/tag")));
}

void TestUpdateChecker::reportsUpToDate()
{
    wire([](const Transport::Request &, int) {
        return FakeTransport::ok(R"({"tag_name":"v0.1.0","html_url":"x"})");
    });
    m_checker->setCurrentVersion(QStringLiteral("0.1.0"));

    QSignalSpy ok(m_checker.get(), &UpdateChecker::upToDate);
    m_checker->check();
    QVERIFY(ok.wait(3000));
}

void TestUpdateChecker::reportsFailureWhenNoReleases()
{
    wire([](const Transport::Request &, int) { return FakeTransport::httpStatus(404); });
    m_checker->setCurrentVersion(QStringLiteral("0.1.0"));

    QSignalSpy fail(m_checker.get(), &UpdateChecker::checkFailed);
    m_checker->check();
    QVERIFY(fail.wait(3000));
}

QTEST_GUILESS_MAIN(TestUpdateChecker)
#include "tst_updatechecker.moc"
