#pragma once

#include <QDialog>

class QTableWidget;

namespace pl {
class Application;
}

namespace pl {

// Lists every folder the user has added to the library (Settings::
// watchedRoots()), flags any that no longer exist on disk, and lets the user
// remove one. Removing a folder that still exists only stops watching it
// (its catalogued photos are untouched). Removing a missing folder offers a
// choice: Purge (delete its catalogued folder/capture/rendition rows too) or
// Retain (stop watching, keep the records in case the drive/folder comes
// back).
class LibraryFoldersDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LibraryFoldersDialog(Application &app, QWidget *parent = nullptr);

signals:
    void librariesChanged();

private:
    void reload();
    void removeSelected();

    Application &m_app;
    QTableWidget *m_table;
    bool m_dirty = false;
};

} // namespace pl
