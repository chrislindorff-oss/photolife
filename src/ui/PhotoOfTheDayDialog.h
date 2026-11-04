#pragma once

#include "catalogue/PhotoOfTheDay.h"

#include <QDialog>
#include <QImage>
#include <QList>

class QLabel;

namespace pl {

class Application;
class PhotoArea;

// Nature Photo of the Day: the day's photo nearly full screen on a dark
// background -- the cached 1024 px preview at once, the full-resolution
// image as soon as it's read -- with its species' names and its whole
// lineage from Life down, when and where it was taken, and whether it pops
// up when PhotoLife opens. Lineage steps the catalogue doesn't have are
// asked of iNaturalist (shown, not stored). F11 toggles full screen.
class PhotoOfTheDayDialog : public QDialog
{
    Q_OBJECT

public:
    PhotoOfTheDayDialog(Application &app, const catalogue::PhotoOfTheDay::Pick &pick, QWidget *parent = nullptr);

signals:
    void showInLibrary(int captureId);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void loadFullImage();
    void showLineage();
    void fetchMissingSteps();

    Application &m_app;
    catalogue::PhotoOfTheDay::Pick m_pick;
    QList<catalogue::PhotoOfTheDay::LineageStep> m_lineage;
    PhotoArea *m_photo = nullptr;
    QLabel *m_lineageLabel = nullptr;
    bool m_full = false;   // the full-resolution image is in
};

} // namespace pl
