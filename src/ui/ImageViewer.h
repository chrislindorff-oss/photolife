#pragma once

#include <QDialog>
#include <QImage>
#include <QVector>

class QLabel;
class QPushButton;
class QScrollArea;

namespace pl {

// Full-image viewer with prev/next navigation over a fixed set of images.
// Fits each image to the window; arrow keys / Page keys navigate, Esc closes.
class ImageViewer : public QDialog
{
    Q_OBJECT

public:
    struct Item
    {
        QString path;
        QString title;     // short, single line — for the window title bar
        QString caption;   // may be multi-line — shown in the caption label
        qint64 captureId = 0;
        bool canBestShot = false;   // has an identified species, so it can be starred
        bool bestShot = false;      // currently starred as a best shot
        bool hasGps = false;
        double latitude = 0.0;      // valid only when hasGps is true
        double longitude = 0.0;
    };

    explicit ImageViewer(QWidget *parent = nullptr);

    void setItems(const QVector<Item> &items, int startIndex);

    // Updates the cached best-shot state of every item with this capture id and,
    // if one is showing, its button. Called back by the owner after it has
    // written the change in response to bestShotToggleRequested().
    void markBestShot(qint64 captureId, bool on);

signals:
    void bestShotToggleRequested(qint64 captureId, bool nominate);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void step(int delta);
    void showCurrent();
    void rescale();
    void updateBestShotButton();
    void updateMapButton();

    QScrollArea *m_scroll;
    QLabel *m_image;
    QLabel *m_caption;
    QLabel *m_counter;
    QPushButton *m_bestShot;
    QPushButton *m_viewOnMap;

    QVector<Item> m_items;
    int m_index = 0;
    QImage m_current;
};

} // namespace pl
