#include "organise/OrganiseMover.h"

#include <QDir>
#include <QHash>
#include <QSet>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace pl::organise {

namespace {

struct Move
{
    QString from;    // absolute
    QString to;
    QString shot;    // base name: groups a shot's files
};

// One organise.
struct Batch
{
    QDateTime when;
    QStringList createdFolders;   // relative
    QList<Move> moves;
};

QJsonObject toJson(const Batch &b)
{
    QJsonArray folders;
    for (const QString &f : b.createdFolders)
        folders.append(f);
    QJsonArray moved;
    for (const Move &m : b.moves)
        moved.append(QJsonObject{{QStringLiteral("from"), m.from},
                                 {QStringLiteral("to"), m.to},
                                 {QStringLiteral("shot"), m.shot}});
    return {{QStringLiteral("when"), b.when.toString(Qt::ISODate)},
            {QStringLiteral("createdFolders"), folders},
            {QStringLiteral("moves"), moved}};
}

Batch fromJson(const QJsonObject &o)
{
    Batch b;
    b.when = QDateTime::fromString(o.value(QStringLiteral("when")).toString(), Qt::ISODate);
    for (const QJsonValue &v : o.value(QStringLiteral("createdFolders")).toArray())
        b.createdFolders << v.toString();
    for (const QJsonValue &v : o.value(QStringLiteral("moves")).toArray()) {
        const QJsonObject m = v.toObject();
        b.moves.append({m.value(QStringLiteral("from")).toString(), m.value(QStringLiteral("to")).toString(),
                        m.value(QStringLiteral("shot")).toString()});
    }
    return b;
}

// Every organise recorded in the folder, oldest first. (A version 1 log
// held just one, with no date: its file's time stands in.)
QList<Batch> readLog(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QList<Batch> batches;
    if (root.contains(QStringLiteral("batches"))) {
        for (const QJsonValue &v : root.value(QStringLiteral("batches")).toArray())
            batches << fromJson(v.toObject());
    } else if (root.contains(QStringLiteral("moves"))) {
        Batch b = fromJson(root);
        b.when = QFileInfo(path).lastModified();
        batches << b;
    }
    batches.removeIf([](const Batch &b) { return b.moves.isEmpty() && b.createdFolders.isEmpty(); });
    return batches;
}

bool writeLog(const QString &path, const QList<Batch> &batches)
{
    if (batches.isEmpty())
        return QFile::remove(path) || !QFileInfo::exists(path);
    QJsonArray all;
    for (const Batch &b : batches)
        all.append(toJson(b));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 2},
                                         {QStringLiteral("batches"), all}})
                   .toJson());
    return file.commit();
}

QString folderOf(const QDir &root, const QString &to)
{
    return root.relativeFilePath(QFileInfo(to).absolutePath());
}

// The shot's files' names in the destination: as they are, or with " (n)"
// after the base name when any of them is taken.
QStringList targetNames(const ShotMove &shot, const QDir &dest)
{
    for (int n = 1;; ++n) {
        QStringList names;
        bool free = true;
        for (const QString &file : shot.files) {
            QString name = QFileInfo(file).fileName();
            if (n > 1)
                name = shot.baseName + QStringLiteral(" (%1)").arg(n) + name.mid(shot.baseName.size());
            if (QFileInfo::exists(dest.filePath(name)))
                free = false;
            names << name;
        }
        if (free)
            return names;
    }
}

} // namespace

OrganiseMover::Result OrganiseMover::execute(const QString &folder, const QList<ShotMove> &shots)
{
    Result result;
    const QDir root(folder);
    const QString logPath = root.filePath(QLatin1String(kLogFile));
    QList<Batch> batches = readLog(logPath);
    batches.append(Batch{QDateTime::currentDateTime(), {}, {}});
    Batch &log = batches.last();
    for (const ShotMove &shot : shots) {
        if (!QFileInfo(root.filePath(shot.folderName)).isDir()) {
            if (!root.mkpath(shot.folderName)) {
                for (const QString &f : shot.files)
                    result.failures << QStringLiteral("%1: couldn't create the folder %2")
                                           .arg(QFileInfo(f).fileName(), shot.folderName);
                continue;
            }
            log.createdFolders << shot.folderName;
        }
        const QDir dest(root.filePath(shot.folderName));
        const QStringList names = targetNames(shot, dest);
        bool any = false;
        for (int i = 0; i < shot.files.size(); ++i) {
            const QString from = shot.files.at(i);
            const QString to = dest.filePath(names.at(i));
            if (!QFile::rename(from, to)) {
                result.failures << QStringLiteral("%1: couldn't move it").arg(QFileInfo(from).fileName());
                continue;
            }
            log.moves.append({from, to, shot.baseName});
            any = true;
        }
        if (any)
            ++result.moved;
        writeLog(logPath, batches);   // after each shot: an interrupted run is still undoable
    }
    if (log.moves.isEmpty() && log.createdFolders.isEmpty()) {
        batches.removeLast();
        writeLog(logPath, batches);
    }
    return result;
}

bool OrganiseMover::canUndo(const QString &folder)
{
    return !readLog(QDir(folder).filePath(QLatin1String(kLogFile))).isEmpty();
}

OrganiseMover::UndoPreview OrganiseMover::previewUndo(const QString &folder)
{
    UndoPreview preview;
    const QDir root(folder);
    const QList<Batch> batches = readLog(root.filePath(QLatin1String(kLogFile)));
    if (batches.isEmpty())
        return preview;
    const Batch &last = batches.last();
    preview.when = last.when;
    preview.earlier = int(batches.size()) - 1;
    QHash<QString, int> index;   // folder -> preview.folders
    QSet<QString> shots;
    for (const Move &m : last.moves) {
        const QString name = folderOf(root, m.to);
        if (!index.contains(name)) {
            index.insert(name, int(preview.folders.size()));
            preview.folders.append({name, 0, 0});
        }
        UndoPreview::Folder &f = preview.folders[index.value(name)];
        ++f.files;
        ++preview.files;
        const QString key = name + QLatin1Char('/') + (m.shot.isEmpty() ? m.from : m.shot);
        if (!shots.contains(key)) {
            shots.insert(key);
            ++f.shots;
            ++preview.shots;
        }
        if (!QFileInfo::exists(m.to))
            ++preview.missing;
    }
    return preview;
}

OrganiseMover::Result OrganiseMover::undo(const QString &folder)
{
    Result result;
    const QDir root(folder);
    const QString logPath = root.filePath(QLatin1String(kLogFile));
    QList<Batch> batches = readLog(logPath);
    if (batches.isEmpty())
        return result;
    const Batch last = batches.takeLast();
    QSet<QString> shotsBack;
    for (auto it = last.moves.crbegin(); it != last.moves.crend(); ++it) {
        const Move &m = *it;
        if (!QFileInfo::exists(m.to)) {
            result.failures << QStringLiteral("%1: it's no longer where it was put").arg(QFileInfo(m.to).fileName());
            continue;
        }
        if (QFileInfo::exists(m.from)) {
            result.failures << QStringLiteral("%1: another file is in its old place").arg(QFileInfo(m.from).fileName());
            continue;
        }
        if (QFile::rename(m.to, m.from))
            shotsBack.insert(folderOf(root, m.to) + QLatin1Char('/') + (m.shot.isEmpty() ? m.from : m.shot));
        else
            result.failures << QStringLiteral("%1: couldn't move it back").arg(QFileInfo(m.to).fileName());
    }
    result.moved = int(shotsBack.size());
    // Deepest first; rmdir only removes a folder that's empty.
    QStringList folders = last.createdFolders;
    std::sort(folders.begin(), folders.end(), [](const QString &a, const QString &b) {
        return a.count(QLatin1Char('/')) > b.count(QLatin1Char('/'));
    });
    for (const QString &f : folders)
        root.rmdir(f);
    // What couldn't go back stays as it is; the entry goes either way, so
    // the next undo is the organise before.
    writeLog(logPath, batches);
    return result;
}

} // namespace pl::organise
