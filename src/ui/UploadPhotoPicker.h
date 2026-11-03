#pragma once

#include <QDate>
#include <QDialog>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <functional>

class QCheckBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

namespace pl {

class Application;

// Add Photos (Upload Observations): finds library photos by when they were
// taken, their folder and their species, shows them as thumbnails, and
// hands back the ones selected. A folder can be chosen from the drive; when
// it has photos the library doesn't, it offers to add the folder to the
// library (through the folder adder), as only catalogued photos can be
// uploaded. Nothing else is changed here.
class UploadPhotoPicker : public QDialog
{
    Q_OBJECT

public:
    struct Filter
    {
        QDate from, to;        // taken between (inclusive); invalid = no bound
        // A folder's full path (with `subfolders`, those in it too), or part
        // of a folder's path; empty = any.
        QString folder;
        bool subfolders = true;
        QString species;       // part of the matched species' scientific or common name; empty = any
        bool notOnInat = true; // leave out photos already on iNaturalist
        int limit = 2000;
    };
    struct Found
    {
        int captureId = 0;
        QString path;          // its preview file
        QString contentHash;
        QString label;         // file name
        QString species;       // its matched name, if any
        bool onInat = false;
    };
    // Newest first. Exposed for tests.
    static QList<Found> find(const QString &connectionName, const Filter &filter);

    // How many photo files in `folder` (and with `subfolders`, those in
    // it) the catalogue doesn't have. Reads only names. Exposed for tests.
    static int countUncatalogued(const QString &connectionName, const QString &folder, bool subfolders);

    // Adds a folder to the library and matches its new photos; `scanned`
    // runs once that's done.
    using FolderAdder = std::function<void(const QString &folder, std::function<void()> scanned)>;
    void setFolderAdder(FolderAdder addFolder) { m_addFolder = std::move(addFolder); }

    UploadPhotoPicker(Application &app, QWidget *parent = nullptr);

    QList<int> selectedCaptureIds() const;

private:
    void search();
    void updateAdd();
    void browse();
    // The banner for photos in the chosen folder that aren't catalogued.
    void countNew();
    void addFolderToLibrary();
    QString chosenFolder() const;   // the folder field as a full path, or empty
    // Thumbnails are asked for only as photos come into view.
    void fillVisibleIcons();

    Application &m_app;
    QCheckBox *m_useDates = nullptr;
    QDateEdit *m_from = nullptr;
    QDateEdit *m_to = nullptr;
    QLineEdit *m_folder = nullptr;
    QCheckBox *m_subfolders = nullptr;
    QWidget *m_newBanner = nullptr;
    QLabel *m_newText = nullptr;
    QPushButton *m_addFolderButton = nullptr;
    FolderAdder m_addFolder;
    int m_counting = 0;           // which count is the latest
    QSet<int> m_beforeScan;       // photos shown before the folder was added
    QLineEdit *m_species = nullptr;
    QCheckBox *m_notOnInat = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_count = nullptr;
    QPushButton *m_add = nullptr;
    QTimer *m_searchSoon = nullptr;
    QHash<QString, QListWidgetItem *> m_byHash;   // thumbnail key -> its item
};

} // namespace pl
