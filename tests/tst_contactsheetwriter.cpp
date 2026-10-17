#include <QtTest>

#include <QComboBox>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTemporaryDir>

#include "collection/ContactSheetWriter.h"
#include "ui/ContactSheetDialog.h"

using namespace pl;
using namespace pl::collection;

class TestContactSheetWriter : public QObject
{
    Q_OBJECT

private slots:
    void moreColumnsGiveSmallerThumbsAndMorePerPage();
    void landscapeWidensAndFewerCaptionLinesAddRows();
    void optionsRoundTripThroughJson();
    void writesAPdfWithTheExpectedPageCount();
    void groupsAndCreditsAddPages();
    void missingPhotosAreLeftOutUnlessIncluded();
    void continuousGroupingKeepsAFullGrid();
    void longNamesWrapRatherThanBeingCutOff();
    void noPhotosIsAnError();
    void dialogPresetsFollowTheColumnCount();

private:
    static QList<ContactSheetEntry> makeEntries(int count, int withoutPhoto = 0);
    static int pdfPageCount(const QString &path);
};

QList<ContactSheetEntry> TestContactSheetWriter::makeEntries(int count, int withoutPhoto)
{
    QList<ContactSheetEntry> entries;
    for (int i = 0; i < count; ++i) {
        ContactSheetEntry e;
        e.name = QStringLiteral("Genus species%1").arg(i, 3, 10, QLatin1Char('0'));
        e.commonName = QStringLiteral("Common %1").arg(i);
        e.attribution = QStringLiteral("(c) Someone %1, some rights reserved (CC BY)").arg(i);
        e.group = i % 2 ? QStringLiteral("Orchidaceae") : QStringLiteral("Asteraceae");
        if (i >= withoutPhoto) {
            QImage img(i % 3 ? 320 : 200, i % 3 ? 240 : 320, QImage::Format_RGB32);
            img.fill(QColor::fromHsv((i * 37) % 360, 160, 200));
            e.photo = img;
        }
        entries.append(e);
    }
    return entries;
}

int TestContactSheetWriter::pdfPageCount(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    const QString pdf = QString::fromLatin1(f.readAll());
    static const QRegularExpression page(QStringLiteral("/Type\\s*/Page(?!s)"));
    int n = 0;
    for (auto it = page.globalMatch(pdf); it.hasNext(); it.next())
        ++n;
    return n;
}

void TestContactSheetWriter::moreColumnsGiveSmallerThumbsAndMorePerPage()
{
    ContactSheetOptions o;
    o.columns = 3;
    const ContactSheetLayout few = computeContactSheetLayout(o);
    o.columns = 8;
    const ContactSheetLayout many = computeContactSheetLayout(o);

    QVERIFY(many.thumb < few.thumb);
    QVERIFY(many.perPage() > few.perPage());
    QVERIFY(few.rowsPerPage >= 1);
    // The grid fits inside the page margins.
    QVERIFY(few.columns * few.thumb + (few.columns - 1) * few.gutter
            <= few.page.width() - 2 * few.margin + 0.001);

    o.columns = 99;   // clamped
    QCOMPARE(computeContactSheetLayout(o).columns, ContactSheetOptions::kMaxColumns);
}

void TestContactSheetWriter::landscapeWidensAndFewerCaptionLinesAddRows()
{
    ContactSheetOptions o;
    o.columns = 5;
    const ContactSheetLayout portrait = computeContactSheetLayout(o);
    o.orientation = QPageLayout::Landscape;
    const ContactSheetLayout landscape = computeContactSheetLayout(o);
    QVERIFY(landscape.page.width() > landscape.page.height());
    QVERIFY(landscape.thumb > portrait.thumb);

    o.orientation = QPageLayout::Portrait;
    o.captionScientific = false;
    o.captionCommon = false;
    o.credits = ContactSheetOptions::NoCredits;
    o.showDate = o.showSpeciesCount = o.showPageNumbers = false;
    const ContactSheetLayout bare = computeContactSheetLayout(o);
    QCOMPARE(bare.captionHeight, 0.0);
    QCOMPARE(bare.headerHeight, 0.0);
    QVERIFY(bare.rowsPerPage > portrait.rowsPerPage);
}

void TestContactSheetWriter::optionsRoundTripThroughJson()
{
    ContactSheetOptions o;
    o.pageSize = QPageSize::Letter;
    o.orientation = QPageLayout::Landscape;
    o.columns = 7;
    o.cropSquare = false;
    o.showDate = false;
    o.captionCommon = false;
    o.includeMissing = true;
    o.groupRank = QStringLiteral("family");
    o.groupNewRow = false;
    o.credits = ContactSheetOptions::CreditsUnderPhoto;
    o.title = QStringLiteral("not persisted");

    const ContactSheetOptions back = ContactSheetOptions::fromJson(o.toJson());
    QCOMPARE(back.pageSize, o.pageSize);
    QCOMPARE(back.orientation, o.orientation);
    QCOMPARE(back.columns, 7);
    QCOMPARE(back.cropSquare, false);
    QCOMPARE(back.showDate, false);
    QCOMPARE(back.captionCommon, false);
    QCOMPARE(back.includeMissing, true);
    QCOMPARE(back.groupRank, QStringLiteral("family"));
    QCOMPARE(back.groupNewRow, false);
    QCOMPARE(back.credits, ContactSheetOptions::CreditsUnderPhoto);
    QVERIFY(back.title.isEmpty());

    // Defaults from an empty object; junk values fall back.
    const ContactSheetOptions defaults = ContactSheetOptions::fromJson(
        {{QStringLiteral("groupRank"), QStringLiteral("kingdom")},
         {QStringLiteral("columns"), 1}});
    QVERIFY(defaults.groupRank.isEmpty());
    QCOMPARE(defaults.columns, ContactSheetOptions::kMinColumns);
    QCOMPARE(defaults.credits, ContactSheetOptions::CreditsPage);
}

void TestContactSheetWriter::writesAPdfWithTheExpectedPageCount()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("sheet.pdf"));

    ContactSheetOptions o;
    o.title = QStringLiteral("Test Tree");
    o.columns = 4;
    o.credits = ContactSheetOptions::NoCredits;
    const ContactSheetLayout l = computeContactSheetLayout(o);
    const int count = l.perPage() * 2 + 1;   // spills onto a third page

    QString error;
    int pages = 0;
    QVERIFY2(writeContactSheet(path, makeEntries(count), o, &error, &pages), qPrintable(error));
    QCOMPARE(pages, 3);

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(f.read(4) == "%PDF");
    f.close();
    QCOMPARE(pdfPageCount(path), 3);
}

void TestContactSheetWriter::groupsAndCreditsAddPages()
{
    QTemporaryDir dir;
    ContactSheetOptions o;
    o.title = QStringLiteral("Test Tree");
    o.columns = 4;
    o.credits = ContactSheetOptions::NoCredits;
    const ContactSheetLayout l = computeContactSheetLayout(o);
    const QList<ContactSheetEntry> entries = makeEntries(l.perPage());   // exactly one page

    int plain = 0;
    QVERIFY(writeContactSheet(dir.filePath(QStringLiteral("a.pdf")), entries, o, nullptr, &plain));
    QCOMPARE(plain, 1);

    // A group per photo: each takes a row of its own under a heading, so the
    // same photos need several pages.
    QList<ContactSheetEntry> oneEach = entries;
    for (int i = 0; i < oneEach.size(); ++i)
        oneEach[i].group = QStringLiteral("Family %1").arg(i, 3, 10, QLatin1Char('0'));
    o.groupRank = QStringLiteral("family");
    int grouped = 0;
    QVERIFY(writeContactSheet(dir.filePath(QStringLiteral("b.pdf")), oneEach, o, nullptr,
                              &grouped));
    QVERIFY(grouped >= int(oneEach.size()) / l.rowsPerPage);
    QVERIFY(grouped > plain);
    QCOMPARE(pdfPageCount(dir.filePath(QStringLiteral("b.pdf"))), grouped);

    o.groupRank.clear();
    o.credits = ContactSheetOptions::CreditsPage;
    int credited = 0;
    QVERIFY(writeContactSheet(dir.filePath(QStringLiteral("c.pdf")), entries, o, nullptr,
                              &credited));
    QVERIFY(credited >= 2);
    QCOMPARE(pdfPageCount(dir.filePath(QStringLiteral("c.pdf"))), credited);
}

void TestContactSheetWriter::missingPhotosAreLeftOutUnlessIncluded()
{
    QTemporaryDir dir;
    ContactSheetOptions o;
    o.columns = 4;
    o.credits = ContactSheetOptions::NoCredits;
    const ContactSheetLayout l = computeContactSheetLayout(o);
    // A full page with photos plus some without: those spill only if included.
    const QList<ContactSheetEntry> entries = makeEntries(l.perPage() + 3, 3);

    int pages = 0;
    QVERIFY(writeContactSheet(dir.filePath(QStringLiteral("a.pdf")), entries, o, nullptr, &pages));
    QCOMPARE(pages, 1);

    o.includeMissing = true;
    QVERIFY(writeContactSheet(dir.filePath(QStringLiteral("b.pdf")), entries, o, nullptr, &pages));
    QCOMPARE(pages, 2);
}

void TestContactSheetWriter::continuousGroupingKeepsAFullGrid()
{
    QTemporaryDir dir;
    ContactSheetOptions o;
    o.columns = 4;
    o.credits = ContactSheetOptions::NoCredits;
    o.groupRank = QStringLiteral("family");
    o.groupNewRow = false;
    const ContactSheetLayout l = computeContactSheetLayout(o);
    QVERIFY(l.groupStrip > 0);

    // A new group every 3 photos: none starts a row of its own, so exactly
    // one page's worth of photos still fills exactly one page.
    QList<ContactSheetEntry> entries = makeEntries(l.perPage());
    for (int i = 0; i < entries.size(); ++i)
        entries[i].group = QStringLiteral("Family %1").arg(i / 3, 3, 10, QLatin1Char('0'));

    int pages = 0;
    QVERIFY(writeContactSheet(dir.filePath(QStringLiteral("a.pdf")), entries, o, nullptr, &pages));
    QCOMPARE(pages, 1);

    o.groupNewRow = true;   // the same photos, a row and heading per group
    QCOMPARE(computeContactSheetLayout(o).groupStrip, 0.0);
    QVERIFY(writeContactSheet(dir.filePath(QStringLiteral("b.pdf")), entries, o, nullptr, &pages));
    QVERIFY(pages > 1);
}

void TestContactSheetWriter::longNamesWrapRatherThanBeingCutOff()
{
    QTemporaryDir dir;
    ContactSheetOptions o;
    o.credits = ContactSheetOptions::NoCredits;
    ContactSheetCaptionLines two;
    two.scientific = 2;

    // A narrow (6+ across) column count where a second caption line costs a
    // row, so wrapping shows up in the page count.
    ContactSheetLayout oneLine;
    for (o.columns = 6; o.columns <= ContactSheetOptions::kMaxColumns; ++o.columns) {
        oneLine = computeContactSheetLayout(o);
        QVERIFY(computeContactSheetLayout(o, two).captionHeight > oneLine.captionHeight);
        if (computeContactSheetLayout(o, two).rowsPerPage < oneLine.rowsPerPage)
            break;
    }
    QVERIFY(o.columns <= ContactSheetOptions::kMaxColumns);

    // Short names fit one page; names far too long for the cell take a second
    // caption line, so the same count spills onto another page.
    QList<ContactSheetEntry> entries = makeEntries(oneLine.perPage());
    int pages = 0;
    QVERIFY(writeContactSheet(dir.filePath(QStringLiteral("a.pdf")), entries, o, nullptr, &pages));
    QCOMPARE(pages, 1);

    for (ContactSheetEntry &e : entries)
        e.name = QStringLiteral("Caladenia longicauda subsp. eminens");
    QVERIFY(writeContactSheet(dir.filePath(QStringLiteral("b.pdf")), entries, o, nullptr, &pages));
    QCOMPARE(pages, 2);
}

void TestContactSheetWriter::noPhotosIsAnError()
{
    QTemporaryDir dir;
    QString error;
    QVERIFY(!writeContactSheet(dir.filePath(QStringLiteral("a.pdf")), makeEntries(5, 5),
                               ContactSheetOptions(), &error));
    QVERIFY(!error.isEmpty());
}

void TestContactSheetWriter::dialogPresetsFollowTheColumnCount()
{
    ContactSheetOptions initial;
    initial.columns = 4;
    initial.title = QStringLiteral("My Tree");
    initial.groupRank = QStringLiteral("genus");
    ContactSheetDialog dialog(initial, QStringLiteral("/tmp/x.pdf"), 10, 8);

    auto *spin = dialog.findChild<QSpinBox *>();
    QVERIFY(spin);
    QComboBox *preset = nullptr;
    for (QComboBox *box : dialog.findChildren<QComboBox *>()) {
        if (box->findText(QStringLiteral("Custom")) >= 0)
            preset = box;
    }
    QVERIFY(preset);
    QCOMPARE(preset->currentText(), QStringLiteral("Medium"));

    spin->setValue(7);
    QCOMPARE(preset->currentText(), QStringLiteral("Custom"));
    preset->setCurrentIndex(preset->findText(QStringLiteral("Small")));
    emit preset->activated(preset->currentIndex());
    QCOMPARE(spin->value(), 6);

    const ContactSheetOptions o = dialog.options();
    QCOMPARE(o.columns, 6);
    QCOMPARE(o.title, QStringLiteral("My Tree"));
    QCOMPARE(o.groupRank, QStringLiteral("genus"));
    QCOMPARE(dialog.outputPath(), QStringLiteral("/tmp/x.pdf"));
}

QTEST_MAIN(TestContactSheetWriter)
#include "tst_contactsheetwriter.moc"
