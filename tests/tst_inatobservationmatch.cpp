#include <QtTest>

#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QToolButton>
#include <QTemporaryDir>
#include <QTimeZone>

#include "FakeTransport.h"
#include "net/HttpClient.h"
#include "net/INatClient.h"
#include "net/PhotoCache.h"
#include "thumb/ThumbnailCache.h"
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
    void windowNarrowsByTime();
    void dialogWindowsSwitchWithoutSearchingAgain();
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
    sel.photoName = QStringLiteral("Petroica multicolor");
    sel.sampleBaseName = QStringLiteral("Petroica multicolor - Anakie 10-1-2025 (1)");
    sel.sampleNameText = QStringLiteral("Petroica multicolor");
    sel.sampleExtension = QStringLiteral("JPG");
    sel.preferCommonName = false;
    // The photos being matched, shown beside the observations.
    QTemporaryDir thumbDir;
    thumb::ThumbnailCache thumbs(thumbDir.filePath(QStringLiteral("thumbs")));
    for (const QColor &colour : {QColor(Qt::darkGreen), QColor(Qt::darkRed)}) {
        const QString path = thumbDir.filePath(colour.name().mid(1) + QStringLiteral(".jpg"));
        QImage image(600, 400, QImage::Format_RGB32);
        image.fill(colour);
        QVERIFY(image.save(path, "JPG"));
        sel.shown.append({path, QString(), QFileInfo(path).fileName()});
    }
    InatObservationMatchDialog dialog(inat, photos, QStringLiteral("someone"), sel, nullptr, &thumbs);
    dialog.show();
    QLabel *ownCaption = nullptr;
    for (QLabel *label : dialog.findChildren<QLabel *>())
        if (label->text().startsWith(QStringLiteral("1 of 2")))
            ownCaption = label;
    QVERIFY(ownCaption);
    QToolButton *next = nullptr;
    for (QToolButton *b : dialog.findChildren<QToolButton *>())
        if (b->arrowType() == Qt::RightArrow)
            next = b;
    QVERIFY(next && next->isVisible());
    next->click();
    QVERIFY(ownCaption->text().startsWith(QStringLiteral("2 of 2: 800000.jpg")));
    next->click();
    QVERIFY(ownCaption->text().startsWith(QStringLiteral("1 of 2")));

    auto *list = dialog.findChild<QListWidget *>();
    QTRY_COMPARE(list->count(), 2);
    // The name it's filed under, to set against iNat's.
    bool shown = false;
    for (QLabel *label : dialog.findChildren<QLabel *>())
        shown = shown || label->text().contains(QStringLiteral("filed as \u201cPetroica multicolor\u201d"));
    QVERIFY(shown);
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

    // Renaming: off until asked, then with the name chosen.
    QVERIFY(!dialog.renameFiles());
    QCheckBox *rename = nullptr;
    for (QCheckBox *box : dialog.findChildren<QCheckBox *>())
        if (box->text().startsWith(QStringLiteral("Also rename")))
            rename = box;
    QVERIFY(rename && rename->isEnabled());
    rename->setChecked(true);
    QVERIFY(dialog.renameFiles());
    QVERIFY(!dialog.renameWithCommonName());   // as preferred
    QCOMPARE(dialog.renameName(), QStringLiteral("Petroica boodang"));
    QRadioButton *common = nullptr;
    for (QRadioButton *b : dialog.findChildren<QRadioButton *>())
        if (b->text().startsWith(QStringLiteral("Common")))
            common = b;
    QVERIFY(common && common->isEnabled());
    common->setChecked(true);
    QCOMPARE(dialog.renameName(), QStringLiteral("Scarlet Robin"));
    bool example = false;
    for (QLabel *label : dialog.findChildren<QLabel *>())
        example = example || label->text().endsWith(QStringLiteral("→ Scarlet Robin - Anakie 10-1-2025 (1).JPG"));
    QVERIFY(example);
    if (qEnvironmentVariableIsSet("SHOT_OUT")) {
        dialog.show();
        QTest::qWait(300);
        dialog.grab().save(qEnvironmentVariable("SHOT_OUT"));
    }
    // Not without the species.
    for (QCheckBox *box : dialog.findChildren<QCheckBox *>())
        if (box->text().startsWith(QStringLiteral("Species")))
            box->setChecked(false);
    QVERIFY(!dialog.renameFiles());
    QVERIFY(!rename->isEnabled());
    replace->setChecked(true);
    QVERIFY(dialog.replaceExistingCoordinates());
    coordinates->setChecked(false);
    QVERIFY(!dialog.applyCoordinates());
    QVERIFY(!dialog.replaceExistingCoordinates());
}

namespace {

QDateTime at(int day, int hour, int minute)
{
    return QDateTime(QDate(2020, 6, day), QTime(hour, minute), QTimeZone::utc());
}

taxonomy::Observation timed(qint64 id, const QDateTime &when)
{
    taxonomy::Observation o = obs(id, 1, when.date().toString(Qt::ISODate));
    o.timeObserved = when;
    return o;
}

} // namespace

void TestInatObservationMatch::windowNarrowsByTime()
{
    using D = InatObservationMatchDialog;
    const QList<taxonomy::Observation> all = {
        timed(1, at(14, 10, 25)),                       // 5 minutes after the photo
        timed(2, at(14, 11, 30)),                       // 70 minutes after
        timed(3, at(14, 16, 0)),                        // that afternoon
        timed(4, at(15, 9, 0)),                         // the next day
        obs(5, 1, QStringLiteral("2020-06-14")),        // the day, but no time
    };
    const QList<QDateTime> photos = {at(14, 10, 20)};
    int noTime = 0;
    QCOMPARE(ids(D::withinWindow(all, photos, {}, D::Minutes, &noTime)), (QList<qint64>{1}));
    QCOMPARE(noTime, 1);
    QCOMPARE(ids(D::withinWindow(all, photos, {}, D::Hours)), (QList<qint64>{1, 2}));
    QCOMPARE(ids(D::withinWindow(all, photos, {}, D::Days)), (QList<qint64>{1, 2, 3, 4, 5}));
    // A photo with only a date matches on it, whatever the window.
    QCOMPARE(ids(D::withinWindow(all, {}, {QDate(2020, 6, 15)}, D::Minutes)), (QList<qint64>{4}));
    // Nearest in time first.
    QCOMPARE(ids(D::rankCandidates(D::withinWindow(all, photos, {}, D::Days), 0, QDate(2020, 6, 14), photos)),
             (QList<qint64>{1, 2, 3, 5, 4}));
}

void TestInatObservationMatch::dialogWindowsSwitchWithoutSearchingAgain()
{
    auto transport = std::make_unique<FakeTransport>();
    transport->responder = [](const Transport::Request &, int) {
        return FakeTransport::ok(QByteArrayLiteral(
            R"({"total_results": 3, "results": [
                {"id": 1, "observed_on": "2020-06-14", "time_observed_at": "2020-06-14T10:25:00+10:00", "photos": []},
                {"id": 2, "observed_on": "2020-06-14", "time_observed_at": "2020-06-14T11:30:00+10:00", "photos": []},
                {"id": 3, "observed_on": "2020-06-14", "photos": []}
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
    sel.photos = 1;
    sel.from = sel.to = QDate(2020, 6, 14);
    sel.times = {at(14, 10, 20)};
    InatObservationMatchDialog dialog(inat, photos, QStringLiteral("someone"), sel);
    auto *list = dialog.findChild<QListWidget *>();
    QTRY_COMPARE(list->count(), 1);   // within 10 minutes, by default
    QCOMPARE(dialog.window(), InatObservationMatchDialog::Minutes);
    QStringList labels;
    QList<QRadioButton *> windows;
    for (QRadioButton *b : dialog.findChildren<QRadioButton *>()) {
        if (b->text().contains(QLatin1Char('('))
            && (b->text().contains(QStringLiteral("minutes")) || b->text().contains(QStringLiteral("hours"))
                || b->text().contains(QStringLiteral("day")))) {
            labels << b->text();
            windows << b;
        }
    }
    QCOMPARE(labels, (QStringList{QStringLiteral("10 minutes (1)"), QStringLiteral("2 hours (2)"),
                                  QStringLiteral("A day either side (3)")}));
    windows.at(2)->setChecked(true);
    QCOMPARE(list->count(), 3);
    QCOMPARE(dialog.window(), InatObservationMatchDialog::Days);
    windows.at(1)->setChecked(true);
    QCOMPARE(list->count(), 2);
    QCOMPARE(fake->received.size(), 1);   // filtered here, not searched again
    // The name choice's buttons are a group of their own.
    for (QRadioButton *b : dialog.findChildren<QRadioButton *>())
        if (b->text().startsWith(QStringLiteral("Common")) || b->text().startsWith(QStringLiteral("Scientific")))
            b->setChecked(true);
    QVERIFY(windows.at(1)->isChecked());
}

QTEST_MAIN(TestInatObservationMatch)
#include "tst_inatobservationmatch.moc"
