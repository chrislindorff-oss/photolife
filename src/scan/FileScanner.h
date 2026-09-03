#pragma once

#include "scan/ScanTypes.h"

#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

namespace pl::scan {

// Walks one or more root folders and groups the image files it finds into
// captures. Pure filesystem work: no database, no hashing, no image decoding —
// just enough to know what is there. Safe to run on a worker thread.
class FileScanner
{
public:
    using ProgressFn = std::function<void(const ScanProgress &)>;
    using CancelFn = std::function<bool()>;

    // Extensions (lower-case, no dot) recognised as directly-displayable images
    // and as camera RAW respectively.
    static const QSet<QString> &imageExtensions();
    static const QSet<QString> &rawExtensions();

    // File names and directory names skipped outright (case-insensitive).
    static bool isIgnoredFile(const QString &fileName);
    static bool isIgnoredDir(const QString &dirName);

    void setProgressCallback(ProgressFn fn) { m_progress = std::move(fn); }
    void setCancelPredicate(CancelFn fn) { m_externalCancel = std::move(fn); }

    // Requests the current walk stop as soon as possible. Thread-safe.
    void cancel() { m_cancelled.store(true); }
    bool wasCancelled() const { return isCancelled(); }

    // Walks `roots` recursively. Returns every capture found, in directory order.
    // Non-existent roots are skipped silently.
    QList<DiscoveredCapture> scan(const QStringList &roots);

private:
    void scanDir(const QString &dirPath);
    bool isCancelled() const
    {
        return m_cancelled.load() || (m_externalCancel && m_externalCancel());
    }

    ProgressFn m_progress;
    CancelFn m_externalCancel;
    std::atomic_bool m_cancelled{false};

    ScanProgress m_stats;
    QList<DiscoveredCapture> m_captures;
    QSet<QString> m_visitedDirs;   // guards against symlink cycles
};

} // namespace pl::scan
