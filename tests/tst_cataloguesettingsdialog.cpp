#include <QtTest>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>

#include "ui/CatalogueSettingsDialog.h"

using namespace pl;

class TestCatalogueSettingsDialog : public QObject
{
    Q_OBJECT

private slots:
    void driveProfileRoundTrips()
    {
        Settings::CatalogueProfile p;
        p.id = QStringLiteral("club");
        p.server = true;
        p.name = QStringLiteral("Club");
        p.pg.pgHost = QStringLiteral("h");
        p.pg.pgDbName = QStringLiteral("db");
        p.pg.pgUser = QStringLiteral("u");
        p.userName = QStringLiteral("Bob");
        p.libraryRoot = QStringLiteral("/photos");
        p.photoStore = QString::fromLatin1(Settings::kPhotoStoreDrive);
        p.drive.clientId = QStringLiteral("cid");
        p.drive.clientSecret = QStringLiteral("sec");
        p.drive.folderId = QStringLiteral("FOLDER");
        p.drive.refreshToken = QStringLiteral("rt");
        p.drive.account = QStringLiteral("bob@example.com");

        CatalogueSettingsDialog dialog(p);
        auto *ok = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        QVERIFY(ok->isEnabled());
        const auto back = dialog.profile();
        QCOMPARE(back.photoStore, p.photoStore);
        QCOMPARE(back.drive.refreshToken, QStringLiteral("rt"));
        QCOMPARE(back.drive.folderId, QStringLiteral("FOLDER"));

        // Pasting the folder's link keeps just its ID.
        dialog.findChild<QLineEdit *>(QStringLiteral("driveFolder"))
            ->setText(QStringLiteral("https://drive.google.com/drive/folders/NEWID?usp=sharing"));
        QCOMPARE(dialog.profile().drive.folderId, QStringLiteral("NEWID"));

        // Another OAuth client needs another sign-in.
        auto *client = dialog.findChild<QLineEdit *>(QStringLiteral("driveClientId"));
        client->setFocus();
        QTest::keyClick(client, Qt::Key_X);
        QVERIFY(dialog.profile().drive.refreshToken.isEmpty());
        QVERIFY(!ok->isEnabled());
    }

    void plainProfileNeedsNoStorage()
    {
        Settings::CatalogueProfile p;
        p.server = true;
        p.pg.pgHost = QStringLiteral("h");
        p.pg.pgDbName = QStringLiteral("db");
        p.pg.pgUser = QStringLiteral("u");
        p.userName = QStringLiteral("Bob");
        p.libraryRoot = QStringLiteral("/photos");
        CatalogueSettingsDialog dialog(p);
        QVERIFY(dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
        QCOMPARE(dialog.profile().photoStore, QString::fromLatin1(Settings::kPhotoStoreNone));
    }
};

QTEST_MAIN(TestCatalogueSettingsDialog)
#include "tst_cataloguesettingsdialog.moc"
