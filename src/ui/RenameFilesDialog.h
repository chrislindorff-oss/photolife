#pragma once

#include "collection/FileNamePattern.h"
#include "scan/LibraryRenamer.h"

#include <QDialog>
#include <QList>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTreeWidget;

namespace pl {

class Settings;

// Rename Files on Drive: new names for library photos' actual files, either
// a new name part (the rest of each name kept) or the whole name built from
// parts as Export does, with -- when the photos share a folder -- the
// folder renamed too. Shows every photo's name now and after before
// anything is touched; the caller does the renaming, in photos()' order.
class RenameFilesDialog : public QDialog
{
    Q_OBJECT

public:
    RenameFilesDialog(const QString &connectionName, const QList<qint64> &captureIds, Settings &settings,
                      QWidget *parent = nullptr);

    // In the order to rename them, each with its newStem.
    const QList<scan::LibraryRenamer::Photo> &photos() const { return m_photos; }
    // The folder to rename too, and to what: 0 when it isn't.
    int folderId() const;
    QString folderName() const;

private:
    void setUpFolder(const QString &connectionName, const QStringList &roots);
    void chooseParts();
    // The name form picked: fills in the name (or says each photo gets its own).
    void nameFormChosen();
    scan::LibraryRenamer::NameForm nameForm() const;
    void update();

    Settings &m_settings;
    QList<scan::LibraryRenamer::Photo> m_photos;
    collection::FileNamePattern m_pattern;
    int m_folderId = 0;
    QString m_folderNow;
    bool m_folderEdited = false;

    QRadioButton *m_byName = nullptr;
    QComboBox *m_nameForm = nullptr;   // the species' name, in which form, or as typed
    QLineEdit *m_name = nullptr;
    QRadioButton *m_byParts = nullptr;
    QLabel *m_parts = nullptr;
    QPushButton *m_chooseParts = nullptr;
    QCheckBox *m_folder = nullptr;
    QLineEdit *m_folderName = nullptr;
    QLabel *m_folderNote = nullptr;
    QTreeWidget *m_preview = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_ok = nullptr;
};

} // namespace pl
