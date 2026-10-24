#include <QtTest>

#include <QPushButton>
#include <QSignalSpy>

#include "ui/GettingStartedPanel.h"

using pl::GettingStartedPanel;
using pl::GuideState;

class TestGettingStartedPanel : public QObject
{
    Q_OBJECT

private slots:
    void newUserStartsAtAddingPhotos();
    void stepsTickOffInOrder();
    void buttonsWaitForWhatTheyNeed();
    void buttonsAndHelpSayWhichStep();
    void openingAtStartup();
    void picturesAreBundled();

private:
    static QPushButton *button(GettingStartedPanel &p, const QString &text)
    {
        for (auto *b : p.findChildren<QPushButton *>())
            if (b->text() == text)
                return b;
        return nullptr;
    }
};

void TestGettingStartedPanel::newUserStartsAtAddingPhotos()
{
    GettingStartedPanel p;
    p.setState({});
    QCOMPARE(p.nextStep(), int(GettingStartedPanel::Library));
    QVERIFY(p.isExpanded(GettingStartedPanel::Library));
    QVERIFY(!p.isExpanded(GettingStartedPanel::Tree));
    QCOMPARE(p.basicsDone(), 0);
}

void TestGettingStartedPanel::stepsTickOffInOrder()
{
    GettingStartedPanel p;
    GuideState s;
    s.folders = 1;
    s.photos = 1200;
    p.setState(s);
    QVERIFY(p.isDone(GettingStartedPanel::Library));
    QCOMPARE(p.nextStep(), int(GettingStartedPanel::Tree));
    QVERIFY(!p.isExpanded(GettingStartedPanel::Library));   // done: folded away
    QVERIFY(p.isExpanded(GettingStartedPanel::Tree));

    s.trees = 2;
    s.treeNames = {"Orchids of Victoria", "Birds"};
    s.matched = 1100;
    s.toReview = 56;
    p.setState(s);
    QCOMPARE(p.nextStep(), int(GettingStartedPanel::Review));   // matched, but 56 waiting
    QCOMPARE(p.basicsDone(), 3);

    s.toReview = 0;
    s.exploredCoverage = true;
    p.setState(s);
    QCOMPARE(p.basicsDone(), GettingStartedPanel::kBasics);
    QCOMPARE(p.nextStep(), int(GettingStartedPanel::Inat));    // then the extras
    QVERIFY(!p.isExpanded(GettingStartedPanel::Inat));         // offered, not pushed

    s.inatPhotos = 12;
    s.organised = true;
    p.setState(s);
    QCOMPARE(p.nextStep(), -1);
}

void TestGettingStartedPanel::buttonsWaitForWhatTheyNeed()
{
    GettingStartedPanel p;
    p.setState({});
    QVERIFY(button(p, "Add a Folder…")->isEnabled());
    QVERIFY(button(p, "New Reference Tree…")->isEnabled());   // a tree can come first
    QVERIFY(!button(p, "Match Library")->isEnabled());
    QVERIFY(!button(p, "Open Review Unmatched")->isEnabled());

    GuideState s;
    s.folders = 1;
    s.photos = 10;
    s.trees = 1;
    p.setState(s);
    QVERIFY(button(p, "Match Library")->isEnabled());
}

void TestGettingStartedPanel::buttonsAndHelpSayWhichStep()
{
    GettingStartedPanel p;
    GuideState s;
    s.folders = 1;
    s.photos = 10;
    p.setState(s);
    QSignalSpy action(&p, &GettingStartedPanel::actionRequested);
    button(p, "New Reference Tree…")->click();
    button(p, "Import a Tree File…")->click();
    button(p, "Open a Download Folder…")->click();
    QCOMPARE(action.size(), 3);
    QCOMPARE(action.at(0), (QVariantList{int(GettingStartedPanel::Tree), 0}));
    QCOMPARE(action.at(1), (QVariantList{int(GettingStartedPanel::Tree), 1}));
    QCOMPARE(action.at(2), (QVariantList{int(GettingStartedPanel::Organise), 0}));
    QCOMPARE(GettingStartedPanel::helpAnchor(GettingStartedPanel::Explore), QStringLiteral("browse"));
}

void TestGettingStartedPanel::openingAtStartup()
{
    GuideState s;
    QVERIFY(GettingStartedPanel::basicsMissing(s));
    s.folders = 2;
    s.photos = 500;
    QVERIFY(GettingStartedPanel::basicsMissing(s));   // no tree yet
    s.trees = 4;
    QVERIFY(!GettingStartedPanel::basicsMissing(s));
}

void TestGettingStartedPanel::picturesAreBundled()
{
    for (const char *name : {"flow", "library", "tree", "match", "review", "explore", "inat", "organise"}) {
        const QImage image(QStringLiteral(":/guide/%1.png").arg(QLatin1String(name)));
        QVERIFY2(!image.isNull(), name);
        QVERIFY(image.width() >= 1000);   // drawn at twice their size
    }
}

QTEST_MAIN(TestGettingStartedPanel)
#include "tst_gettingstartedpanel.moc"
