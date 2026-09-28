#include <QtTest>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "db/Database.h"
#include "sync/CatalogueSync.h"
#include "sync/ChangeSummary.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::sync;

class TestChangeSummary : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void nothingChanged();
    void describesEachKindOfChange();
    void capsTheItemList();

private:
    void exec(const QString &sql)
    {
        QSqlQuery q(QSqlDatabase::database(m_db->connectionName(), false));
        if (!q.exec(sql))
            qWarning().noquote() << sql << q.lastError().text();
    }
    // The kind with `key`; an empty stand-in (and a failure) when absent.
    const ChangeKind *kind(const ChangeSummary &s, const QString &key)
    {
        for (const ChangeKind &k : s.kinds) {
            if (k.key == key)
                return &k;
        }
        QStringList keys;
        for (const ChangeKind &k : s.kinds)
            keys << k.key;
        QTest::qFail(qPrintable(QStringLiteral("no \"%1\" kind (have: %2)")
                                    .arg(key, keys.join(QStringLiteral(", ")))),
                     __FILE__, __LINE__);
        static const ChangeKind none;
        return &none;
    }
    ChangeSummary summarize(int cap = 500)
    {
        QString error;
        const auto s = summarizeChanges(m_db->connectionName(), m_base, &error, cap);
        if (!s)
            qWarning().noquote() << error;
        return s.value_or(ChangeSummary{});
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    QString m_base;
    int m_tree = -1;
};

void TestChangeSummary::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    m_db = std::make_unique<Database>();
    QVERIFY(m_db->open(m_dir->filePath(QStringLiteral("catalogue.db"))));
    const QString c = m_db->connectionName();

    taxonomy::TaxonomyStore store(c);
    auto taxon = [&](qint64 id, const char *rank, const char *name) {
        taxonomy::Taxon t;
        t.inatId = id;
        t.rank = QLatin1String(rank);
        t.name = QLatin1String(name);
        store.upsertTaxon(t);
    };
    taxon(100, "genus", "Diuris");
    taxon(900, "species", "Diuris pardina");
    taxon(901, "species", "Diuris sulphurea");
    m_tree = store.ensureProject(QStringLiteral("Orchids"), 100, std::nullopt, QStringLiteral("inat"));
    store.addProjectTaxon(m_tree, 100, false, false);
    store.addProjectTaxon(m_tree, 900, true, false);
    store.addStatus(900, {std::nullopt, QStringLiteral("Rare"), QStringLiteral("VBA")});
    exec(QStringLiteral("INSERT INTO folder (id, path, name, depth) VALUES (1, '/p/Orchids', 'Orchids', 0)"));
    exec(QStringLiteral("INSERT INTO capture (id, folder_id, base_name, name_text) VALUES "
                        "(1, 1, 'IMG_1', ''), (2, 1, 'IMG_2', 'Leopard orchid')"));
    exec(QStringLiteral("INSERT INTO capture_match (id, capture_id, taxon_id, method, status) VALUES "
                        "(1, 1, (SELECT id FROM taxon WHERE inat_id = 900), 'folder', 'auto')"));

    m_base = m_dir->filePath(QStringLiteral("catalogue.base.db"));
    CatalogueSync sync(c, QString(), PathMapping());
    QVERIFY2(sync.snapshotTo(m_base), qPrintable(sync.error()));
}

void TestChangeSummary::cleanup()
{
    m_db.reset();
    m_dir.reset();
}

void TestChangeSummary::nothingChanged()
{
    const ChangeSummary s = summarize();
    QVERIFY(s.isEmpty());
    QCOMPARE(s.headline(), QStringLiteral("No changes"));
}

void TestChangeSummary::describesEachKindOfChange()
{
    // Re-identify one photo, identify another; rename the tree and add a
    // taxon to it; star a photo; learn a name; drop a status; and an
    // automatic cover photo.
    exec(QStringLiteral("UPDATE capture_match SET taxon_id = (SELECT id FROM taxon WHERE inat_id = 901), "
                        "status = 'confirmed' WHERE id = 1"));
    exec(QStringLiteral("INSERT INTO capture_match (id, capture_id, taxon_id, method, status) VALUES "
                        "(2, 2, (SELECT id FROM taxon WHERE inat_id = 900), 'user', 'confirmed')"));
    exec(QStringLiteral("UPDATE project SET name = 'Orchids of Victoria' WHERE id = %1").arg(m_tree));
    taxonomy::TaxonomyStore(m_db->connectionName()).addProjectTaxon(m_tree, 901, true, false);
    exec(QStringLiteral("INSERT INTO best_shot (capture_id) VALUES (2)"));
    exec(QStringLiteral("INSERT INTO name_alias (raw_text, raw_folded, taxon_id) VALUES "
                        "('Leopard', 'leopard', (SELECT id FROM taxon WHERE inat_id = 900))"));
    exec(QStringLiteral("DELETE FROM taxon_status"));
    // A status from the ALA: an automatic update, not one of yours.
    exec(QStringLiteral("INSERT INTO taxon_status (taxon_id, status, source) VALUES "
                        "((SELECT id FROM taxon WHERE inat_id = 901), 'Vulnerable', 'ALA dr1')"));
    exec(QStringLiteral("INSERT INTO representative (project_id, taxon_id, capture_id) VALUES "
                        "(%1, (SELECT id FROM taxon WHERE inat_id = 100), 2)").arg(m_tree));

    const ChangeSummary s = summarize();
    const ChangeKind *ids = kind(s, QStringLiteral("identifications"));
    QVERIFY(ids);
    QCOMPARE(ids->added, 1);
    QCOMPARE(ids->changed, 1);
    QVERIFY(ids->items.contains(QStringLiteral("Diuris pardina (confirmed) — Leopard orchid [IMG_2]")));
    QVERIFY(ids->items.contains(QStringLiteral("Diuris sulphurea (was: Diuris pardina) — IMG_1")));

    const ChangeKind *trees = kind(s, QStringLiteral("trees"));
    QVERIFY(trees);
    QCOMPARE(trees->items, QStringList{QStringLiteral("Renamed: Orchids → Orchids of Victoria")});
    QCOMPARE(kind(s, QStringLiteral("treeTaxa"))->items,
             QStringList{QStringLiteral("Orchids of Victoria: + Diuris sulphurea")});
    QCOMPARE(kind(s, QStringLiteral("bestShots"))->items,
             QStringList{QStringLiteral("★ Leopard orchid [IMG_2]")});
    QCOMPARE(kind(s, QStringLiteral("learnedNames"))->items,
             QStringList{QStringLiteral("Leopard → Diuris pardina")});
    QCOMPARE(kind(s, QStringLiteral("statuses"))->items,
             QStringList{QStringLiteral("Diuris pardina: Rare removed")});

    const ChangeKind *covers = kind(s, QStringLiteral("coverPhotos"));
    QVERIFY(covers && covers->automatic);
    const ChangeKind *ala = kind(s, QStringLiteral("alaStatuses"));
    QVERIFY(ala && ala->automatic);
    QCOMPARE(ala->items, QStringList{QStringLiteral("Diuris sulphurea: Vulnerable")});
    QCOMPARE(s.automaticRows(), 2);
    QCOMPARE(s.totalRows, 9);
    QVERIFY(s.headline().startsWith(
        QStringLiteral("Photo identifications (2) · Reference trees (1) · ")));
    QVERIFY(s.headline().endsWith(QStringLiteral(" · + 2 automatic updates")));
    // Automatic kinds come after the ones you did.
    QVERIFY(!s.kinds.first().automatic);
    QVERIFY(s.kinds.last().automatic);
}

void TestChangeSummary::capsTheItemList()
{
    taxonomy::TaxonomyStore store(m_db->connectionName());
    for (int i = 0; i < 30; ++i) {
        taxonomy::Taxon t;
        t.inatId = 5000 + i;
        t.rank = QStringLiteral("species");
        t.name = QStringLiteral("Diuris sp. %1").arg(i);
        store.upsertTaxon(t);
        store.addProjectTaxon(m_tree, t.inatId, true, false);
    }
    const ChangeSummary s = summarize(10);
    const ChangeKind *treeTaxa = kind(s, QStringLiteral("treeTaxa"));
    QCOMPARE(treeTaxa->added, 30);
    QCOMPARE(treeTaxa->items.size(), 10);
    QCOMPARE(treeTaxa->moreItems, 20);
}

QTEST_GUILESS_MAIN(TestChangeSummary)
#include "tst_changesummary.moc"
