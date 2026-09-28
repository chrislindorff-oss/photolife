#include "sync/ChangeSummary.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace pl::sync {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("pl::sync::ChangeSummary", text);
}

// How one table's changes are described. The item queries return one text
// column; in them `m` is a row of the live catalogue and `b` one of the
// base, and {ADDED} / {CHANGED} / {REMOVED} expand to the primary keys of
// that table's added / changed / removed rows. `filter` (a condition on
// the row as `x`) splits one table into several kinds.
struct Spec
{
    const char *table;
    const char *key;
    const char *title;
    bool automatic;
    const char *added = nullptr;
    const char *changed = nullptr;
    const char *removed = nullptr;
    const char *filter = nullptr;
};

// Display order. A photo's readable name, used throughout: its file name,
// with its own name added when the file name doesn't already say it
// ("Leopard orchid [IMG_1234]") -- the name alone is often just the species.
#define PHOTO(alias)                                                                        \
    "CASE WHEN COALESCE(" alias ".name_text, '') <> '' AND instr(" alias ".base_name, "     \
    alias ".name_text) = 0 THEN " alias ".name_text || ' [' || " alias ".base_name || ']' " \
    "ELSE COALESCE(" alias ".base_name, 'photo') END"

const Spec kSpecs[] = {
    {"capture_match", "identifications", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Photo identifications"), false,
     // The identification first, then which photo -- so a cut-off line still
     // shows what changed.
     "SELECT COALESCE(t.name, 'unidentified') "
     "  || CASE m.status WHEN 'confirmed' THEN ' (confirmed)' ELSE '' END || ' — ' || " PHOTO("c") " "
     "FROM main.capture_match m JOIN main.capture c ON c.id = m.capture_id "
     "LEFT JOIN main.taxon t ON t.id = m.taxon_id WHERE m.id IN ({ADDED})",
     "SELECT COALESCE(t.name, 'unidentified') || CASE "
     "  WHEN COALESCE(bt.name, '') <> COALESCE(t.name, '') "
     "    THEN ' (was: ' || COALESCE(bt.name, 'unidentified') || ')' "
     "  WHEN b.status <> m.status THEN ' (' || m.status || ', was ' || b.status || ')' "
     "  ELSE ' (details updated)' END || ' — ' || " PHOTO("c") " "
     "FROM main.capture_match m JOIN base.capture_match b ON b.id = m.id "
     "JOIN main.capture c ON c.id = m.capture_id "
     "LEFT JOIN main.taxon t ON t.id = m.taxon_id LEFT JOIN base.taxon bt ON bt.id = b.taxon_id "
     "WHERE m.id IN ({CHANGED})",
     "SELECT 'Identification removed — ' || " PHOTO("c") " FROM base.capture_match b "
     "LEFT JOIN base.capture c ON c.id = b.capture_id WHERE b.id IN ({REMOVED})"},
    {"project", "trees", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Reference trees"), false,
     "SELECT 'New tree: ' || m.name FROM main.project m WHERE m.id IN ({ADDED})",
     "SELECT CASE WHEN b.name <> m.name THEN 'Renamed: ' || b.name || ' → ' || m.name "
     "  ELSE 'Updated: ' || m.name END "
     "FROM main.project m JOIN base.project b ON b.id = m.id WHERE m.id IN ({CHANGED})",
     "SELECT 'Deleted: ' || b.name FROM base.project b WHERE b.id IN ({REMOVED})"},
    {"project_taxon", "treeTaxa", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Taxa in reference trees"), false,
     "SELECT p.name || ': + ' || t.name FROM main.project_taxon m "
     "JOIN main.project p ON p.id = m.project_id JOIN main.taxon t ON t.id = m.taxon_id "
     "WHERE (m.project_id, m.taxon_id) IN ({ADDED})",
     "SELECT p.name || ': ' || t.name || ' (details updated)' FROM main.project_taxon m "
     "JOIN main.project p ON p.id = m.project_id JOIN main.taxon t ON t.id = m.taxon_id "
     "WHERE (m.project_id, m.taxon_id) IN ({CHANGED})",
     "SELECT COALESCE(p.name, 'a deleted tree') || ': − ' || COALESCE(t.name, 'a taxon') "
     "FROM base.project_taxon b LEFT JOIN base.project p ON p.id = b.project_id "
     "LEFT JOIN base.taxon t ON t.id = b.taxon_id WHERE (b.project_id, b.taxon_id) IN ({REMOVED})"},
    {"project_excluded_taxon", "pruned", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Pruned groups"), false,
     "SELECT p.name || ': pruned ' || t.name FROM main.project_excluded_taxon m "
     "JOIN main.project p ON p.id = m.project_id JOIN main.taxon t ON t.id = m.taxon_id "
     "WHERE (m.project_id, m.taxon_id) IN ({ADDED})",
     nullptr,
     "SELECT COALESCE(p.name, 'a deleted tree') || ': restored ' || COALESCE(t.name, 'a taxon') "
     "FROM base.project_excluded_taxon b LEFT JOIN base.project p ON p.id = b.project_id "
     "LEFT JOIN base.taxon t ON t.id = b.taxon_id WHERE (b.project_id, b.taxon_id) IN ({REMOVED})"},
    {"taxon_status", "statuses", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Conservation statuses"), false,
     "SELECT t.name || ': ' || m.status || COALESCE(' (' || m.source || ')', '') "
     "FROM main.taxon_status m JOIN main.taxon t ON t.id = m.taxon_id WHERE m.id IN ({ADDED})",
     "SELECT t.name || ': ' || m.status || ' (was ' || b.status || ')' FROM main.taxon_status m "
     "JOIN base.taxon_status b ON b.id = m.id JOIN main.taxon t ON t.id = m.taxon_id "
     "WHERE m.id IN ({CHANGED})",
     "SELECT COALESCE(t.name, 'a taxon') || ': ' || b.status || ' removed' FROM base.taxon_status b "
     "LEFT JOIN base.taxon t ON t.id = b.taxon_id WHERE b.id IN ({REMOVED})",
     "COALESCE(x.source, '') NOT LIKE 'ALA %'"},
    {"taxon_origin", "origins", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Native / introduced"), false,
     "SELECT t.name || ': ' || m.origin || ' (' || m.source || ')' FROM main.taxon_origin m "
     "JOIN main.taxon t ON t.id = m.taxon_id WHERE m.id IN ({ADDED})",
     "SELECT t.name || ': ' || m.origin || ' (was ' || b.origin || ')' FROM main.taxon_origin m "
     "JOIN base.taxon_origin b ON b.id = m.id JOIN main.taxon t ON t.id = m.taxon_id "
     "WHERE m.id IN ({CHANGED})",
     "SELECT COALESCE(t.name, 'a taxon') || ': ' || b.origin || ' removed' FROM base.taxon_origin b "
     "LEFT JOIN base.taxon t ON t.id = b.taxon_id WHERE b.id IN ({REMOVED})",
     "x.source NOT IN ('APC', 'iNaturalist')"},
    {"best_shot", "bestShots", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Best shots"), false,
     "SELECT '★ ' || " PHOTO("c") " FROM main.best_shot m JOIN main.capture c ON c.id = m.capture_id "
     "WHERE m.capture_id IN ({ADDED})",
     nullptr,
     "SELECT '☆ ' || " PHOTO("c") " || ' (unstarred)' FROM base.best_shot b "
     "LEFT JOIN base.capture c ON c.id = b.capture_id WHERE b.capture_id IN ({REMOVED})"},
    {"name_alias", "learnedNames", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Learned names"), false,
     "SELECT m.raw_text || ' → ' || CASE WHEN m.not_a_taxon <> 0 THEN 'not a taxon' "
     "  ELSE COALESCE(t.name, '?') END FROM main.name_alias m "
     "LEFT JOIN main.taxon t ON t.id = m.taxon_id WHERE m.id IN ({ADDED})",
     "SELECT m.raw_text || ' → ' || CASE WHEN m.not_a_taxon <> 0 THEN 'not a taxon' "
     "  ELSE COALESCE(t.name, '?') END || ' (updated)' FROM main.name_alias m "
     "LEFT JOIN main.taxon t ON t.id = m.taxon_id WHERE m.id IN ({CHANGED})",
     "SELECT 'Forgot: ' || b.raw_text FROM base.name_alias b WHERE b.id IN ({REMOVED})"},
    {"capture", "photos", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Photos"), false,
     "SELECT " PHOTO("m") " || COALESCE(' (' || f.name || ')', '') FROM main.capture m "
     "LEFT JOIN main.folder f ON f.id = m.folder_id WHERE m.id IN ({ADDED})",
     "SELECT " PHOTO("m") " || ' (details updated)' FROM main.capture m WHERE m.id IN ({CHANGED})",
     "SELECT " PHOTO("b") " || ' removed' FROM base.capture b WHERE b.id IN ({REMOVED})"},
    {"folder", "folders", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Library folders"), false,
     "SELECT m.path FROM main.folder m WHERE m.id IN ({ADDED})",
     "SELECT m.path || ' (updated)' FROM main.folder m WHERE m.id IN ({CHANGED})",
     "SELECT b.path || ' (removed)' FROM base.folder b WHERE b.id IN ({REMOVED})"},
    {"capture_keyword_hint", "keywordHints", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Lightroom keyword hints"), false},
    // Derived or background data.
    {"taxon_status", "alaStatuses", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Conservation statuses (Atlas of Living Australia)"), true,
     "SELECT t.name || ': ' || m.status FROM main.taxon_status m "
     "JOIN main.taxon t ON t.id = m.taxon_id WHERE m.id IN ({ADDED})",
     "SELECT t.name || ': ' || m.status || ' (was ' || b.status || ')' FROM main.taxon_status m "
     "JOIN base.taxon_status b ON b.id = m.id JOIN main.taxon t ON t.id = m.taxon_id "
     "WHERE m.id IN ({CHANGED})",
     "SELECT COALESCE(t.name, 'a taxon') || ': ' || b.status || ' removed' FROM base.taxon_status b "
     "LEFT JOIN base.taxon t ON t.id = b.taxon_id WHERE b.id IN ({REMOVED})",
     "x.source LIKE 'ALA %'"},
    {"taxon_origin", "autoOrigins", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Native / introduced (APC, iNaturalist)"), true,
     "SELECT t.name || ': ' || m.origin || ' (' || m.source || ')' FROM main.taxon_origin m "
     "JOIN main.taxon t ON t.id = m.taxon_id WHERE m.id IN ({ADDED})",
     nullptr, nullptr, "x.source IN ('APC', 'iNaturalist')"},
    {"rendition", "photoFiles", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Photo files (scan details)"), true},
    {"taxon", "taxa", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Taxa from iNaturalist"), true,
     "SELECT m.name || ' (' || m.rank || ')' FROM main.taxon m WHERE m.id IN ({ADDED})",
     "SELECT CASE WHEN b.name <> m.name THEN b.name || ' → ' || m.name "
     "  ELSE m.name || ' (details updated)' END "
     "FROM main.taxon m JOIN base.taxon b ON b.id = m.id WHERE m.id IN ({CHANGED})",
     "SELECT b.name || ' removed' FROM base.taxon b WHERE b.id IN ({REMOVED})"},
    {"taxon_name", "taxonNames", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Taxon names and synonyms"), true},
    {"representative", "coverPhotos", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Tree cover photos"), true},
    {"geocode_cache", "localities", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Photo localities"), true,
     "SELECT m.locality FROM main.geocode_cache m WHERE m.id IN ({ADDED})"},
    {"place", "places", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Place details"), true,
     "SELECT COALESCE(m.display_name, m.name) FROM main.place m WHERE m.inat_id IN ({ADDED})",
     "SELECT COALESCE(m.display_name, m.name) || ' (updated)' FROM main.place m "
     "WHERE m.inat_id IN ({CHANGED})"},
    {"project_build_checkpoint", "buildProgress", QT_TRANSLATE_NOOP("pl::sync::ChangeSummary", "Tree build progress"), true},
};

#undef PHOTO

const QSet<QString> kSkipped = {QStringLiteral("schema_migrations"), QStringLiteral("catalogue_sync")};

QString quoted(const QString &id)
{
    return QLatin1Char('"') + id + QLatin1Char('"');
}

struct TableInfo
{
    QStringList pk;
    bool inBase = true;
};

} // namespace

int ChangeSummary::automaticRows() const
{
    int n = 0;
    for (const ChangeKind &k : kinds) {
        if (k.automatic)
            n += k.total();
    }
    return n;
}

QString ChangeSummary::headline() const
{
    if (isEmpty())
        return tr("No changes");
    QStringList parts;
    for (const ChangeKind &k : kinds) {
        if (!k.automatic)
            parts << QStringLiteral("%1 (%2)").arg(k.title, QLocale().toString(k.total()));
    }
    if (const int automatic = automaticRows(); automatic > 0) {
        const QString count = QLocale().toString(automatic);
        parts << (automatic == 1 ? tr("%1 automatic update") : tr("%1 automatic updates")).arg(count)
                     .prepend(parts.isEmpty() ? QString() : QStringLiteral("+ "));
    }
    return parts.join(QStringLiteral(" · "));
}

std::optional<ChangeSummary> summarizeChanges(const QString &localConnection,
                                              const QString &basePath, QString *error,
                                              int itemCap)
{
    auto failed = [&](const QString &message) -> std::optional<ChangeSummary> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!QFile::exists(basePath))
        return failed(tr("No record of the last sync (%1).").arg(basePath));

    QSqlDatabase db = QSqlDatabase::database(localConnection, false);
    {
        QSqlQuery attach(db);
        attach.prepare(QStringLiteral("ATTACH DATABASE ? AS base"));
        attach.addBindValue(basePath);
        if (!attach.exec())
            return failed(attach.lastError().text());
    }
    struct Detach
    {
        QSqlDatabase db;
        ~Detach() { QSqlQuery(db).exec(QStringLiteral("DETACH DATABASE base")); }
    } detach{db};

    // Every content table, with its primary key and whether the base has it.
    QHash<QString, TableInfo> tables;
    QStringList order;
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral("SELECT name FROM main.sqlite_master WHERE type = 'table' "
                              "AND name NOT LIKE 'sqlite_%' ORDER BY name"));
        while (q.next()) {
            const QString name = q.value(0).toString();
            if (kSkipped.contains(name))
                continue;
            TableInfo info;
            QSqlQuery cols(db);
            cols.exec(QStringLiteral("PRAGMA main.table_info(%1)").arg(quoted(name)));
            QList<std::pair<int, QString>> pk;
            while (cols.next()) {
                if (const int pos = cols.value(5).toInt(); pos > 0)
                    pk.append({pos, cols.value(1).toString()});
            }
            std::sort(pk.begin(), pk.end());
            for (const auto &p : pk)
                info.pk << p.second;
            QSqlQuery inBase(db);
            inBase.prepare(QStringLiteral(
                "SELECT 1 FROM base.sqlite_master WHERE type = 'table' AND name = ?"));
            inBase.addBindValue(name);
            info.inBase = inBase.exec() && inBase.next();
            if (!info.pk.isEmpty()) {
                tables.insert(name, info);
                order << name;
            }
        }
    }

    ChangeSummary summary;
    QSet<QString> described;
    auto describe = [&](const QString &table, const QString &key, const QString &title,
                        bool automatic, const char *added, const char *changed,
                        const char *removed, const char *filterSql) -> bool {
        const QString filter = filterSql ? QString::fromLatin1(filterSql) : QStringLiteral("1 = 1");
        const TableInfo info = tables.value(table);
        ChangeKind kind;
        kind.key = key;
        kind.title = title;
        kind.automatic = automatic;
        const QString pk = QStringList(info.pk).replaceInStrings(QRegularExpression(QStringLiteral("^(.*)$")),
                                                                 QStringLiteral("\"\\1\""))
                               .join(QStringLiteral(", "));
        const QString t = quoted(table);
        QSqlQuery q(db);
        if (!info.inBase) {   // a table new since the last sync: all of it is new
            if (q.exec(QStringLiteral("SELECT COUNT(*) FROM main.%1").arg(t)) && q.next())
                kind.added = q.value(0).toInt();
        } else {
            // Rows are judged as they are now (added, changed) and as they
            // were (removed). A row that moved between kinds -- e.g. a
            // status whose source changed -- leaves one kind and joins the
            // other, rather than counting as "changed" in only one.
            const QString addedKeys =
                QStringLiteral("SELECT %1 FROM main.%2 x WHERE %3 "
                               "EXCEPT SELECT %1 FROM base.%2 x WHERE %3")
                    .arg(pk, t, filter);
            const QString removedKeys =
                QStringLiteral("SELECT %1 FROM base.%2 x WHERE %3 "
                               "EXCEPT SELECT %1 FROM main.%2 x WHERE %3")
                    .arg(pk, t, filter);
            const QString changedKeys =
                QStringLiteral("SELECT %1 FROM (SELECT * FROM main.%2 EXCEPT SELECT * FROM base.%2) x "
                               "WHERE %3 INTERSECT SELECT %1 FROM base.%2 x WHERE %3")
                    .arg(pk, t, filter);
            auto count = [&](const QString &keys) {
                return q.exec(QStringLiteral("SELECT COUNT(*) FROM (%1)").arg(keys)) && q.next()
                           ? q.value(0).toInt()
                           : 0;
            };
            kind.added = count(addedKeys);
            kind.changed = count(changedKeys);
            kind.removed = count(removedKeys);
            auto items = [&](const char *sql, const QString &keys, int n) {
                if (!sql || n == 0)
                    return;
                QString text = QString::fromUtf8(sql);
                text.replace(QStringLiteral("{ADDED}"), keys)
                    .replace(QStringLiteral("{CHANGED}"), keys)
                    .replace(QStringLiteral("{REMOVED}"), keys);
                const int room = itemCap - int(kind.items.size());
                if (room <= 0) {
                    kind.moreItems += n;
                    return;
                }
                QSqlQuery lines(db);
                if (!lines.exec(text + QStringLiteral(" LIMIT %1").arg(room))) {
                    kind.moreItems += n;   // unreadable (e.g. an older base): counts only
                    return;
                }
                int got = 0;
                while (lines.next()) {
                    kind.items << lines.value(0).toString();
                    ++got;
                }
                kind.moreItems += n - got;
            };
            items(added, addedKeys, kind.added);
            items(changed, changedKeys, kind.changed);
            items(removed, removedKeys, kind.removed);
        }
        if (kind.total() > 0) {
            summary.kinds << kind;
            summary.totalRows += kind.total();
        }
        described.insert(table);
        return true;
    };

    for (const Spec &s : kSpecs) {
        const QString table = QString::fromLatin1(s.table);
        if (tables.contains(table))
            describe(table, QString::fromLatin1(s.key), tr(s.title), s.automatic, s.added,
                     s.changed, s.removed, s.filter);
    }
    // Anything not described above still counts.
    for (const QString &table : std::as_const(order)) {
        if (!described.contains(table))
            describe(table, table, tr("Other: %1").arg(table), true, nullptr, nullptr, nullptr,
                     nullptr);
    }
    // What you did first; automatic updates after.
    std::stable_sort(summary.kinds.begin(), summary.kinds.end(),
                     [](const ChangeKind &a, const ChangeKind &b) { return !a.automatic && b.automatic; });
    return summary;
}

} // namespace pl::sync
