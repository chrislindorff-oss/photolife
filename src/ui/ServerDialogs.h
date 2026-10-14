#pragma once

#include "sync/ChangeSummary.h"
#include "ui/ServerBar.h"

#include <QDialog>

#include <optional>

class QCheckBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTreeWidget;

namespace pl {

// "Server Status": everything about the shared catalogue at a glance --
// connection, who you are and what mode you're in, the edit lock, versions,
// your unsent changes, the photo library folder and backups. Non-modal;
// MainWindow keeps it up to date with setView().
class ServerStatusDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ServerStatusDialog(QWidget *parent = nullptr);
    void setView(const ServerView &view);

signals:
    void startEditingRequested();
    void getLatestRequested();
    void syncRequested();
    void finishEditingRequested();
    void reviewChangesRequested();
    void testConnectionRequested();
    void activityLogRequested();
    void restoreBackupRequested();
    void syncPhotosRequested();

private:
    QLabel *m_mode;
    QLabel *m_server;
    QLabel *m_connection;
    QLabel *m_you;
    QLabel *m_lock;
    QLabel *m_versions;
    QLabel *m_changes;
    QLabel *m_library;
    QLabel *m_photos;
    QPushButton *m_syncPhotos;
    QLabel *m_backups;
    QPushButton *m_startEditing;
    QPushButton *m_getLatest;
    QPushButton *m_sync;
    QPushButton *m_finish;
};

// "Review Changes": what this computer has changed since the last sync, by
// kind, each expandable to the individual items. Automatic updates (cover
// photos, localities ...) are grouped separately.
class ReviewChangesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ReviewChangesDialog(QWidget *parent = nullptr);
    void setSummary(const std::optional<sync::ChangeSummary> &summary, bool editing,
                    const QString &error = {});

signals:
    void refreshRequested();
    void syncRequested();
    void discardRequested();

private:
    QLabel *m_headline;
    QTreeWidget *m_tree;
    QPushButton *m_sync;
    QPushButton *m_discard;
};

// "Activity Log": every server action on this computer, newest first.
class ActivityLogDialog : public QDialog
{
    Q_OBJECT

public:
    ActivityLogDialog(QString logPath, QWidget *parent = nullptr);
    void reload();

private:
    QString m_path;
    QTableWidget *m_table;
    QCheckBox *m_errorsOnly;
};

// A time for these windows: "3:10 pm" today, else date and time.
QString describeTime(const QDateTime &time);

} // namespace pl
