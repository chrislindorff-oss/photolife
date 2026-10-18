#pragma once

#include "organise/OrganiseSession.h"

#include <QWidget>

#include <memory>

class QLabel;
class QListView;
class QPushButton;
class QToolButton;

namespace pl {

class Application;
class ImageViewer;

namespace ui_organise {
class OrganiseModel;
class OrganiseDelegate;
}

// Sorting a day's download folder into taxon folders: the photos as a
// filmstrip (or a grid), in the order they were taken. Click the first photo
// of each taxon and name it; every photo up to the next named one goes with
// it. Mark rejects. "Organise into Folders" then moves each run into a
// subfolder of the download folder named for it (rejects into "Rejects"),
// keeping the file names -- and can be undone. Progress is kept in the
// folder, so a session can be left and picked up again.
class OrganiseView : public QWidget
{
    Q_OBJECT

public:
    explicit OrganiseView(Application &app, QWidget *parent = nullptr);
    ~OrganiseView() override;

    void chooseFolder();
    void openFolder(const QString &folder);
    void commit();
    void undoLast();
    QString folder() const { return m_session.folder(); }

signals:
    void statusMessage(const QString &text, int timeoutMs);
    // Files were moved in `folder` (organised or undone).
    void organised(const QString &folder);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void setFilmstrip(bool on);
    void updateCellSize();
    void updateCounts();
    void nameAt(int row);
    void toggleRejected();
    void showFullSize(int row);
    void contextMenuAt(const QPoint &pos);
    void changed(int row);   // after a name/reject: repaint the run, save
    void updatePreview();

    Application &m_app;
    organise::OrganiseSession m_session;
    ui_organise::OrganiseModel *m_model = nullptr;
    ui_organise::OrganiseDelegate *m_delegate = nullptr;
    QListView *m_list = nullptr;
    QLabel *m_folderLabel = nullptr;
    QLabel *m_counts = nullptr;
    QLabel *m_hint = nullptr;
    QToolButton *m_filmstripButton = nullptr;
    QToolButton *m_gridButton = nullptr;
    QPushButton *m_commitButton = nullptr;
    QPushButton *m_undoButton = nullptr;
    ImageViewer *m_viewer = nullptr;
    QLabel *m_preview = nullptr;          // the selected photo, large (filmstrip mode)
    QLabel *m_previewCaption = nullptr;
    QString m_previewKey;                 // its thumbnail key, while loading
    bool m_filmstrip = true;
    bool m_loading = false;
};

} // namespace pl
