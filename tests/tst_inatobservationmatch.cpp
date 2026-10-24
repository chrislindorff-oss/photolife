#include <QtTest>

#include <QCheckBox>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>

#include "FakeTransport.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "net/PhotoCache.h"
#include "ui/InatObservationMatchDialog.h"

using namespace pl;
using namespace pl::net;

namespace {

taxonomy::Observation obs(qint64 id, qint64 taxon, const QString &date)
{
    taxonomy::Observation o;
    o.id = id;
    o.taxonInatId = taxon;
    o.observedOn = date;
    return o;
}

QList<qint64> ids(const QList<taxonomy::Observation> &list)
{
    QList<qint64> out;
    for (const taxonomy::Observation &o : list)
        out << o.id;
    return out;
}

} // namespace

class TestInatObservationMatch : public QObject
{
    Q_OBJECT

private slots:
    void rankPutsTheirSpeciesFirstThenNearestDate();
    void speciesTextUsesWhatThereIs();
    void dialogSearchesEverySpeciesAndOffersWhatTheObservationHas();
};

void TestInatObservationMatch::rankPutsTheirSpeciesFirstThenNearestDate()
{
    const QList<taxonomy::Observation> list = {obs(1, 10, QStringLiteral("2025-01-09")),
                                               obs(2, 20, QStringLiteral("2025-01-10")),
                                               obs(3, 10, QStringLiteral("2025-01-10")),
                                               obs(4, 30, QString()),
                                               obs(5, 30, QStringLiteral("2025-01-10"))};
    const QDate centre(2025, 1, 10);
    // Species 10 first (nearest first), then the rest by date, newest id first.
    QCOMPARE(ids(InatObservationMatchDialog::rankCandidates(list, 10, centre)),
             (QList<qint64>{3, 1, 5, 2, 4}));
    // No species to prefer: by date alone.
    QCOMPARE(ids(InatObservationMatchDialog::rankCandidates(list, 0, centre)),
             (QList<qint64>{5, 3, 2, 1, 4}));
}

void TestInatObservationMatch::speciesTextUsesWhatThereIs()
{
    taxonomy::Observation o;
    o.taxonName = QStringLiteral("Petroica boodang");
    o.taxonCommonName = QStringLiteral("Scarlet Robin");
    o.taxonRank = QStringLiteral("species");
    QCOMPARE(InatObservationMatchDialog::speciesText(o),
             QStringLiteral("Scarlet Robin (Petroica boodang) · species"));
    o.taxonCommonName.clear();
    o.taxonRank.clear();
    QCOMPARE(InatObservationMatchDialog::speciesText(o), QStringLiteral("Petroica boodang"));
}

void TestInatObservationMatch::dialogSearchesEverySpeciesAndOffersWhatTheObservationHas()
{
    auto transport = std::make_unique<FakeTransport>();
    transport->responder = [](const Transport::Request &, int) {
        return FakeTransport::ok(QByteArrayLiteral(
            R"({"total_results": 2, "results": [
                {"id": 7, "observed_on": "2025-01-10",
                 "taxon": {"id": 900, "name": "Petroica boodang", "preferred_common_name": "Scarlet Robin", "rank": "species"},
                 "geojson": {"type": "Point", "coordinates": [144.9631, -37.8136]}, "photos": []},
                {"id": 8, "observed_on": "2025-01-10", "photos": []}
            ]})"));
    };
    FakeTransport *fake = transport.get();
    HttpClient http(std::move(transport), nullptr);
    http.setMinRequestIntervalMs(0);
    INatClient inat(http);
    inat.setBaseUrl(QStringLiteral("https://api.test/v1"));
    QTemporaryDir cacheDir;
    PhotoCache photos(cacheDir.path(), QByteArrayLiteral("test"));

    InatObservationMatchDialog::Selection sel;
    sel.photos = 2;
    sel.withGps = 1;
    sel.anyNeedsSpecies = true;
    sel.from = QDate(2025, 1, 10);
    sel.to = QDate(2025, 1, 12);
    InatObservationMatchDialog dialog(inat, photos, QStringLiteral("someone"), sel);

    auto *list = dialog.findChild<QListWidget *>();
    QTRY_COMPARE(list->count(), 2);
    const QString url = fake->received.at(0).url.toString();
    QVERIFY(!url.contains(QStringLiteral("taxon_id")));
    QVERIFY(url.contains(QStringLiteral("d1=2025-01-09")));
    QVERIFY(url.contains(QStringLiteral("d2=2025-01-13")));

    // The first (newest id 8) is unidentified with no location: nothing to apply.
    QCOMPARE(dialog.chosenObservation()->id, 8);
    QVERIFY(!dialog.applyCoordinates());
    QVERIFY(!dialog.applySpecies());

    // The robin: species and coordinates, both ticked; existing GPS kept.
    list->setCurrentRow(1);
    QCOMPARE(dialog.chosenObservation()->id, 7);
    QVERIFY(dialog.applySpecies());
    QVERIFY(dialog.applyCoordinates());
    QVERIFY(!dialog.replaceExistingCoordinates());
    QCheckBox *replace = nullptr;
    QCheckBox *coordinates = nullptr;
    for (QCheckBox *box : dialog.findChildren<QCheckBox *>()) {
        if (box->text().startsWith(QStringLiteral("Also replace")))
            replace = box;
        if (box->text().startsWith(QStringLiteral("Coordinates")))
            coordinates = box;
    }
    QVERIFY(replace && coordinates);
    replace->setChecked(true);
    QVERIFY(dialog.replaceExistingCoordinates());
    coordinates->setChecked(false);
    QVERIFY(!dialog.applyCoordinates());
    QVERIFY(!dialog.replaceExistingCoordinates());
}

QTEST_MAIN(TestInatObservationMatch)
#include "tst_inatobservationmatch.moc"
