#include "inat/PhotoIdentifier.h"
#include "ui/ImageViewer.h"

#include <QBuffer>
#include <QImage>
#include <QScrollArea>
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

    // Shift+drag round part of the photo selects it, as a part of the
    // picture (0..1); Esc clears it first and only then closes.
    void shiftDragSelectsAPart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("bird.jpg"));
        QImage image(400, 300, QImage::Format_RGB32);
        image.fill(Qt::darkGreen);
        QVERIFY(image.save(path));
        ImageViewer::Item item;
        item.path = path;
        item.canIdentify = true;

        ImageViewer viewer;
        viewer.resize(900, 700);
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        viewer.setItems({item}, 0);
        QWidget *viewport = viewer.findChild<QScrollArea *>()->viewport();
        // Shown 1:1 (it's smaller than the window), in the middle.
        const QPoint corner((viewport->width() - 400) / 2, (viewport->height() - 300) / 2);

        QTest::mousePress(viewport, Qt::LeftButton, Qt::ShiftModifier, corner + QPoint(100, 75));
        QTest::mouseMove(viewport, corner + QPoint(300, 225));
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::ShiftModifier, corner + QPoint(300, 225));
        QVERIFY(viewer.selection());
        QVERIFY(qAbs(viewer.selection()->x() - 0.25) < 0.01);
        QVERIFY(qAbs(viewer.selection()->y() - 0.25) < 0.01);
        QVERIFY(qAbs(viewer.selection()->width() - 0.5) < 0.01);
        QVERIFY(qAbs(viewer.selection()->height() - 0.5) < 0.01);

        QTest::keyClick(&viewer, Qt::Key_Escape);
        QVERIFY(!viewer.selection());
        QVERIFY(viewer.isVisible());
    }

    // What's sent for a part: just that part, from the full picture.
    void aSelectedPartIsWhatIsSent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("two.png"));
        QImage image(400, 200, QImage::Format_RGB32);
        image.fill(Qt::red);
        for (int y = 0; y < 200; ++y)
            for (int x = 200; x < 400; ++x)
                image.setPixelColor(x, y, Qt::blue);
        QVERIFY(image.save(path));

        inat::IdentifyRequest whole;
        whole.path = path;
        QImage sent = QImage::fromData(inat::PhotoIdentifier::renderJpeg(whole), "JPG");
        QCOMPARE(sent.size(), QSize(400, 200));

        inat::IdentifyRequest right = whole;
        right.crop = QRectF(0.5, 0, 0.5, 1);
        sent = QImage::fromData(inat::PhotoIdentifier::renderJpeg(right), "JPG");
        QCOMPARE(sent.size(), QSize(200, 200));
        const QColor middle = sent.pixelColor(100, 100);
        QVERIFY(middle.blue() > 200 && middle.red() < 60);
    }
};

QTEST_MAIN(TestImageViewer)
#include "tst_imageviewer.moc"
