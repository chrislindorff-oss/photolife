#include "treefile/TreeFile.h"

#include "checklist/ChecklistParser.h"
#include "coverage/CoverageCalculator.h"
#include "taxonomy/TaxonomyStore.h"
#include "util/Csv.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

#include "xlsxcellrange.h"
#include "xlsxdocument.h"
#include "xlsxformat.h"

#include <algorithm>
#include <functional>

namespace pl::treefile {
namespace {

// --- columns ---------------------------------------------------------------

enum class Col {
    Rank, Name, CommonName,
    Kingdom, Phylum, Class, Order, Family, Genus,
    Status, StatusSource, Origin, InRegion,
    Photographed, Photos, LatestPhoto, SpeciesPhotographed,
    InatId, ParentInatId, Ancestry, RankLevel, Active, FromChecklist, AllStatuses, AllOrigins,
};

struct ColumnDef
{
    Col col;
    const char *header;
};

// Written in this order. Everything from InatId on is PhotoLife's own data
// (grouped / collapsible in the .xlsx).
constexpr ColumnDef kColumns[] = {
    {Col::Rank, "Rank"},
    {Col::Name, "Scientific name"},
    {Col::CommonName, "Common name"},
    {Col::Kingdom, "Kingdom"},
    {Col::Phylum, "Phylum"},
    {Col::Class, "Class"},
    {Col::Order, "Order"},
    {Col::Family, "Family"},
    {Col::Genus, "Genus"},
    {Col::Status, "Status"},
    {Col::StatusSource, "Status source"},
    {Col::Origin, "Origin"},
    {Col::InRegion, "In region"},
    {Col::Photographed, "Photographed"},
    {Col::Photos, "Photos"},
    {Col::LatestPhoto, "Latest photo"},
    {Col::SpeciesPhotographed, "Species photographed"},
    {Col::InatId, "iNat ID"},
    {Col::ParentInatId, "Parent iNat ID"},
    {Col::Ancestry, "Ancestry"},
    {Col::RankLevel, "Rank level"},
    {Col::Active, "Active"},
    {Col::FromChecklist, "From checklist"},
    {Col::AllStatuses, "All statuses"},
    {Col::AllOrigins, "All origins"},
};
constexpr int kColumnCount = int(std::size(kColumns));
constexpr int kFirstPhotoLifeColumn = int(Col::InatId);

// The readable Kingdom..Genus columns: the rank each one shows.
constexpr std::pair<Col, const char *> kRankColumns[] = {
    {Col::Kingdom, "kingdom"}, {Col::Phylum, "phylum"}, {Col::Class, "class"},
    {Col::Order, "order"},     {Col::Family, "family"}, {Col::Genus, "genus"},
};

// Header cell -> column, for reading. Exact (case-insensitive) matches of our
// own headers first, then the checklist aliases a hand-made file might use.
QHash<Col, int> mapHeader(const QStringList &cells)
{
    QHash<Col, int> map;
    for (int i = 0; i < cells.size(); ++i) {
        const QString h = cells.at(i).trimmed().toLower();
        for (const ColumnDef &def : kColumns) {
            if (h == QString::fromLatin1(def.header).toLower() && !map.contains(def.col))
                map.insert(def.col, i);
        }
    }
    for (int i = 0; i < cells.size(); ++i) {
        const QString h = cells.at(i).trimmed().toLower();
        if (std::find(map.cbegin(), map.cend(), i) != map.cend())
            continue;   // already one of ours
        if (!map.contains(Col::Name) && checklist::isNameHeader(h))
            map.insert(Col::Name, i);
        else if (!map.contains(Col::CommonName)
                 && (h == QLatin1String("common name") || h == QLatin1String("vernacular name")))
            map.insert(Col::CommonName, i);
        else if (!map.contains(Col::Status) && h.contains(QLatin1String("status")))
            map.insert(Col::Status, i);
        else if (!map.contains(Col::Origin) && checklist::isOriginHeader(h))
            map.insert(Col::Origin, i);
    }
    return map;
}

// --- values ------------------------------------------------------------------

QString yesNo(bool b)
{
    return b ? QStringLiteral("yes") : QStringLiteral("no");
}

std::optional<bool> parseBool(const QString &text)
{
    const QString v = text.trimmed().toLower();
    if (v == QLatin1String("yes") || v == QLatin1String("y") || v == QLatin1String("true")
        || v == QLatin1String("1"))
        return true;
    if (v == QLatin1String("no") || v == QLatin1String("n") || v == QLatin1String("false")
        || v == QLatin1String("0"))
        return false;
    return std::nullopt;
}

std::optional<qint64> parseId(const QString &text)
{
    bool ok = false;
    const qint64 id = text.trimmed().toLongLong(&ok);
    return ok && id > 0 ? std::optional<qint64>(id) : std::nullopt;
}

QString factsJson(const QList<TreeFileFact> &facts)
{
    if (facts.isEmpty())
        return {};
    QJsonArray arr;
    for (const TreeFileFact &f : facts) {
        QJsonObject o;
        o.insert(QStringLiteral("value"), f.value);
        if (!f.source.isEmpty())
            o.insert(QStringLiteral("source"), f.source);
        if (f.placeInatId)
            o.insert(QStringLiteral("place"), double(*f.placeInatId));
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QList<TreeFileFact> parseFactsJson(const QString &text)
{
    QList<TreeFileFact> facts;
    const QJsonArray arr = QJsonDocument::fromJson(text.toUtf8()).array();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        TreeFileFact f;
        f.value = o.value(QStringLiteral("value")).toString().trimmed();
        f.source = o.value(QStringLiteral("source")).toString();
        if (const QJsonValue p = o.value(QStringLiteral("place")); p.isDouble())
            f.placeInatId = qint64(p.toDouble());
        if (!f.value.isEmpty())
            facts.append(f);
    }
    return facts;
}

// The fact the readable Status / Origin column shows: one for the tree's own
// place first, then one with no place, then any.
const TreeFileFact *headlineFact(const QList<TreeFileFact> &facts, std::optional<qint64> place)
{
    for (const TreeFileFact &f : facts) {
        if (place && f.placeInatId == place)
            return &f;
    }
    for (const TreeFileFact &f : facts) {
        if (!f.placeInatId)
            return &f;
    }
    return facts.isEmpty() ? nullptr : &facts.first();
}

// --- table <-> rows ----------------------------------------------------------

// One row per taxon, as cell text in kColumns order, plus each row's depth in
// the tree (for the .xlsx indent).
struct Table
{
    QList<QStringList> cells;
    QList<int> depths;
};

Table toTable(const TreeFile &file)
{
    QHash<qint64, const TreeFileRow *> byId;
    for (const TreeFileRow &row : file.rows) {
        if (row.inatId)
            byId.insert(*row.inatId, &row);
    }

    Table table;
    for (const TreeFileRow &row : file.rows) {
        // The names of the ranks this row sits in, itself included.
        QHash<QString, QString> rankNames;
        for (const QString &part : row.ancestry.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            if (const TreeFileRow *a = byId.value(part.toLongLong()))
                rankNames.insert(a->rank, a->name);
        }
        rankNames.insert(row.rank, row.name);

        int depth = 0;
        for (auto p = row.parentInatId; p && byId.contains(*p) && depth < 64;
             p = byId.value(*p)->parentInatId)
            ++depth;

        QStringList cells(kColumnCount);
        auto set = [&cells](Col c, const QString &v) { cells[int(c)] = v; };
        set(Col::Rank, row.rank);
        set(Col::Name, row.name);
        set(Col::CommonName, row.commonName);
        for (const auto &[col, rank] : kRankColumns)
            set(col, rankNames.value(QLatin1String(rank)));
        if (const TreeFileFact *s = headlineFact(row.statuses, file.placeInatId)) {
            set(Col::Status, s->value);
            set(Col::StatusSource, s->source);
        }
        if (const TreeFileFact *o = headlineFact(row.origins, file.placeInatId))
            set(Col::Origin, o->value);
        if (row.inRegion)
            set(Col::InRegion, yesNo(*row.inRegion));
        if (row.coverage) {
            const TreeFileRow::Coverage &c = *row.coverage;
            set(Col::Photographed, yesNo(c.photoCount > 0));
            set(Col::Photos, QString::number(c.photoCount));
            set(Col::LatestPhoto, c.latestPhoto);
            if (c.speciesTotal > 0 && !taxonomy::TaxonomyStore::isLeafRank(row.rank))
                set(Col::SpeciesPhotographed,
                    QStringLiteral("%1 / %2").arg(c.speciesWithPhotos).arg(c.speciesTotal));
        }
        if (row.inatId)
            set(Col::InatId, QString::number(*row.inatId));
        if (row.parentInatId)
            set(Col::ParentInatId, QString::number(*row.parentInatId));
        set(Col::Ancestry, row.ancestry);
        if (row.rankLevel)
            set(Col::RankLevel, QString::number(*row.rankLevel));
        set(Col::Active, yesNo(row.isActive));
        set(Col::FromChecklist, yesNo(row.fromChecklist));
        set(Col::AllStatuses, factsJson(row.statuses));
        set(Col::AllOrigins, factsJson(row.origins));
        table.cells.append(cells);
        table.depths.append(depth);
    }
    return table;
}

std::optional<TreeFileRow> rowFromCells(const QHash<Col, int> &map, const QStringList &cells)
{
    auto get = [&](Col c) -> QString {
        const int i = map.value(c, -1);
        return i >= 0 ? cells.value(i).trimmed() : QString();
    };

    TreeFileRow row;
    row.name = get(Col::Name);
    row.inatId = parseId(get(Col::InatId));
    if (row.name.startsWith(QLatin1Char('*'))) {   // VicFlora-style "introduced" marker
        row.name = row.name.mid(1).trimmed();
        if (!map.contains(Col::AllOrigins) && get(Col::Origin).isEmpty())
            row.origins.append({QStringLiteral("introduced"), {}, {}});
    }
    if (row.name.isEmpty() && !row.inatId)
        return std::nullopt;

    row.rank = get(Col::Rank).toLower();
    row.commonName = get(Col::CommonName);
    row.inRegion = parseBool(get(Col::InRegion));
    row.parentInatId = parseId(get(Col::ParentInatId));
    row.ancestry = get(Col::Ancestry);
    bool levelOk = false;
    const int level = get(Col::RankLevel).toInt(&levelOk);
    if (levelOk)
        row.rankLevel = level;
    row.isActive = parseBool(get(Col::Active)).value_or(true);
    row.fromChecklist = parseBool(get(Col::FromChecklist)).value_or(false);

    // The JSON columns carry every fact; without them (a hand-made or
    // hand-edited file) fall back to the readable columns.
    if (map.contains(Col::AllStatuses) && !get(Col::AllStatuses).isEmpty())
        row.statuses = parseFactsJson(get(Col::AllStatuses));
    else if (const QString s = get(Col::Status); !s.isEmpty())
        row.statuses.append({s, get(Col::StatusSource), {}});
    if (map.contains(Col::AllOrigins) && !get(Col::AllOrigins).isEmpty())
        row.origins = parseFactsJson(get(Col::AllOrigins));
    else if (const QString o = checklist::originFromValue(get(Col::Origin)); !o.isEmpty())
        row.origins = {{o, {}, {}}};
    return row;
}

// --- tree details ------------------------------------------------------------

const QString kKeyFormat = QStringLiteral("PhotoLife tree format");
const QString kKeyTree = QStringLiteral("Tree");
const QString kKeyRootId = QStringLiteral("Root taxon ID");
const QString kKeyRoot = QStringLiteral("Root taxon");
const QString kKeyPlaceId = QStringLiteral("Place ID");
const QString kKeyPlace = QStringLiteral("Place");
const QString kKeyExcluded = QStringLiteral("Excluded taxon IDs");
constexpr int kExcludedPerLine = 200;   // keeps each .xlsx cell well under Excel's limit

QList<std::pair<QString, QString>> detailPairs(const TreeFile &file)
{
    QList<std::pair<QString, QString>> pairs;
    pairs.append({kKeyFormat, QString::number(TreeFile::kFormatVersion)});
    pairs.append({kKeyTree, file.name});
    if (file.rootInatId) {
        pairs.append({kKeyRootId, QString::number(*file.rootInatId)});
        pairs.append({kKeyRoot, file.rootName});
    }
    if (file.placeInatId) {
        pairs.append({kKeyPlaceId, QString::number(*file.placeInatId)});
        pairs.append({kKeyPlace, file.placeName});
    }
    for (qsizetype i = 0; i < file.excludedInatIds.size(); i += kExcludedPerLine) {
        QStringList ids;
        for (qint64 id : file.excludedInatIds.mid(i, kExcludedPerLine))
            ids << QString::number(id);
        pairs.append({kKeyExcluded, ids.join(QLatin1Char(' '))});
    }
    return pairs;
}

void applyDetail(TreeFile &file, const QString &key, const QString &value)
{
    const QString k = key.trimmed();
    const QString v = value.trimmed();
    if (k.compare(kKeyFormat, Qt::CaseInsensitive) == 0)
        file.formatVersion = v.toInt();
    else if (k.compare(kKeyTree, Qt::CaseInsensitive) == 0)
        file.name = v;
    else if (k.compare(kKeyRootId, Qt::CaseInsensitive) == 0)
        file.rootInatId = parseId(v);
    else if (k.compare(kKeyRoot, Qt::CaseInsensitive) == 0)
        file.rootName = v;
    else if (k.compare(kKeyPlaceId, Qt::CaseInsensitive) == 0)
        file.placeInatId = parseId(v);
    else if (k.compare(kKeyPlace, Qt::CaseInsensitive) == 0)
        file.placeName = v;
    else if (k.compare(kKeyExcluded, Qt::CaseInsensitive) == 0) {
        static const QRegularExpression sep(QStringLiteral("[\\s,;]+"));
        for (const QString &part : v.split(sep, Qt::SkipEmptyParts)) {
            if (const auto id = parseId(part))
                file.excludedInatIds.append(*id);
        }
    }
}

void rowsFromTable(TreeFile &file, const QList<QStringList> &table)
{
    if (table.isEmpty())
        return;
    const QHash<Col, int> map = mapHeader(table.first());
    for (qsizetype i = 1; i < table.size(); ++i) {
        if (auto row = rowFromCells(map, table.at(i)))
            file.rows.append(*row);
    }
}

// --- CSV ---------------------------------------------------------------------

bool writeCsv(const QString &path, const TreeFile &file, QString *error)
{
    QString out;
    for (const auto &[key, value] : detailPairs(file))
        out += QStringLiteral("# %1: %2\n").arg(key, value);

    QStringList header;
    for (const ColumnDef &def : kColumns)
        header << QString::fromLatin1(def.header);
    out += header.join(QLatin1Char(',')) + QLatin1Char('\n');

    for (const QStringList &cells : toTable(file).cells) {
        QStringList fields;
        for (const QString &c : cells)
            fields << util::csvField(c);
        out += fields.join(QLatin1Char(',')) + QLatin1Char('\n');
    }

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        *error = f.errorString();
        return false;
    }
    // A BOM so Excel opens the file as UTF-8 (accented names, the × of hybrids).
    f.write("\xEF\xBB\xBF");
    f.write(out.toUtf8());
    if (!f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

std::optional<TreeFile> readCsv(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return std::nullopt;
    }
    QString text = QString::fromUtf8(f.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);

    TreeFile file;
    QList<QStringList> table;
    static const QRegularExpression lineBreak(QStringLiteral("\r\n|\n|\r"));
    for (const QString &line : text.split(lineBreak)) {
        if (line.trimmed().isEmpty())
            continue;
        if (table.isEmpty() && line.startsWith(QLatin1Char('#'))) {
            const QString detail = line.mid(1);
            const int colon = int(detail.indexOf(QLatin1Char(':')));
            if (colon > 0)
                applyDetail(file, detail.left(colon), detail.mid(colon + 1));
            continue;
        }
        table.append(util::splitCsvLine(line));
    }
    rowsFromTable(file, table);
    return file;
}

// --- .xlsx -------------------------------------------------------------------

const QString kTaxaSheet = QStringLiteral("Taxa");
const QString kTreeSheet = QStringLiteral("Tree");

bool writeXlsx(const QString &path, const TreeFile &file, QString *error)
{
    QXlsx::Document doc;
    // A new document has no sheet until something creates one.
    if (doc.sheetNames().isEmpty())
        doc.addSheet(kTaxaSheet);
    else
        doc.renameSheet(doc.sheetNames().first(), kTaxaSheet);
    doc.selectSheet(kTaxaSheet);

    QXlsx::Format header;
    header.setFontBold(true);
    header.setFillPattern(QXlsx::Format::PatternSolid);
    header.setPatternBackgroundColor(QColor(0xE8, 0xEE, 0xF4));
    for (int c = 0; c < kColumnCount; ++c)
        doc.write(1, c + 1, QString::fromLatin1(kColumns[c].header), header);

    const Table table = toTable(file);
    const int nameCol = int(Col::Name) + 1;
    const int idCols[] = {int(Col::Photos), int(Col::InatId), int(Col::ParentInatId),
                          int(Col::RankLevel)};
    for (qsizetype r = 0; r < table.cells.size(); ++r) {
        const QStringList &cells = table.cells.at(r);
        const int row = int(r) + 2;
        const bool leaf = taxonomy::TaxonomyStore::isLeafRank(cells.at(int(Col::Rank)));
        for (int c = 0; c < kColumnCount; ++c) {
            const QString &v = cells.at(c);
            if (v.isEmpty())
                continue;
            // Ids and levels as numbers, so Excel sorts and filters them as such.
            if (std::find(std::begin(idCols), std::end(idCols), c) != std::end(idCols)) {
                doc.write(row, c + 1, v.toLongLong());
                continue;
            }
            if (c + 1 == nameCol) {
                QXlsx::Format nameFormat;
                nameFormat.setIndent(std::min(table.depths.at(r), 15));
                nameFormat.setFontItalic(leaf || cells.at(int(Col::Rank)) == QLatin1String("genus"));
                nameFormat.setFontBold(!leaf);
                doc.write(row, c + 1, v, nameFormat);
                continue;
            }
            doc.write(row, c + 1, v);
        }
    }

    doc.setColumnWidth(int(Col::Rank) + 1, 12);
    doc.setColumnWidth(nameCol, 40);
    doc.setColumnWidth(int(Col::CommonName) + 1, 28);
    doc.setColumnWidth(int(Col::Kingdom) + 1, int(Col::Genus) + 1, 16);
    doc.setColumnWidth(int(Col::Status) + 1, int(Col::LatestPhoto) + 1, 14);
    doc.setColumnWidth(int(Col::SpeciesPhotographed) + 1, 20);
    doc.setColumnWidth(kFirstPhotoLifeColumn + 1, kColumnCount, 14);
    // PhotoLife's own columns, collapsed out of the way (the +/- above them).
    doc.groupColumns(kFirstPhotoLifeColumn + 1, kColumnCount, true);

    doc.addSheet(kTreeSheet);
    doc.selectSheet(kTreeSheet);
    QXlsx::Format key;
    key.setFontBold(true);
    int row = 1;
    for (const auto &[k, v] : detailPairs(file)) {
        doc.write(row, 1, k, key);
        doc.write(row, 2, v);
        ++row;
    }
    doc.setColumnWidth(1, 22);
    doc.setColumnWidth(2, 60);
    doc.selectSheet(kTaxaSheet);

    if (!doc.saveAs(path)) {
        *error = QObject::tr("could not write %1").arg(path);
        return false;
    }
    return true;
}

// A cell as text: whole numbers without a trailing ".0" (ids come back as
// doubles).
QString cellText(const QVariant &v)
{
    if (v.typeId() == QMetaType::Double) {
        const double d = v.toDouble();
        if (d == double(qint64(d)))
            return QString::number(qint64(d));
    }
    return v.toString();
}

std::optional<TreeFile> readXlsx(const QString &path, QString *error)
{
    QXlsx::Document doc(path);
    if (!doc.load()) {
        *error = QObject::tr("not a readable .xlsx workbook");
        return std::nullopt;
    }

    TreeFile file;
    const QStringList sheets = doc.sheetNames();
    if (sheets.contains(kTreeSheet)) {
        doc.selectSheet(kTreeSheet);
        const QXlsx::CellRange range = doc.dimension();
        for (int r = range.firstRow(); r <= range.lastRow(); ++r)
            applyDetail(file, cellText(doc.read(r, 1)), cellText(doc.read(r, 2)));
    }

    // The Taxa sheet, or a hand-made workbook's first sheet.
    doc.selectSheet(sheets.contains(kTaxaSheet) ? kTaxaSheet : sheets.value(0));
    const QXlsx::CellRange range = doc.dimension();
    QList<QStringList> table;
    for (int r = range.firstRow(); r <= range.lastRow(); ++r) {
        QStringList cells;
        bool any = false;
        for (int c = 1; c <= range.lastColumn(); ++c) {
            cells << cellText(doc.read(r, c));
            any = any || !cells.last().trimmed().isEmpty();
        }
        if (any)
            table.append(cells);
    }
    rowsFromTable(file, table);
    return file;
}

bool isXlsx(const QString &path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("xlsx"), Qt::CaseInsensitive) == 0;
}

} // namespace

std::optional<TreeFile> readTreeFile(const QString &path, QString *error)
{
    QString err;
    auto file = isXlsx(path) ? readXlsx(path, &err) : readCsv(path, &err);
    if (!file) {
        if (error)
            *error = err;
        return std::nullopt;
    }
    if (file->rows.isEmpty()) {
        if (error)
            *error = QObject::tr("no taxa found -- the file needs a \"Scientific name\" column");
        return std::nullopt;
    }
    return file;
}

bool writeTreeFile(const QString &path, const TreeFile &file, QString *error)
{
    QString err;
    const bool ok = isXlsx(path) ? writeXlsx(path, file, &err) : writeCsv(path, file, &err);
    if (!ok && error)
        *error = err;
    return ok;
}

TreeFile exportProject(const taxonomy::TaxonomyStore &store, int projectId,
                       const QString &treeName, const coverage::ProjectCoverage *coverage)
{
    TreeFile file;
    file.formatVersion = TreeFile::kFormatVersion;
    file.name = treeName;
    file.rootInatId = store.projectRootTaxonInatId(projectId);
    file.placeInatId = store.projectPlaceInatId(projectId);
    if (file.placeInatId) {
        if (const auto place = store.placeByInatId(*file.placeInatId))
            file.placeName = place->displayName.isEmpty() ? place->name : place->displayName;
    }
    file.excludedInatIds = store.projectExcludedTaxonIds(projectId);
    std::sort(file.excludedInatIds.begin(), file.excludedInatIds.end());

    const QList<taxonomy::TreeNode> nodes = store.projectTree(projectId);
    QList<qint64> ids;
    for (const auto &n : nodes)
        ids << n.inatId;
    const QHash<qint64, taxonomy::Taxon> taxa = store.taxaByInatIds(ids);

    QHash<qint64, QList<TreeFileFact>> statuses, origins;
    for (const auto &f : store.projectStatuses(projectId))
        statuses[f.taxonInatId].append({f.value, f.source, f.placeInatId});
    for (const auto &f : store.projectOrigins(projectId))
        origins[f.taxonInatId].append({f.value, f.source, f.placeInatId});

    // Depth-first from the top-level nodes, children by name.
    QHash<qint64, const taxonomy::TreeNode *> byId;
    for (const auto &n : nodes)
        byId.insert(n.inatId, &n);
    QHash<qint64, QList<const taxonomy::TreeNode *>> children;
    QList<const taxonomy::TreeNode *> tops;
    for (const auto &n : nodes) {
        if (n.parentInatId && byId.contains(*n.parentInatId))
            children[*n.parentInatId].append(&n);
        else
            tops.append(&n);
    }
    auto byName = [](const taxonomy::TreeNode *a, const taxonomy::TreeNode *b) {
        return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
    };
    // The root first, then any taxon whose parent the tree lacks (broadest
    // rank first), so the file opens on the tree's real top.
    std::sort(tops.begin(), tops.end(), [&](const taxonomy::TreeNode *a,
                                           const taxonomy::TreeNode *b) {
        const int la = taxa.value(a->inatId).rankLevel.value_or(0);
        const int lb = taxa.value(b->inatId).rankLevel.value_or(0);
        return la != lb ? la > lb : byName(a, b);
    });

    std::function<void(const taxonomy::TreeNode *)> visit = [&](const taxonomy::TreeNode *n) {
        TreeFileRow row;
        row.rank = n->rank;
        row.name = n->name;
        row.commonName = n->commonName;
        row.inRegion = n->inRegion;
        row.inatId = n->inatId;
        row.parentInatId = n->parentInatId;
        row.fromChecklist = n->fromChecklist;
        if (const auto it = taxa.constFind(n->inatId); it != taxa.constEnd()) {
            row.ancestry = it->ancestry;
            row.rankLevel = it->rankLevel;
            row.isActive = it->isActive;
        }
        row.statuses = statuses.value(n->inatId);
        row.origins = origins.value(n->inatId);
        if (coverage) {
            const coverage::TaxonCoverage tc = coverage->byTaxon.value(n->inatId);
            row.coverage = TreeFileRow::Coverage{tc.captureCount, tc.newestCapture.left(10),
                                                 tc.speciesTotal, tc.speciesWithPhotos};
        }
        file.rows.append(row);

        QList<const taxonomy::TreeNode *> kids = children.value(n->inatId);
        std::sort(kids.begin(), kids.end(), byName);
        for (const auto *k : kids)
            visit(k);
    };
    for (const auto *t : tops)
        visit(t);

    if (file.rootInatId) {
        if (const auto it = byId.constFind(*file.rootInatId); it != byId.constEnd())
            file.rootName = (*it)->name;
    }
    return file;
}

} // namespace pl::treefile
