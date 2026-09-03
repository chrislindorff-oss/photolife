#include "scan/FilenameParser.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace pl::scan {
namespace {

// Organ / phenology / colour words that appear as "(bud)", "_leaf", "(yellow)".
// Kept deliberately small: a false positive would swallow part of a real name.
const QSet<QString> &organTagWords()
{
    static const QSet<QString> words = {
        // organs / structures
        QStringLiteral("bud"),      QStringLiteral("buds"),
        QStringLiteral("flower"),   QStringLiteral("flowers"),
        QStringLiteral("fruit"),    QStringLiteral("fruits"),
        QStringLiteral("leaf"),     QStringLiteral("leaves"),
        QStringLiteral("bark"),     QStringLiteral("seed"),
        QStringLiteral("seeds"),    QStringLiteral("stem"),
        QStringLiteral("root"),     QStringLiteral("roots"),
        QStringLiteral("bulb"),     QStringLiteral("tuber"),
        QStringLiteral("habit"),    QStringLiteral("habitat"),
        QStringLiteral("seedling"), QStringLiteral("rosette"),
        QStringLiteral("trunk"),    QStringLiteral("cone"),
        QStringLiteral("cones"),    QStringLiteral("frond"),
        QStringLiteral("fronds"),   QStringLiteral("whole"),
        QStringLiteral("labellum"), QStringLiteral("column"),
        // colours
        QStringLiteral("white"),    QStringLiteral("yellow"),
        QStringLiteral("red"),      QStringLiteral("pink"),
        QStringLiteral("purple"),   QStringLiteral("blue"),
        QStringLiteral("green"),    QStringLiteral("orange"),
        QStringLiteral("cream"),    QStringLiteral("mauve"),
        QStringLiteral("brown"),    QStringLiteral("black"),
    };
    return words;
}

// Matches every " - " / " – " / " — " (hyphen, en-dash, em-dash) separator.
const QRegularExpression &separatorRe()
{
    static const QRegularExpression re(QStringLiteral(R"(\s+[-\x{2013}\x{2014}]\s+)"));
    return re;
}

// A d-m-yyyy (or d.m.yyyy) date, optionally followed by " (n)" and/or a "_suffix"
// run, anchored at the end of the string.
const QRegularExpression &trailingDateRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"((?<!\d)(\d{1,2})[-.](\d{1,2})[-.](\d{4})(?!\d)\s*(?:\((\d+)\))?\s*(?:_\S+)?\s*$)"));
    return re;
}

// A bare " (n)" counter at the end, used when there is no date.
const QRegularExpression &trailingSeqRe()
{
    static const QRegularExpression re(QStringLiteral(R"(\s*\((\d+)\)\s*$)"));
    return re;
}

QDate makeDate(int a, int b, int year)
{
    // Library convention is day-month-year; fall back to month-day-year only if
    // that is the sole reading that yields a valid date.
    const QDate dmy(year, b, a);
    if (dmy.isValid())
        return dmy;
    const QDate mdy(year, a, b);
    return mdy;
}

// Pulls "(...)" and "_word" organ tags off a name fragment, returning the
// cleaned name plus the tags in the order they appeared. A parenthetical or an
// underscore run is only treated as tags when *every* word in it is a known
// organ tag; otherwise it is left in the name (informal-form markers like
// "(Upright form)", fauna common-name pairs like "Jacky Winter_White-winged
// Triller").
void extractOrganTags(const QString &fragment, QString *name, QStringList *tags)
{
    static const QRegularExpression parenRe(QStringLiteral(R"(\(([^()]*)\))"));
    static const QRegularExpression innerSplitRe(QStringLiteral(R"([,/]\s*)"));

    QString rebuilt;
    qsizetype last = 0;
    auto it = parenRe.globalMatch(fragment);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        rebuilt += fragment.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();

        const QStringList parts =
            m.captured(1).trimmed().split(innerSplitRe, Qt::SkipEmptyParts);
        bool allTags = !parts.isEmpty();
        for (const QString &part : parts) {
            if (!isOrganTag(part.trimmed())) {
                allTags = false;
                break;
            }
        }
        if (allTags) {
            for (const QString &part : parts)
                tags->append(part.trimmed().toLower());
            rebuilt += QLatin1Char(' ');
        } else {
            rebuilt += m.captured(0);
        }
    }
    rebuilt += fragment.mid(last);

    QString cleaned = rebuilt.trimmed();
    const qsizetype underscore = cleaned.indexOf(QLatin1Char('_'));
    if (underscore >= 0) {
        const QString head = cleaned.left(underscore);
        const QStringList rest = cleaned.mid(underscore + 1)
                                     .split(QLatin1Char('_'), Qt::SkipEmptyParts);
        bool allTags = !rest.isEmpty();
        for (const QString &w : rest) {
            if (!isOrganTag(w.trimmed())) {
                allTags = false;
                break;
            }
        }
        if (allTags) {
            for (const QString &w : rest)
                tags->append(w.trimmed().toLower());
            cleaned = head.trimmed();
        }
    }

    *name = cleaned.simplified();
}

} // namespace

bool isOrganTag(const QString &word)
{
    return organTagWords().contains(word.trimmed().toLower());
}

ParsedFilename parseStem(const QString &stem)
{
    ParsedFilename out;

    const QString trimmed = stem.trimmed();
    if (trimmed.isEmpty())
        return out;

    // Split into the name fragment and the "locality + date" remainder on the
    // first separator. Everything after the first separator is the remainder,
    // even if it contains further separators (localities rarely do; dates never).
    QString namePart = trimmed;
    QString rest;
    const QRegularExpressionMatch sep = separatorRe().match(trimmed);
    if (sep.hasMatch()) {
        namePart = trimmed.left(sep.capturedStart());
        rest = trimmed.mid(sep.capturedEnd());
    }

    // Date (and duplicate counter) from the end of the remainder.
    QString localityPart = rest;
    const QRegularExpressionMatch date = trailingDateRe().match(rest);
    if (date.hasMatch()) {
        out.capturedOn = makeDate(date.captured(1).toInt(),
                                  date.captured(2).toInt(),
                                  date.captured(3).toInt());
        if (!date.captured(4).isEmpty())
            out.sequence = date.captured(4).toInt();
        localityPart = rest.left(date.capturedStart());
    } else {
        const QRegularExpressionMatch seq = trailingSeqRe().match(rest);
        if (seq.hasMatch()) {
            out.sequence = seq.captured(1).toInt();
            localityPart = rest.left(seq.capturedStart());
        }
    }

    out.locality = localityPart.trimmed();
    while (out.locality.endsWith(QLatin1Char(',')) || out.locality.endsWith(QLatin1Char('-')))
        out.locality.chop(1);
    out.locality = out.locality.trimmed();

    // A trailing "(n)" on the name fragment is a duplicate counter, not a tag —
    // whether or not a separator was present.
    if (out.sequence == 0) {
        const QRegularExpressionMatch seq = trailingSeqRe().match(namePart);
        if (seq.hasMatch()) {
            out.sequence = seq.captured(1).toInt();
            namePart = namePart.left(seq.capturedStart());
        }
    }

    extractOrganTags(namePart, &out.name, &out.organTags);
    return out;
}

ParsedFilename parseFileName(const QString &fileName)
{
    return parseStem(captureBaseName(fileName));
}

QString captureBaseName(const QString &fileName)
{
    const QFileInfo info(fileName);
    return info.completeBaseName();
}

} // namespace pl::scan
