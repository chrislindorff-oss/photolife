#pragma once

#include <QDialog>
#include <QImage>
#include <QVector>

class QLabel;
class QPushButton;
class QScrollArea;

namespace pl {

class ImageCanvas;

// Full-image viewer with prev/next navigation over a fixed set of images.
// Fits each image to the window; arrow keys / Page keys navigate, Esc closes.
// Zoom: A (or a double-click) toggles fit / actual pixels (1:1, one image
// pixel per screen pixel) at the spot under the pointer; the wheel zooms
// around the pointer, + / - step, 0 or F fits. Zoomed in, drag with the hand
// to pan. Stepping to another photo keeps the zoom and spot, for comparing a
// burst's sharpness. Where the owner allows it (Organise), R rejects the
// photo -- or takes it back -- and a rejected one moves on to the next.
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
        bool canReject = false;     // R marks it as a reject (Organise)
        bool rejected = false;
    };

    explicit ImageViewer(QWidget *parent = nullptr);

    void setItems(const QVector<Item> &items, int startIndex);

    // Updates the cached best-shot state of every item with this capture id and,
    // if one is showing, its button. Called back by the owner after it has
    // written the change in response to bestShotToggleRequested().
    void markBestShot(qint64 captureId, bool on);
    // Replaces one item's details (its caption, reject mark) after the owner
    // has acted on rejectToggleRequested(); the photo itself isn't reloaded.
    void updateItem(int index, const Item &item);

signals:
    void bestShotToggleRequested(qint64 captureId, bool nominate);
    void rejectToggleRequested(int index, bool reject);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void step(int delta);
    void showCurrent();
    void rescale();
    // Zooms to `scale` (device pixels per image pixel; <= 0: fit) keeping the
    // image point under `anchor` (viewport coordinates) where it is.
    void zoomTo(double scale, QPoint anchor);
    void zoomBy(double factor, QPoint anchor);
    double fitScale() const;
    QPoint viewportCenter() const;
    void updateCounter();
    void updateBestShotButton();
    void updateMapButton();
    void updateRejectButton();
    void toggleReject();

    QScrollArea *m_scroll;
    ImageCanvas *m_image;
    QLabel *m_caption;
    QLabel *m_counter;
    QPushButton *m_bestShot;
    QPushButton *m_viewOnMap;
    QPushButton *m_reject;

    QVector<Item> m_items;
    int m_index = 0;
    QImage m_current;
    bool m_fit = true;
    double m_scale = 1.0;        // device pixels per image pixel, when not fitting
    QPointF m_spot{0.5, 0.5};    // the viewed spot, as a fraction of the image (kept across photos)
    bool m_panning = false;
    QPoint m_panFrom;
};

} // namespace pl
