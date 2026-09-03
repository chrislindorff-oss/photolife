#pragma once

#include <QDialog>
#include <QImage>
#include <QVector>

class QLabel;
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
        QString caption;
    };

    explicit ImageViewer(QWidget *parent = nullptr);

    void setItems(const QVector<Item> &items, int startIndex);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void step(int delta);
    void showCurrent();
    void rescale();

    QScrollArea *m_scroll;
    QLabel *m_image;
    QLabel *m_caption;
    QLabel *m_counter;

    QVector<Item> m_items;
    int m_index = 0;
    QImage m_current;
};

} // namespace pl
