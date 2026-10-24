#include "scan/CaptureRenamer.h"

#include "scan/FilenameParser.h"
#include "util/PathSanitize.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace pl::scan {
namespace {

constexpr int kMaxStem = 200;

// Editing programs' sidecars, kept beside the photo under its name.
bool isSidecar(const QString &fileName)
{
    static const QSet<QString> kinds = {QStringLiteral("xmp"), QStringLiteral("dop"), QStringLiteral("pp3"),
                                        QStringLiteral("on1"), QStringLiteral("acr")};
    return kinds.contains(QFileInfo(fileName).suffix().toLower());
}

QString cleaned(const QString &text)
{
    QString out = util::sanitizeFilenameComponent(text.simplified()).simplified();
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);
    return out;
}

} // namespace

CaptureRenamer::CaptureRenamer(QString connectionName) : m_connectionName(std::move(connectionName)) {}

QString CaptureRenamer::renamedStem(const QString &baseName, const QString &nameText, const QString &name)
{
    // What follows the name: " (bud) - Locality 14-6-2020 (1)".
    QString rest;
    if (!nameText.isEmpty() && baseName.startsWith(nameText, Qt::CaseInsensitive))
        rest = baseName.mid(nameText.size());
    else if (const int dash = baseName.indexOf(QStringLiteral(" - ")); dash >= 0)
        rest = baseName.mid(dash);
    QString stem = cleaned(cleaned(name) + rest);
    stem.truncate(kMaxStem);
    return cleaned(stem);
}

CaptureRenamer::Result CaptureRenamer::rename(qint64 captureId, const QString &name)
{
    Result result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    QSqlQuery cap(db);
    cap.prepare(QStringLiteral("SELECT c.base_name, c.name_text, c.folder_id, f.path FROM capture c "
                               "JOIN folder f ON f.id = c.folder_id WHERE c.id = ?"));
    cap.addBindValue(captureId);
    if (!cap.exec() || !cap.next()) {
        result.error = QStringLiteral("photo not found");
        return result;
    }
    const QString base = cap.value(0).toString();
    const QString nameText = cap.value(1).toString();
    const int folderId = cap.value(2).toInt();

    struct File
    {
        qint64 renditionId = 0;   // 0: a sidecar
        QString from;
        QString to;
    };
    QList<File> files;
    QSqlQuery rend(db);
    rend.prepare(QStringLiteral("SELECT id, path FROM rendition WHERE capture_id = ? ORDER BY id"));
    rend.addBindValue(captureId);
    if (!rend.exec()) {
        result.error = rend.lastError().text();
        return result;
    }
    while (rend.next())
        files.append({rend.value(0).toLongLong(), rend.value(1).toString(), {}});
    if (files.isEmpty()) {
        result.error = QStringLiteral("no files");
        return result;
    }
    const QDir dir = QFileInfo(files.first().from).absoluteDir();
    for (const File &f : std::as_const(files)) {
        if (!QFileInfo::exists(f.from) || QFileInfo(f.from).absoluteDir() != dir) {
            result.error = QStringLiteral("not all of its files are on this computer");
            return result;
        }
    }
    // Sidecars: "<base>.xmp", "<base>.NEF.xmp"...
    const QStringList entries = dir.entryList(QDir::Files | QDir::Hidden);
    QSet<QString> known;
    for (const File &f : std::as_const(files))
        known.insert(QFileInfo(f.from).fileName());
    for (const QString &entry : entries) {
        if (!known.contains(entry) && entry.startsWith(base + QLatin1Char('.')) && isSidecar(entry))
            files.append({0, dir.filePath(entry), {}});
    }

    // A free name: no file of that stem in the folder (any extension, any
    // case) and no other photo of that name in the catalogue.
    QString stem = renamedStem(base, nameText, name);
    if (stem.isEmpty() || stem == base) {
        result.error = stem.isEmpty() ? QStringLiteral("no name") : QString();
        result.newBaseName = base;
        return result;
    }
    const auto taken = [&](const QString &candidate) {
        if (candidate.compare(base, Qt::CaseInsensitive) == 0)
            return false;   // its own (a change of case)
        const QString prefix = candidate + QLatin1Char('.');
        for (const QString &entry : entries) {
            if (entry.compare(candidate, Qt::CaseInsensitive) == 0 || entry.startsWith(prefix, Qt::CaseInsensitive))
                return true;
        }
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT 1 FROM capture WHERE folder_id = ? AND base_name = ? AND id <> ?"));
        q.addBindValue(folderId);
        q.addBindValue(candidate);
        q.addBindValue(captureId);
        return !q.exec() || q.next();
    };
    if (taken(stem)) {
        // The next free "(n)": counting on from a "(n)" the name already has.
        static const QRegularExpression counter(QStringLiteral("^(.*) \\((\\d+)\\)$"));
        const QRegularExpressionMatch m = counter.match(stem);
        const QString root = m.hasMatch() ? m.captured(1) : stem;
        int n = m.hasMatch() ? m.captured(2).toInt() + 1 : 2;
        while (taken(root + QStringLiteral(" (%1)").arg(n)))
            ++n;
        stem = root + QStringLiteral(" (%1)").arg(n);
    }

    for (File &f : files)
        f.to = dir.filePath(stem + QFileInfo(f.from).fileName().mid(base.size()));

    // The files, putting back any already renamed if one fails.
    QList<File> done;
    for (const File &f : std::as_const(files)) {
        if (QFileInfo::exists(f.to) && f.to.compare(f.from, Qt::CaseInsensitive) != 0) {
            result.error = QStringLiteral("%1 already exists").arg(QFileInfo(f.to).fileName());
        } else if (!QFile::rename(f.from, f.to)) {
            result.error = QStringLiteral("couldn't rename %1").arg(QFileInfo(f.from).fileName());
        } else {
            done << f;
            continue;
        }
        for (const File &back : std::as_const(done))
            QFile::rename(back.to, back.from);
        return result;
    }

    // Then the catalogue: the photo's name, as a scan would read it, and its files.
    const ParsedFilename parsed = parseStem(stem);
    bool ok = db.transaction();
    QSqlQuery upd(db);
    upd.prepare(QStringLiteral(
        "UPDATE capture SET base_name = ?, name_text = ?, locality_text = ?, organ_tags = ? WHERE id = ?"));
    upd.addBindValue(stem);
    upd.addBindValue(parsed.name);
    upd.addBindValue(parsed.locality.isEmpty() ? QVariant() : parsed.locality);
    upd.addBindValue(parsed.organTags.isEmpty() ? QVariant() : parsed.organTags.join(QLatin1Char(',')));
    upd.addBindValue(captureId);
    ok = ok && upd.exec();
    for (const File &f : std::as_const(files)) {
        if (!ok || f.renditionId == 0)
            continue;
        QSqlQuery r(db);
        r.prepare(QStringLiteral("UPDATE rendition SET path = ? WHERE id = ?"));
        r.addBindValue(f.to);
        r.addBindValue(f.renditionId);
        ok = r.exec();
        if (!ok)
            result.error = r.lastError().text();
    }
    if (!ok || !db.commit()) {
        if (result.error.isEmpty())
            result.error = upd.lastError().text();
        db.rollback();
        for (const File &back : std::as_const(done))
            QFile::rename(back.to, back.from);
        return result;
    }
    result.renamed = true;
    result.newBaseName = stem;
    for (const File &f : std::as_const(files))
        result.files << f.to;
    return result;
}

} // namespace pl::scan
