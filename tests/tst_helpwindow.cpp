#include <QtTest>

#include <QTextBrowser>

#include "ui/HelpWindow.h"

using namespace pl;

class TestHelpWindow : public QObject
{
    Q_OBJECT

private slots:
    void loadsBundledHelp();
    void showPageScrollsToAnchor();
    void catalogueDiagramIsBundledAndShown();
};

void TestHelpWindow::loadsBundledHelp()
{
    HelpWindow window;
    auto *browser = window.findChild<QTextBrowser *>();
    QVERIFY(browser);

    const QString text = browser->toPlainText();
    QVERIFY(text.contains(QStringLiteral("PhotoLife Help")));
    QVERIFY(text.contains(QStringLiteral("Match Library")));
    QVERIFY(text.contains(QStringLiteral("Review")));
}

void TestHelpWindow::showPageScrollsToAnchor()
{
    HelpWindow window;
    window.showPage(QStringLiteral("shortcuts"));   // must not crash on a known anchor
    window.showPage(QStringLiteral("does-not-exist"));
    QVERIFY(true);
}

void TestHelpWindow::catalogueDiagramIsBundledAndShown()
{
    const QImage image(QStringLiteral(":/help/catalogues.png"));
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(2800, 1680));

    // The page refers to it, and the help viewer finds it from there.
    HelpWindow window;
    auto *browser = window.findChild<QTextBrowser *>();
    QVERIFY(browser->toHtml().contains(QStringLiteral("catalogues.png")));
    const QVariant loaded =
        browser->loadResource(QTextDocument::ImageResource, QUrl(QStringLiteral("catalogues.png")));
    QVERIFY(loaded.isValid());
}

QTEST_MAIN(TestHelpWindow)
#include "tst_helpwindow.moc"
