#pragma once

#include "inat/DuplicateFinder.h"
#include "inat/ObservationGrouper.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;

namespace pl {

class Application;
class ImageViewer;
class InatAccountBar;
namespace inat {
class InatDuplicateCheck;
class ObservationUploader;
}

// Upload Observations: library photos go to the user's iNaturalist account
// as new observations. Photos come in from Add Photos (a search of the
// catalogue, or a folder on the drive, added to the library first) or the
// Library's "Upload to iNaturalist…"; photos of a species taken close
// together are proposed as one observation, which the user can merge,
// split, reorder, or give a species, location or date before sending.
// Before sending, each is looked for among the user's own observations on
// iNaturalist, and one that looks the same is held for the user to review.
// Sent observations and photos are recorded on their captures, so they're
// never sent again -- and Download My Observations knows them.
class InatUploadPage : public QWidget
{
    Q_OBJECT

public:
    InatUploadPage(Application &app, InatAccountBar &account, QWidget *parent = nullptr);

    // MainWindow's parts: whether the catalogue may be changed now (a
    // server catalogue's working copy may need to be made editable first),
    // and Set Location on Map for these captures (true when it set one).
    void setEditGuard(std::function<bool()> canEdit) { m_canEdit = std::move(canEdit); }
    void setLocationSetter(std::function<bool(const QList<int> &)> setLocation)
    {
        m_setLocation = std::move(setLocation);
    }
    // ...and Paste Location (true when it set one)...
    void setLocationPaster(std::function<bool(const QList<int> &)> pasteLocation)
    {
        m_pasteLocation = std::move(pasteLocation);
    }
    // ...and Add Folder to Library for a folder chosen in Add Photos, its
    // new photos matched too: `scanned` runs once that's done.
    using FolderAdder = std::function<void(const QString &folder, std::function<void()> scanned)>;
    void setFolderAdder(FolderAdder addFolder) { m_addFolder = std::move(addFolder); }

    // Adds these photos (those not here already), proposing observations for them.
    void addCaptures(const QList<int> &captureIds);
    bool isUploading() const;

signals:
    // Captures now carry iNaturalist ids: the grids may want to reload.
    void uploaded();

private:
    struct Row
    {
        inat::ProposedObservation obs;
        bool use = true;
        QString status;   // while sending, or why it failed
        bool failed = false;
        // Observations already on iNaturalist that look like this one --
        // it's held until the user says what it is.
        QList<inat::DuplicateHit> hits;
        bool asNew = false;   // the user said it's a different sighting
        bool held() const { return !hits.isEmpty() && !asNew; }
    };

    void rebuild();
    void refreshRow(int row);
    QString statusOf(const Row &row) const;
    QList<int> selectedRows() const;
    // The photo selected, as (row, photo); (-1, -1) when it isn't one photo.
    std::pair<int, int> selectedPhoto() const;
    void updateButtons();
    void showWho();
    bool isBusy() const;   // uploading or checking
    // Row `r`'s photos or species changed: whatever was found for it is stale.
    void changed(int r);

    void addPhotos();
    void setSpecies(int row, const taxonomy::Taxon &taxon, bool fromVision);
    void chooseSpecies();
    void identify();
    void setLocation();
    void setDate();
    void merge();
    void split();
    void removeSelected();
    void regroup();
    void openOnInat();
    // Photo `from` of row `fromRow` to position `to` of row `toRow`.
    void movePhoto(int fromRow, int from, int toRow, int to);
    void moveSelected(int by);
    void reviewMatch();
    void editNote(int r);
    // The full-screen viewer, over row `r`'s photos, at photo `index`.
    void viewPhoto(int r, int index);
    void contextMenuAt(const QPoint &pos);
    QList<int> selectedCaptures() const;
    // These photos' locations, read back after one was set; their rows changed.
    void reloadLocations(const QList<int> &captureIds);
    double maxKm() const;   // the distance option, 0 when off

    // Upload: check the token, then look for the ticked observations on
    // iNaturalist (those not looked for since they changed), then confirm
    // and send.
    void upload();
    void checkNow();
    void checkInat(bool thenUpload);
    void checkFinished(bool ok, const QString &error, int status, const QHash<int, QList<inat::DuplicateHit>> &hits);
    void sendReady();
    void uploadFinished(bool tokenExpired);

    Application &m_app;
    InatAccountBar &m_account;
    std::function<bool()> m_canEdit;
    std::function<bool(const QList<int> &)> m_setLocation;
    std::function<bool(const QList<int> &)> m_pasteLocation;
    FolderAdder m_addFolder;
    inat::ObservationUploader *m_uploader = nullptr;
    inat::InatDuplicateCheck *m_check = nullptr;
    bool m_uploadAfterCheck = false;
    bool m_checkStopped = false;   // by Stop, not a failure
    QList<int> m_checking;   // rows being looked for

    QList<Row> m_rows;
    QSet<int> m_fromVision;     // captures whose species was picked from iNaturalist's suggestions
    QList<int> m_sentThisRun;   // rows sent by the last Upload, in order
    QString m_login;            // the token's account, once known

    QLabel *m_who = nullptr;
    QSpinBox *m_gap = nullptr;
    QCheckBox *m_byDistance = nullptr;
    QSpinBox *m_distance = nullptr;
    QSlider *m_photoSize = nullptr;
    ImageViewer *m_viewer = nullptr;
    QComboBox *m_geoprivacy = nullptr;
    QStackedWidget *m_stack = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_species = nullptr;
    QPushButton *m_identify = nullptr;
    QPushButton *m_location = nullptr;
    QPushButton *m_date = nullptr;
    QPushButton *m_merge = nullptr;
    QPushButton *m_split = nullptr;
    QPushButton *m_up = nullptr;
    QPushButton *m_down = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_regroup = nullptr;
    QPushButton *m_checkButton = nullptr;
    QPushButton *m_review = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_upload = nullptr;
    QPushButton *m_stop = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_message = nullptr;
};

} // namespace pl
