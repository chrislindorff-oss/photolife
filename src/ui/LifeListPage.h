#pragma once

#include "coverage/LifeList.h"
#include "ui/ImageViewer.h"

#include <QList>
#include <QPair>
#include <QSet>
#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QSortFilterProxyModel;
class QToolButton;
class QTreeView;

namespace pl {

class Application;

namespace model {
class LifeListModel;
}

// The Photographic Life List tab: a reference tree's species in the order
// they were first photographed, numbered, with a filter, the subspecies
// toggle, thumbnails and a choice of columns -- Camera and Focal length read
// from the first photos' EXIF in the background while they're shown. A row's
// first photo opens full screen.
class LifeListPage : public QWidget
{
    Q_OBJECT

public:
    // What the page asks of the main window.
    struct Hooks
    {
        std::function<QStringList(const QList<int> &captureIds)> onlineOnlyRenditions;
        std::function<void(const QStringList &paths, std::function<void()> then)> fetchPhotos;
        std::function<void(std::function<void()> afterScan)> requestScan;
        std::function<void(const QVector<ImageViewer::Item> &items, int start)> showInViewer;
    };

    LifeListPage(Application &app, Hooks hooks, QWidget *parent = nullptr);

    // Reads the life list of the tree `projectId` (0: none) from the catalogue again.
    void reload(int projectId);

signals:
    void statusMessage(const QString &text, int timeoutMs);   // empty text: clear it
    void showTaxonPhotos(qint64 inatId);                      // Show All Photos of This Taxon
    void countChanged(int shown);                             // entries listed, for the tab's title

private:
    void showEntries();   // numbers what reload() read and lists it (the subspecies toggle)
    void openPhoto(const QModelIndex &proxyIndex);
    void readCameras();
    void readNextCameraChunk();

    Application &m_app;
    Hooks m_hooks;
    int m_projectId = 0;
    QList<coverage::LifeListEntry> m_all;   // reload()'s, species and below
    QLineEdit *m_search = nullptr;
    QCheckBox *m_infra = nullptr;     // subspecies, varieties and hybrids as entries
    QCheckBox *m_thumbs = nullptr;    // each entry's first photo
    QToolButton *m_columnsButton = nullptr;   // Columns…: which columns show
    QMenu *m_columnsMenu = nullptr;
    // Camera and Focal length: the first photos' EXIF, read a chunk at a time.
    QList<QPair<int, QString>> m_cameraQueue;   // capture id, file
    QSet<int> m_cameraRequested;                // queued this session (read or not)
    bool m_cameraReading = false;
    int m_cameraTotal = 0;
    int m_cameraDone = 0;
    QLabel *m_counts = nullptr;
    QTreeView *m_view = nullptr;
    model::LifeListModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
};

} // namespace pl
