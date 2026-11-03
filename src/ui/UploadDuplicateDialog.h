#pragma once

#include "inat/DuplicateFinder.h"

#include <QDialog>
#include <QList>

class QLabel;
class QListWidget;
class QPushButton;

namespace pl {

class Application;

// Upload Observations: shows photos about to be uploaded beside the
// observations already on iNaturalist that look like the same sighting,
// and asks what to do. Nothing is changed here: read choice() and
// observationId() once it's accepted.
class UploadDuplicateDialog : public QDialog
{
    Q_OBJECT

public:
    enum Choice {
        Link,        // the same sighting: record the photos as of it, send nothing
        AddTo,       // the same sighting: add these photos to it
        UploadNew,   // a different one: upload it as new
    };

    UploadDuplicateDialog(Application &app, const QList<inat::UploadPhoto> &photos,
                          const QList<inat::DuplicateHit> &hits, QWidget *parent = nullptr);

    Choice choice() const { return m_choice; }
    qint64 observationId() const;   // the one chosen, for Link and AddTo

private:
    void showHit();
    void fillPhotos();

    Application &m_app;
    QList<inat::DuplicateHit> m_hits;
    QListWidget *m_hitList = nullptr;
    QListWidget *m_theirs = nullptr;
    QLabel *m_details = nullptr;
    QPushButton *m_addTo = nullptr;
    Choice m_choice = UploadNew;
};

} // namespace pl
