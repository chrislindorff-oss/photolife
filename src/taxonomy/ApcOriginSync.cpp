#include "taxonomy/ApcOriginSync.h"

#include "taxonomy/TaxonomyStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QtConcurrent>

#include <memory>

namespace pl::taxonomy {

const QUrl ApcOriginSync::kExportUrl(QStringLiteral(
    "https://biodiversity.org.au/nsl/services/export/taxonCsv"));

namespace {

const QString kSource = QStringLiteral("APC");

// Splits one CSV record (possibly spanning lines) into fields, honouring
// double-quoted fields with "" escapes.
QStringList splitRecord(const QString &record)
{
    QStringList fields;
    QString field;
    bool inQuotes = false;
    for (qsizetype i = 0; i < record.size(); ++i) {
        const QChar c = record.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < record.size() && record.at(i + 1) == QLatin1Char('"')) {
                    field += QLatin1Char('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
        } else if (c == QLatin1Char('"')) {
            inQuotes = true;
        } else if (c == QLatin1Char(',')) {
            fields << field;
            field.clear();
        } else {
            field += c;
        }
    }
    fields << field;
    return fields;
}

// The next whole record: lines are joined while a quoted field is still open.
std::optional<QString> readRecord(QIODevice &device)
{
    if (device.atEnd())
        return std::nullopt;
    QString record;
    qsizetype quotes = 0;
    do {
        QString line = QString::fromUtf8(device.readLine());
        quotes += line.count(QLatin1Char('"'));
        record += line;
    } while (quotes % 2 != 0 && !device.atEnd());
    while (record.endsWith(QLatin1Char('\n')) || record.endsWith(QLatin1Char('\r')))
        record.chop(1);
    return record;
}

} // namespace

ApcOriginSync::ApcOriginSync(TaxonomyStore &store, QString cacheDir, QByteArray userAgent,
                             QObject *parent)
    : QObject(parent), m_store(store), m_cacheDir(std::move(cacheDir)),
      m_userAgent(std::move(userAgent)), m_url(kExportUrl),
      m_nam(new QNetworkAccessManager(this))
{
}

QString ApcOriginSync::csvPath() const
{
    return QDir(m_cacheDir).filePath(QStringLiteral("apc-taxon.csv"));
}

std::optional<qint64> ApcOriginSync::placeForState(const QString &code)
{
    // iNaturalist places for the APC's state and territory codes (the same
    // ones AlaStatusSync::placeForRegion maps by name).
    static const QHash<QString, qint64> places = {
        {QStringLiteral("ACT"), 12986}, {QStringLiteral("NSW"), 6825},
        {QStringLiteral("NT"), 9994},   {QStringLiteral("Qld"), 7308},
        {QStringLiteral("SA"), 6899},   {QStringLiteral("Tas"), 6829},
        {QStringLiteral("Vic"), 7830},  {QStringLiteral("WA"), 6827},
    };
    const auto it = places.constFind(code);
    if (it == places.constEnd())
        return std::nullopt;
    return *it;
}

QHash<QString, QString> ApcOriginSync::parseDistribution(const QString &distribution)
{
    static const QRegularExpression part(QStringLiteral("^\\s*([A-Za-z]+)\\s*(?:\\((.*)\\))?\\s*$"));
    QHash<QString, QString> out;
    for (const QString &raw : distribution.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QRegularExpressionMatch m = part.match(raw);
        if (!m.hasMatch() || !placeForState(m.captured(1)))
            continue;   // offshore islands (LHI, NI, ...) and anything unparseable
        const QString q = m.captured(2).trimmed().toLower();
        QString origin;
        if (q.isEmpty() || q.contains(QLatin1String("native"))
            || q == QLatin1String("presumed extinct"))
            origin = QStringLiteral("native");   // incl. "native and naturalised"
        else if (q.contains(QLatin1String("naturalised")))
            origin = QStringLiteral("introduced");   // naturalised, doubtfully, formerly
        else
            continue;   // "uncertain origin" and the like: unknown
        out.insert(m.captured(1), origin);
    }
    return out;
}

QHash<QString, QString> ApcOriginSync::readTaxonCsv(QIODevice &device)
{
    QHash<QString, QString> byName;
    const auto header = readRecord(device);
    if (!header)
        return byName;
    const QStringList cols = splitRecord(*header);
    const int idCol = int(cols.indexOf(QStringLiteral("taxonID")));
    const int acceptedCol = int(cols.indexOf(QStringLiteral("acceptedNameUsageID")));
    const int statusCol = int(cols.indexOf(QStringLiteral("taxonomicStatus")));
    const int nameCol = int(cols.indexOf(QStringLiteral("canonicalName")));
    const int distCol = int(cols.indexOf(QStringLiteral("taxonDistribution")));
    if (idCol < 0 || acceptedCol < 0 || statusCol < 0 || nameCol < 0 || distCol < 0)
        return byName;

    static const QSet<QString> synonymStatuses = {
        QStringLiteral("taxonomic synonym"), QStringLiteral("nomenclatural synonym"),
        QStringLiteral("orthographic variant")};
    QHash<QString, QString> distributionById;
    QSet<QString> acceptedNames;
    QList<std::pair<QString, QString>> synonyms;   // (folded name, accepted id)
    while (const auto record = readRecord(device)) {
        if (record->isEmpty())
            continue;
        const QStringList f = splitRecord(*record);
        const QString status = f.value(statusCol);
        const QString name = TaxonomyStore::foldListName(f.value(nameCol));
        if (name.isEmpty())
            continue;
        if (status == QLatin1String("accepted")) {
            const QString dist = f.value(distCol);
            distributionById.insert(f.value(idCol), dist);
            byName.insert(name, dist);
            acceptedNames.insert(name);
        } else if (synonymStatuses.contains(status)) {
            synonyms.append({name, f.value(acceptedCol)});
        }
    }
    // A synonym carries its accepted taxon's distribution -- unless the same
    // spelling is itself an accepted name.
    for (const auto &[name, acceptedId] : std::as_const(synonyms)) {
        if (acceptedNames.contains(name))
            continue;
        const auto it = distributionById.constFind(acceptedId);
        if (it != distributionById.constEnd())
            byName.insert(name, *it);
    }
    return byName;
}

void ApcOriginSync::start(bool forceDownload)
{
    if (m_running)
        return;
    m_running = true;
    const QFileInfo info(csvPath());
    const bool stale = !info.exists() || info.size() == 0
                       || info.lastModified().daysTo(QDateTime::currentDateTime()) > kMaxAgeDays;
    if (forceDownload || stale)
        download();
    else
        parseAndStore();
}

void ApcOriginSync::download()
{
    QDir().mkpath(m_cacheDir);
    auto file = std::make_shared<QSaveFile>(csvPath());
    if (!file->open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("could not write %1").arg(csvPath()));
        return;
    }
    emit progress(tr("Downloading the Australian Plant Census…"));

    QNetworkRequest req(m_url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(120000);   // no data for 2 minutes: give up
    if (!m_userAgent.isEmpty())
        req.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::readyRead, this,
            [reply, file] { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file] {
        reply->deleteLater();
        file->write(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || (status != 0 && status != 200)) {
            file->cancelWriting();
            fail(reply->errorString());
            return;
        }
        if (!file->commit()) {
            fail(QStringLiteral("could not save %1").arg(csvPath()));
            return;
        }
        parseAndStore();
    });
}

void ApcOriginSync::parseAndStore()
{
    emit progress(tr("Reading the Australian Plant Census…"));
    auto *watcher = new QFutureWatcher<QHash<QString, QString>>(this);
    connect(watcher, &QFutureWatcher<QHash<QString, QString>>::finished, this, [this, watcher] {
        watcher->deleteLater();
        const QHash<QString, QString> byName = watcher->result();
        if (byName.isEmpty()) {
            fail(QStringLiteral("the Australian Plant Census file had no usable rows"));
            return;
        }
        const QHash<QString, qint64> taxa = m_store.leafTaxaByFoldedName();
        QList<TaxonomyStore::SourcedFact> rows;
        QSet<qint64> matched;
        for (auto it = taxa.constBegin(); it != taxa.constEnd(); ++it) {
            const auto dist = byName.constFind(it.key());
            if (dist == byName.constEnd())
                continue;
            const QHash<QString, QString> origins = parseDistribution(*dist);
            for (auto o = origins.constBegin(); o != origins.constEnd(); ++o) {
                rows.append({it.value(), placeForState(o.key()), o.value()});
                matched.insert(it.value());
            }
        }
        if (m_store.replaceOriginsFromSource(kSource, rows) < 0) {
            fail(QStringLiteral("could not save the Australian Plant Census origins"));
            return;
        }
        m_running = false;
        emit finished(true, QString(), int(matched.size()));
    });
    const QString path = csvPath();
    watcher->setFuture(QtConcurrent::run([path] {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return QHash<QString, QString>();
        return readTaxonCsv(f);
    }));
}

void ApcOriginSync::fail(const QString &error)
{
    m_running = false;
    emit finished(false, error, 0);
}

} // namespace pl::taxonomy
