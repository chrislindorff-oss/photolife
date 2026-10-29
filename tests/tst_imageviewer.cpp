#include "ui/ImageViewer.h"

#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace pl;

class TestImageViewer : public QObject
{
    Q_OBJECT

private:
    static QVector<ImageViewer::Item> items(const QTemporaryDir &dir, bool canReject)
    {
        QVector<ImageViewer::Item> out;
        for (const QString name : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}) {
            const QString path = dir.filePath(name + QStringLiteral(".jpg"));
            QImage image(40, 30, QImage::Format_RGB32);
            image.fill(Qt::darkGreen);
            image.save(path);
            ImageViewer::Item item;
            item.path = path;
            item.title = name;
            item.caption = name;
            item.canReject = canReject;
            out << item;
        }
        return out;
    }

private slots:
    // Organise's viewer: R rejects the photo on view and moves on; on a
    // rejected one it takes the reject back and stays.
    void rRejectsAndMovesOn()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVector<ImageViewer::Item> shown = items(dir, true);
        ImageViewer viewer;
        viewer.setItems(shown, 0);
        // The owner records it and hands the item back, as Organise does.
        QSignalSpy spy(&viewer, &ImageViewer::rejectToggleRequested);
        connect(&viewer, &ImageViewer::rejectToggleRequested, &viewer, [&](int i, bool reject) {
            shown[i].rejected = reject;
            viewer.updateItem(i, shown[i]);
        });

        QTest::keyClick(&viewer, Qt::Key_R);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 0);
        QCOMPARE(spy.at(0).at(1).toBool(), true);
        QVERIFY(shown[0].rejected);
        QCOMPARE(viewer.windowTitle(), QStringLiteral("b"));   // on to the next

        QTest::keyClick(&viewer, Qt::Key_Left);
        QCOMPARE(viewer.windowTitle(), QStringLiteral("a"));
        QTest::keyClick(&viewer, Qt::Key_R);
        QCOMPARE(spy.size(), 2);
        QCOMPARE(spy.at(1).at(1).toBool(), false);
        QVERIFY(!shown[0].rejected);
        QCOMPARE(viewer.windowTitle(), QStringLiteral("a"));   // kept, and stays
    }

    // Elsewhere (the library, Review) R does nothing.
    void rIgnoredWhereRejectingIsntOffered()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ImageViewer viewer;
        viewer.setItems(items(dir, false), 0);
        QSignalSpy spy(&viewer, &ImageViewer::rejectToggleRequested);
        QTest::keyClick(&viewer, Qt::Key_R);
        QCOMPARE(spy.size(), 0);
        QCOMPARE(viewer.windowTitle(), QStringLiteral("a"));
    }
};

QTEST_MAIN(TestImageViewer)
#include "tst_imageviewer.moc"
