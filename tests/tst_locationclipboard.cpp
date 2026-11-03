#include <QtTest>

#include "geo/LatLon.h"

using namespace pl;

class TestLocationClipboard : public QObject
{
    Q_OBJECT

private slots:
    void readsALocation_data();
    void readsALocation();
    void refusesWhatIsntOne();
    void writesWhatItReads();
};

void TestLocationClipboard::readsALocation_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("comma") << QStringLiteral("-37.8312, 144.9821");
    QTest::newRow("space") << QStringLiteral("-37.8312 144.9821");
    QTest::newRow("semicolon") << QStringLiteral("-37.8312;144.9821");
    QTest::newRow("google maps") << QStringLiteral("-37.83120, 144.98210");
    QTest::newRow("padded") << QStringLiteral("  -37.8312 ,  144.9821\n");
}

void TestLocationClipboard::readsALocation()
{
    QFETCH(QString, text);
    const std::optional<QPointF> at = geo::parseLatLon(text);
    QVERIFY(at);
    QCOMPARE(at->y(), -37.8312);
    QCOMPARE(at->x(), 144.9821);
}

void TestLocationClipboard::refusesWhatIsntOne()
{
    for (const QString &text : {QStringLiteral("abc"), QStringLiteral("95, 10"), QStringLiteral("10, 200"),
                                QStringLiteral("-37.8"), QStringLiteral("-37.8, 144.9, 3"), QString()})
        QVERIFY2(!geo::parseLatLon(text), qPrintable(text));
}

void TestLocationClipboard::writesWhatItReads()
{
    const QString text = geo::formatLatLon(-37.8312, 144.9821);
    QCOMPARE(text, QStringLiteral("-37.831200, 144.982100"));
    QCOMPARE(geo::parseLatLon(text)->y(), -37.8312);
}

QTEST_GUILESS_MAIN(TestLocationClipboard)
#include "tst_locationclipboard.moc"
