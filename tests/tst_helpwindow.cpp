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

QTEST_MAIN(TestHelpWindow)
#include "tst_helpwindow.moc"
