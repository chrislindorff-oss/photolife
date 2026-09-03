#include "match/PathClassifier.h"

#include "match/NameParser.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>

namespace pl::match {
namespace {

const QRegularExpression &numberPrefixRe()
{
    static const QRegularExpression re(QStringLiteral(R"(^\s*\d+\s*[.)]\s*)"));
    return re;
}

QString stripNumbering(const QString &name)
{
    QString s = name;
    s.remove(numberPrefixRe());
    return s.trimmed();
}

bool isAllCaps(const QString &s)
{
    return s == s.toUpper() && s != s.toLower();
}

// Informal grouping words used in this library, plus the generic Linnaean
// "informal rank" folders ("Superfamily BOMBYCOIDEA", "Suborder ...").
bool isGroupWord(const QString &lowered)
{
    static const QSet<QString> words = {
        QStringLiteral("gymnosperms"),   QStringLiteral("ferns"),
        QStringLiteral("ferns & fern allies"), QStringLiteral("fern allies"),
        QStringLiteral("monocotyledons"), QStringLiteral("dicotyledons"),
        QStringLiteral("monocots"),      QStringLiteral("dicots"),
        QStringLiteral("fungi"),         QStringLiteral("bryophytes"),
        QStringLiteral("lichen"),        QStringLiteral("lichens"),
        QStringLiteral("algae"),         QStringLiteral("mosses"),
        QStringLiteral("liverworts"),    QStringLiteral("hornworts"),
        QStringLiteral("angiosperms"),   QStringLiteral("conifers"),
        QStringLiteral("flowering plants"), QStringLiteral("vascular plants"),
        QStringLiteral("birds"),         QStringLiteral("mammals"),
        QStringLiteral("reptiles"),      QStringLiteral("amphibians"),
        QStringLiteral("fish"),          QStringLiteral("insects"),
        QStringLiteral("invertebrates"), QStringLiteral("arthropods"),
    };
    if (words.contains(lowered))
        return true;

    static const QRegularExpression informalRank(
        QStringLiteral(R"(^(super|sub|infra|parv|magn)?(kingdom|phylum|division|class|order|cohort|family|tribe)\s+\S)"),
        QRegularExpression::CaseInsensitiveOption);
    return informalRank.match(lowered).hasMatch();
}

bool endsWithFamilySuffix(const QString &word)
{
    const QString l = word.toLower();
    return l.endsWith(QLatin1String("aceae")) || l.endsWith(QLatin1String("idae"))
           || l.endsWith(QLatin1String("inae"));
}

bool isSingleCapitalisedWord(const QString &s)
{
    static const QRegularExpression re(QStringLiteral(R"(^[A-Z][a-z\-]+$)"));
    return re.match(s).hasMatch();
}

} // namespace

FolderClass classifyFolderName(const QString &folderName)
{
    FolderClass out;
    const QString trimmed = folderName.trimmed();
    if (trimmed.isEmpty())
        return out;

    const QString stripped = stripNumbering(trimmed);
    const QString lowered = stripped.toLower();

    // Staging folders.
    static const QRegularExpression stagingRe(
        QStringLiteral(R"(\b(to sort|to upload|staging|unsorted|new folder|duplicates?|rejects?|for id|to id|jpg_original)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    if (stagingRe.match(lowered).hasMatch()) {
        out.kind = FolderKind::Staging;
        return out;
    }

    // Informal groups: known words, or numbered, or SHOUTY multi-word labels.
    const bool wasNumbered = numberPrefixRe().match(trimmed).hasMatch();
    if (isGroupWord(lowered)
        || (wasNumbered && isAllCaps(stripped))
        || (isAllCaps(stripped) && stripped.contains(QLatin1Char(' ')))) {
        out.kind = FolderKind::Group;
        return out;
    }

    // Locality folders.
    if (looksNonTaxonomic(stripped)) {
        out.kind = FolderKind::Locality;
        return out;
    }

    const ParsedName parsed = parseName(stripped);

    static const QRegularExpression validGenusRe(QStringLiteral(R"(^[A-Z][a-zA-Z\-]+$)"));
    static const QRegularExpression validEpithetRe(QStringLiteral(R"(^[a-z][a-z\-]+$)"));
    const bool validGenus = validGenusRe.match(parsed.genus).hasMatch();
    const bool validEpithet = parsed.specificEpithet.isEmpty()
                              || validEpithetRe.match(parsed.specificEpithet).hasMatch();
    // A Latin binomial's epithet is lower-case in the folder name too; an
    // upper-case second word ("Pacific Black Duck") means it is a common name.
    const QStringList words = stripped.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const bool secondWordLower = words.size() < 2 || (!words.at(1).isEmpty()
                                                      && words.at(1).front().isLower());
    if (!validGenus || !validEpithet || (parsed.hasSpecies() && !secondWordLower))
        return out;   // Unknown

    if (parsed.hasSpecies() && !parsed.infraEpithet.isEmpty()) {
        out.kind = FolderKind::Taxon;
        out.rank = QStringLiteral("infraspecies");
        out.inferredName = parsed.canonical();
        return out;
    }
    if (parsed.hasSpecies()) {
        out.kind = FolderKind::Taxon;
        out.rank = QStringLiteral("species");
        out.inferredName = parsed.canonical();
        return out;
    }
    if (parsed.isGenusOnly() && endsWithFamilySuffix(parsed.genus)) {
        out.kind = FolderKind::Taxon;
        out.rank = QStringLiteral("family");
        out.inferredName = parsed.genus;
        return out;
    }
    if (parsed.isGenusOnly() && isSingleCapitalisedWord(parsed.genus)) {
        out.kind = FolderKind::Taxon;
        out.rank = QStringLiteral("genus");
        out.inferredName = parsed.genus;
        return out;
    }

    return out;   // Unknown; classifyFolders() may still resolve it from context
}

int classifyFolders(const QString &connectionName)
{
    QSqlDatabase db = QSqlDatabase::database(connectionName, false);

    struct Row
    {
        int id = 0;
        int parentId = -1;
        int depth = 0;
        QString name;
        FolderClass klass;
    };

    QHash<int, Row> rows;
    QHash<int, QList<int>> childrenOf;

    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral("SELECT id, parent_id, depth, name FROM folder ORDER BY depth")))
            return 0;
        while (q.next()) {
            Row r;
            r.id = q.value(0).toInt();
            r.parentId = q.value(1).isNull() ? -1 : q.value(1).toInt();
            r.depth = q.value(2).toInt();
            r.name = q.value(3).toString();
            r.klass = classifyFolderName(r.name);
            rows.insert(r.id, r);
            childrenOf[r.parentId].append(r.id);
        }
    }

    auto nearestKind = [&](int id, FolderKind want) -> bool {
        for (int cur = id; rows.contains(cur);) {
            const Row &r = rows.value(cur);
            if (r.klass.kind == want)
                return true;
            cur = r.parentId;
        }
        return false;
    };

    // Resolve the Unknowns from context.
    for (auto &r : rows) {
        if (r.klass.kind != FolderKind::Unknown)
            continue;

        const bool underGroup = nearestKind(r.parentId, FolderKind::Group);
        const bool underTaxon = nearestKind(r.parentId, FolderKind::Taxon);
        const ParsedName parsed = parseName(stripNumbering(r.name));

        // A multi-word Title-Case phrase directly under a group (no genus above)
        // is a Fauna common-name species folder ("Pacific Black Duck").
        static const QRegularExpression commonNameRe(
            QStringLiteral(R"(^([A-Z][a-z'-]+ ){1,4}[A-Z][a-z'-]+$)"));

        if (underTaxon && !underGroup && parsed.genus.isEmpty()) {
            r.klass.kind = FolderKind::Locality;
        } else if (parsed.hasGenus() && parsed.isGenusOnly()) {
            // A capitalised-ish word under a group/family: call it a genus.
            r.klass.kind = FolderKind::Taxon;
            r.klass.rank = QStringLiteral("genus");
            r.klass.inferredName = parsed.genus;
        } else if (underGroup && !underTaxon
                   && commonNameRe.match(stripNumbering(r.name)).hasMatch()) {
            r.klass.kind = FolderKind::Taxon;
            r.klass.rank = QStringLiteral("species");
            r.klass.inferredName = stripNumbering(r.name);
        } else if (underTaxon) {
            r.klass.kind = FolderKind::Locality;
        }
    }

    int updated = 0;
    if (!db.transaction())
        return 0;
    for (const auto &r : rows) {
        QSqlQuery up(db);
        up.prepare(QStringLiteral(
            "UPDATE folder SET kind = ?, inferred_rank = ?, inferred_name = ? WHERE id = ?"));
        const char *kindStr = "unknown";
        switch (r.klass.kind) {
        case FolderKind::Group:    kindStr = "group"; break;
        case FolderKind::Taxon:    kindStr = "taxon"; break;
        case FolderKind::Locality: kindStr = "locality"; break;
        case FolderKind::Staging:  kindStr = "staging"; break;
        case FolderKind::Unknown:  kindStr = "unknown"; break;
        }
        up.addBindValue(QString::fromLatin1(kindStr));
        up.addBindValue(r.klass.rank.isEmpty() ? QVariant() : r.klass.rank);
        up.addBindValue(r.klass.inferredName.isEmpty() ? QVariant() : r.klass.inferredName);
        up.addBindValue(r.id);
        if (up.exec())
            ++updated;
    }
    db.commit();
    return updated;
}

} // namespace pl::match
