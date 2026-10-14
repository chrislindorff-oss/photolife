#pragma once

#include "libsync/LibrarySync.h"
#include "sync/SyncService.h"

#include <QDateTime>
#include <QWidget>

#include <optional>

class QMenu;
class QToolButton;

namespace pl {

// Everything the window knows about the shared catalogue right now: fed by
// MainWindow from sync results, shown by the ServerBar, the Server Status
// window and the window title.
struct ServerView
{
    bool running = false;             // a server operation is under way
    QString runningPhase;
    bool editing = false;             // this computer holds the edit lock
    bool offline = false;             // the last contact with the server failed
    bool checked = false;             // the server has answered at least once
    QString lastError;
    std::optional<sync::ServerState> state;
    qint64 localVersion = 0;
    std::optional<sync::ChangeSummary> changes;
    std::optional<sync::SyncService::ConnectionTest> lastTest;
    sync::SyncIdentity me;
    QString server;                   // "photolifedb on host"
    QString libraryRoot;
    QDateTime lastContact;
    QDateTime lastSync;
    int backups = 0;

    // The photos, when PhotoLife keeps them online itself
    // (libsync::PhotoSyncService); independent of the catalogue's sync.
    bool photosEnabled = false;
    bool photosRunning = false;
    QString photosPhase;
    std::optional<libsync::LibraryPlan> photosPlan;   // from the last check or sync
    int photosFailed = 0;                             // files the last sync couldn't move
    bool photosStopped = false;                       // the last sync was stopped part-way
    int photosOnlineOnly = 0;                         // online, not on this computer
    QString photosKept;                               // "every photo", "3 trees' photos"
    QString photosError;                              // the last photo sync failed outright
    QDateTime photosLastSync;

    bool lockedByOther() const { return state && state->isLocked() && !state->heldBy(me); }
    bool newerAvailable() const { return state && checked && state->version != localVersion; }
    int pending() const { return changes ? changes->totalRows : 0; }
};

// The shared-catalogue control strip in the menu bar's top-right corner: a
// coloured badge saying what mode this computer is in (click it for the
// Server Status window), the buttons that make sense in that mode, and a
// menu button with every server command.
class ServerBar : public QWidget
{
    Q_OBJECT

public:
    ServerBar(QMenu *serverMenu, QWidget *parent = nullptr);

    void setView(const ServerView &view);

    // The badge's wording and colour for a view -- shared with the title and
    // the Server Status window.
    static QString modeText(const ServerView &view);
    // One line on the photos' sync ("Photos: 12 to download"), or empty when
    // PhotoLife doesn't sync them.
    static QString photosText(const ServerView &view);
    static QColor modeColor(const ServerView &view);

signals:
    void statusRequested();
    void startEditingRequested();
    void getLatestRequested();
    void syncRequested();
    void finishEditingRequested();
    void reviewChangesRequested();
    void takeOverRequested();
    void retryRequested();

private:
    QToolButton *button(const QString &text, const QString &tip);

    QToolButton *m_badge;
    QToolButton *m_startEditing;
    QToolButton *m_getLatest;
    QToolButton *m_takeOver;
    QToolButton *m_changes;
    QToolButton *m_sync;
    QToolButton *m_finish;
    QToolButton *m_retry;
    QToolButton *m_menuButton;
};

} // namespace pl
