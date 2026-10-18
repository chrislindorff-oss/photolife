#include "organise/OrganiseMover.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace pl::organise {

namespace {

struct Log
{
    QStringList createdFolders;            // relative
    QList<std::pair<QString, QString>> moves;   // absolute from -> to

    bool write(const QString &path) const
    {
        QJsonArray folders;
        for (const QString &f : createdFolders)
            folders.append(f);
        QJsonArray moved;
        for (const auto &[from, to] : moves)
            moved.append(QJsonObject{{QStringLiteral("from"), from}, {QStringLiteral("to"), to}});
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                             {QStringLiteral("createdFolders"), folders},
                                             {QStringLiteral("moves"), moved}})
                       .toJson());
        return file.commit();
    }

    static Log read(const QString &path)
    {
        Log log;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return log;
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        for (const QJsonValue &v : root.value(QStringLiteral("createdFolders")).toArray())
            log.createdFolders << v.toString();
        for (const QJsonValue &v : root.value(QStringLiteral("moves")).toArray()) {
            const QJsonObject o = v.toObject();
            log.moves.append({o.value(QStringLiteral("from")).toString(),
                              o.value(QStringLiteral("to")).toString()});
        }
        return log;
    }
};

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
    Log log;
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
            log.moves.append({from, to});
            any = true;
        }
        if (any)
            ++result.moved;
        log.write(logPath);   // after each shot: an interrupted run is still undoable
    }
    if (log.moves.isEmpty() && log.createdFolders.isEmpty())
        QFile::remove(logPath);
    return result;
}

bool OrganiseMover::canUndo(const QString &folder)
{
    return QFileInfo::exists(QDir(folder).filePath(QLatin1String(kLogFile)));
}

OrganiseMover::Result OrganiseMover::undo(const QString &folder)
{
    Result result;
    const QDir root(folder);
    const QString logPath = root.filePath(QLatin1String(kLogFile));
    const Log log = Log::read(logPath);
    for (auto it = log.moves.crbegin(); it != log.moves.crend(); ++it) {
        const auto &[from, to] = *it;
        if (!QFileInfo::exists(to)) {
            result.failures << QStringLiteral("%1: it's no longer where it was put").arg(QFileInfo(to).fileName());
            continue;
        }
        if (QFileInfo::exists(from)) {
            result.failures << QStringLiteral("%1: another file is in its old place").arg(QFileInfo(from).fileName());
            continue;
        }
        if (QFile::rename(to, from))
            ++result.moved;
        else
            result.failures << QStringLiteral("%1: couldn't move it back").arg(QFileInfo(to).fileName());
    }
    // Deepest first; rmdir only removes a folder that's empty.
    QStringList folders = log.createdFolders;
    std::sort(folders.begin(), folders.end(), [](const QString &a, const QString &b) {
        return a.count(QLatin1Char('/')) > b.count(QLatin1Char('/'));
    });
    for (const QString &f : folders)
        root.rmdir(f);
    if (result.failures.isEmpty())
        QFile::remove(logPath);
    return result;
}

} // namespace pl::organise
