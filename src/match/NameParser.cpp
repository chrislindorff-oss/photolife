#include "match/NameParser.h"

#include <QRegularExpression>
#include <QSet>

namespace pl::match {
namespace {

QString lc(const QString &s)
{
    return s.toLower();
}

// Strips a leading token if it matches, returning true and removing it.
bool takeLeading(QStringList &tokens, const QSet<QString> &words)
{
    if (!tokens.isEmpty() && words.contains(lc(tokens.first()))) {
        tokens.removeFirst();
        return true;
    }
    return false;
}

bool isSpMarker(const QString &t)
{
    const QString l = lc(t);
    return l == QLatin1String("sp") || l == QLatin1String("sp.")
           || l == QLatin1String("spp") || l == QLatin1String("spp.")
           || l == QLatin1String("species");
}

bool isAffMarker(const QString &t)
{
    const QString l = lc(t);
    return l == QLatin1String("aff") || l == QLatin1String("aff.")
           || l == QLatin1String("cf") || l == QLatin1String("cf.");
}

QString normaliseInfraRank(const QString &t)
{
    const QString l = lc(t);
    if (l.startsWith(QLatin1String("subsp")) || l.startsWith(QLatin1String("ssp")))
        return QStringLiteral("subsp");
    if (l.startsWith(QLatin1String("var")))
        return QStringLiteral("var");
    if (l == QLatin1String("f") || l == QLatin1String("f.") || l.startsWith(QLatin1String("form")))
        return QStringLiteral("f");
    return {};
}

bool isHybridSign(const QString &t)
{
    return t == QLatin1String("x") || t == QLatin1String("X")
           || t == QString::fromUtf8("\xC3\x97");  // U+00D7 multiplication sign
}

// Removes trailing "sensu lato / stricto" and aggregate markers, setting the
// qualifier. Returns the remaining string.
QString stripTrailingQualifiers(QString s, Qualifier *qualifier)
{
    static const QRegularExpression aggRe(
        QStringLiteral(R"(\s+(?:spp?\.?\s+)?agg\.?\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression slRe(
        QStringLiteral(R"(\s+(?:s\.?\s*l\.?|sensu\s+lato)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ssRe(
        QStringLiteral(R"(\s+(?:s\.?\s*s\.?|sensu\s+stricto)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);

    if (s.contains(aggRe)) {
        s.remove(aggRe);
        *qualifier = Qualifier::Aggregate;
    } else if (s.contains(slRe)) {
        s.remove(slRe);
        *qualifier = Qualifier::SensuLato;
    } else if (s.contains(ssRe)) {
        s.remove(ssRe);
        *qualifier = Qualifier::SensuStricto;
    }
    return s.trimmed();
}

} // namespace

ParsedName parseName(const QString &raw)
{
    ParsedName out;
    out.raw = raw.trimmed();
    if (out.raw.isEmpty())
        return out;

    QString work = out.raw;

    // "Unidentified Caladenia" / "Caladenia indet." → Unidentified.
    static const QRegularExpression unidLead(
        QStringLiteral(R"(^(?:unidentified|unknown|indet\.?|unid\.?)\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression unidTrail(
        QStringLiteral(R"(\s+indet\.?\s*$)"), QRegularExpression::CaseInsensitiveOption);
    bool unidentified = false;
    if (work.contains(unidLead)) {
        work.remove(unidLead);
        unidentified = true;
    }
    if (work.contains(unidTrail)) {
        work.remove(unidTrail);
        unidentified = true;
    }

    work = stripTrailingQualifiers(work, &out.qualifier);

    // Pull a trailing parenthetical (an informal-form tag) aside before tokenising.
    static const QRegularExpression trailingParen(QStringLiteral(R"(\s*\(([^()]*)\)\s*$)"));
    QString parenTag;
    if (const auto m = trailingParen.match(work); m.hasMatch()) {
        parenTag = m.captured(1).trimmed();
        work = work.left(m.capturedStart()).trimmed();
    }

    QStringList tokens = work.split(QRegularExpression(QStringLiteral(R"(\s+)")),
                                    Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        if (unidentified)
            out.qualifier = Qualifier::Unidentified;
        return out;
    }

    // Genus (with a "Corybas_Corysanthes" synonym pair).
    QString first = tokens.takeFirst();
    if (first.contains(QLatin1Char('_'))) {
        out.altGenera = first.split(QLatin1Char('_'), Qt::SkipEmptyParts);
        out.genus = out.altGenera.value(0);
    } else {
        out.genus = first;
    }

    if (unidentified) {
        out.qualifier = Qualifier::Unidentified;
        return out;
    }

    // "sp" / "sp." / "sp. aff." / "sp. 1" / "sp. (form)".
    if (!tokens.isEmpty() && isSpMarker(tokens.first())) {
        tokens.removeFirst();
        if (out.qualifier == Qualifier::None)
            out.qualifier = Qualifier::Sp;

        if (!tokens.isEmpty() && isAffMarker(tokens.first())) {
            tokens.removeFirst();
            out.qualifier = Qualifier::SpAff;
            if (!tokens.isEmpty())
                out.specificEpithet = tokens.takeFirst().toLower();
        } else if (!tokens.isEmpty()
                   && tokens.first().contains(QRegularExpression(QStringLiteral(R"(^\d)")))) {
            out.qualifier = Qualifier::Undescribed;
            out.informalTag = tokens.join(QLatin1Char(' '));
            tokens.clear();
        } else if (!parenTag.isEmpty()) {
            out.qualifier = Qualifier::Undescribed;
        }

        if (!parenTag.isEmpty() && out.informalTag.isEmpty())
            out.informalTag = parenTag;
        return out;
    }

    // "cf." / "aff." straight after the genus, before an epithet.
    if (!tokens.isEmpty() && isAffMarker(tokens.first())) {
        tokens.removeFirst();
        out.qualifier = Qualifier::SpAff;
    }

    // Specific epithet.
    if (!tokens.isEmpty()) {
        out.specificEpithet = tokens.takeFirst().toLower();
    }

    // Hybrid: "epithet x epithet2".
    if (!tokens.isEmpty() && isHybridSign(tokens.first())) {
        tokens.removeFirst();
        out.isHybrid = true;
        if (out.qualifier == Qualifier::None)
            out.qualifier = Qualifier::Hybrid;
        if (!tokens.isEmpty())
            out.hybridEpithet2 = tokens.takeFirst().toLower();
    }

    // Infraspecific rank + epithet.
    if (!tokens.isEmpty()) {
        const QString maybeRank = normaliseInfraRank(tokens.first());
        if (!maybeRank.isEmpty()) {
            tokens.removeFirst();
            out.infraRank = maybeRank;
            if (!tokens.isEmpty())
                out.infraEpithet = tokens.takeFirst().toLower();
        } else if (tokens.size() == 1 && tokens.first().front().isLower()) {
            // A trailing lowercase word with no rank marker — treat as an
            // (unranked) infraspecific epithet rather than dropping it.
            out.infraRank = QStringLiteral("subsp");
            out.infraEpithet = tokens.takeFirst().toLower();
        }
    }

    if (!parenTag.isEmpty() && out.informalTag.isEmpty())
        out.informalTag = parenTag;

    return out;
}

QString ParsedName::foldedBinomial() const
{
    if (genus.isEmpty() || specificEpithet.isEmpty())
        return {};
    return (genus + QLatin1Char(' ') + specificEpithet).toLower();
}

QString ParsedName::canonical() const
{
    if (genus.isEmpty())
        return raw;

    if (qualifier == Qualifier::Unidentified)
        return QStringLiteral("Unidentified %1").arg(genus);

    if (specificEpithet.isEmpty()) {
        QString s = genus + QStringLiteral(" sp.");
        if (!informalTag.isEmpty())
            s += QStringLiteral(" ") + informalTag;
        return s;
    }

    QString s = genus + QLatin1Char(' ') + specificEpithet;
    if (isHybrid && !hybridEpithet2.isEmpty())
        s += QString::fromUtf8(" \xC3\x97 ") + hybridEpithet2;
    if (!infraRank.isEmpty() && !infraEpithet.isEmpty())
        s += QStringLiteral(" %1. %2").arg(infraRank, infraEpithet);

    switch (qualifier) {
    case Qualifier::SensuLato:   s += QStringLiteral(" s.l."); break;
    case Qualifier::SensuStricto: s += QStringLiteral(" s.s."); break;
    case Qualifier::Aggregate:   s += QStringLiteral(" agg."); break;
    case Qualifier::SpAff:       s = QStringLiteral("%1 aff. %2").arg(genus, specificEpithet); break;
    default: break;
    }
    return s;
}

bool looksNonTaxonomic(const QString &raw)
{
    const QString s = raw.trimmed();
    if (s.isEmpty())
        return true;

    static const QRegularExpression localityRe(
        QStringLiteral(R"(\b(road|rd|street|track|lane|hwy|highway|creek|gully|reserve|np|nsp|brnp|sf|cp|farm|paddock|swamp|wetland|cemetery|quarry|bridge|crossing|carpark|lookout|campground)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression stagingRe(
        QStringLiteral(R"(\b(to sort|to upload|staging|unsorted|new folder|duplicates?|rejects?|for id|to id)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression leadingNumberAddress(QStringLiteral(R"(^\d+\s+\S)"));

    if (s.contains(stagingRe))
        return true;
    if (s.contains(leadingNumberAddress))
        return true;
    if (s.contains(localityRe) && s.contains(QLatin1Char(',')))
        return true;
    return false;
}

} // namespace pl::match
