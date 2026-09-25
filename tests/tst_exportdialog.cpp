#include <QtTest>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>

#include "collection/CollectionManifest.h"
#include "db/Database.h"
#include "taxonomy/TaxonomyStore.h"
#include "thumb/ThumbnailCache.h"
#include "ui/ExportDialog.h"

using namespace pl;

// The "Update an existing collection" folder check: which folders are
// accepted outright, which need the user's confirmation, and which are refused.
class TestExportDialog : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void acceptsAFolderMarkedForThisTreeEvenIfRenamed();
    void refusesAFolderMarkedForAnotherTree();
    void acceptsAnUnmarkedFolderNamedAfterTheTree();
    void offersTheCollectionWhenItsParentIsChosen();
    void requiresConfirmationForAnUnrecognisedFolder();
    void refusesAMissingFolder();

private:
    std::unique_ptr<Database> m_db;
    std::unique_ptr<QTemporaryDir> m_thumbDir;
    std::unique_ptr<thumb::ThumbnailCache> m_thumbs;
    QTemporaryDir m_tmp;
    int m_projectId = 0;

    // A dialog already switched to update mode, pointed at `folder`.
    std::unique_ptr<ExportDialog> openUpdate(const QString &folder);
    static bool okEnabled(ExportDialog &d);
    static QCheckBox *checkBox(ExportDialog &d, const QString &textStart);
    QString makeDir(const QString &relative);
};

void TestExportDialog::init()
{
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(QStringLiteral(":memory:")));
    m_thumbDir = std::make_unique<QTemporaryDir>();
    m_thumbs = std::make_unique<thumb::ThumbnailCache>(m_thumbDir->path());

    taxonomy::TaxonomyStore store(m_db->connectionName());
    m_projectId = store.ensureProject(QStringLiteral("Test Tree"), std::nullopt, std::nullopt,
                                      QStringLiteral("inat"));
    QVERIFY(m_projectId > 0);
}

void TestExportDialog::cleanup()
{
    m_thumbs.reset();
    m_db.reset();
}

std::unique_ptr<ExportDialog> TestExportDialog::openUpdate(const QString &folder)
{
    auto d = std::make_unique<ExportDialog>(*m_db, *m_thumbs, m_projectId,
                                            QStringLiteral("Test Tree (customized)"),
                                            QList<qint64>{1}, false, QString(), folder);
    for (QRadioButton *r : d->findChildren<QRadioButton *>()) {
        if (r->text().contains(QStringLiteral("Update")))
            r->click();
    }
    return d;
}

bool TestExportDialog::okEnabled(ExportDialog &d)
{
    return d.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled();
}

QCheckBox *TestExportDialog::checkBox(ExportDialog &d, const QString &textStart)
{
    for (QCheckBox *c : d.findChildren<QCheckBox *>()) {
        if (c->text().startsWith(textStart))
            return c;
    }
    return nullptr;
}

QString TestExportDialog::makeDir(const QString &relative)
{
    const QString path = m_tmp.filePath(relative);
    QDir().mkpath(path);
    return path;
}

void TestExportDialog::acceptsAFolderMarkedForThisTreeEvenIfRenamed()
{
    const QString folder = makeDir(QStringLiteral("marked/My Orchids"));
    collection::CollectionManifest m;
    m.projectId = m_projectId;
    m.projectName = QStringLiteral("Test Tree");
    m.includeCommonName = true;
    m.updatedAt = QDateTime::currentDateTimeUtc();
    QVERIFY(m.write(folder));

    auto d = openUpdate(folder);
    QVERIFY(d->updatesExistingCollection());
    QCOMPARE(d->destRoot(), folder);
    QVERIFY(okEnabled(*d));
    QVERIFY(!checkBox(*d, QStringLiteral("I'm sure"))->isVisibleTo(d.get()));

    // The collection's own naming style, locked.
    QCheckBox *common = checkBox(*d, QStringLiteral("Include common name"));
    QVERIFY(common->isChecked());
    QVERIFY(!common->isEnabled());
    QVERIFY(d->options().updateExisting);
    QVERIFY(d->options().includeCommonName);
}

void TestExportDialog::refusesAFolderMarkedForAnotherTree()
{
    const QString folder = makeDir(QStringLiteral("other/Test Tree"));
    collection::CollectionManifest m;
    m.projectId = m_projectId + 1;
    m.projectName = QStringLiteral("Birds");
    QVERIFY(m.write(folder));

    auto d = openUpdate(folder);
    QVERIFY(!okEnabled(*d));
    QVERIFY(!checkBox(*d, QStringLiteral("I'm sure"))->isVisibleTo(d.get()));
}

void TestExportDialog::acceptsAnUnmarkedFolderNamedAfterTheTree()
{
    const QString folder = makeDir(QStringLiteral("legacy/Test Tree"));
    makeDir(QStringLiteral("legacy/Test Tree/Orchidaceae  ·  Orchids"));

    auto d = openUpdate(folder);
    QVERIFY(okEnabled(*d));
    // Naming style inferred from the folders already there.
    QVERIFY(checkBox(*d, QStringLiteral("Include common name"))->isChecked());
}

void TestExportDialog::offersTheCollectionWhenItsParentIsChosen()
{
    const QString parent = makeDir(QStringLiteral("parent"));
    makeDir(QStringLiteral("parent/Test Tree"));

    auto d = openUpdate(parent);
    QVERIFY(!okEnabled(*d));
    QCheckBox *confirm = checkBox(*d, QStringLiteral("I'm sure"));
    QVERIFY(confirm->isVisibleTo(d.get()));

    QPushButton *use = nullptr;
    for (QPushButton *b : d->findChildren<QPushButton *>()) {
        if (b->text() == QStringLiteral("Use \"Test Tree\""))
            use = b;
    }
    QVERIFY(use && use->isVisibleTo(d.get()));
    use->click();
    QCOMPARE(d->destRoot(), QDir(parent).filePath(QStringLiteral("Test Tree")));
    QVERIFY(okEnabled(*d));
}

void TestExportDialog::requiresConfirmationForAnUnrecognisedFolder()
{
    const QString folder = makeDir(QStringLiteral("somewhere/Holiday Snaps"));

    auto d = openUpdate(folder);
    QVERIFY(!okEnabled(*d));
    QCheckBox *confirm = checkBox(*d, QStringLiteral("I'm sure"));
    QVERIFY(confirm->isVisibleTo(d.get()));
    confirm->setChecked(true);
    QVERIFY(okEnabled(*d));

    // A confirmation covers that one folder only.
    d->findChild<QLineEdit *>()->setText(makeDir(QStringLiteral("somewhere/Other")));
    QVERIFY(!confirm->isChecked());
    QVERIFY(!okEnabled(*d));
}

void TestExportDialog::refusesAMissingFolder()
{
    auto d = openUpdate(m_tmp.filePath(QStringLiteral("does-not-exist")));
    QVERIFY(!okEnabled(*d));
}

QTEST_MAIN(TestExportDialog)
#include "tst_exportdialog.moc"
