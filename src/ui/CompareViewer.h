#pragma once

#include "ui/ImageViewer.h"

#include <QDialog>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QVector>

class QLabel;

namespace pl {

class ComparePane;

// Compare: two to four photos side by side, zoomed and panned together, to
// pick the sharpest of a burst. The wheel zooms (all of them, round the same
// spot), a drag pans, A or a double-click toggles fit and 1:1 at the pointer.
// R rejects, B stars and N names the photo under the pointer (asking the
// owner, as ImageViewer does); Esc closes.
class CompareViewer : public QDialog
{
    Q_OBJECT

public:
    // `indexes`: what the owner calls each photo, echoed back in the signals.
    CompareViewer(const QVector<ImageViewer::Item> &items, const QList<int> &indexes, QWidget *parent = nullptr);

    // After the owner has acted: the photo's captions and marks again.
    void updateItem(int index, const ImageViewer::Item &item, const QString &note = QString());
    // A hint shown under a photo, such as "sharpest of the burst".
    void setNote(int index, const QString &note);

    // The view all the panes share.
    bool fit() const { return m_fit; }
    double scale() const { return m_scale; }
    QPointF spot() const { return m_spot; }
    void setView(bool fit, double scale, QPointF spot);

signals:
    void rejectToggleRequested(int index, bool reject);
    void bestToggleRequested(int index, bool best);
    void nameRequested(int index);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    friend class ComparePane;
    ComparePane *paneUnderPointer() const;
    void zoomAt(ComparePane *pane, double factor, QPointF at);
    void toggleOneToOne(ComparePane *pane, QPointF at);
    void panBy(ComparePane *pane, QPointF delta);
    void refreshCaptions();

    QList<ComparePane *> m_panes;
    QList<int> m_indexes;
    QVector<ImageViewer::Item> m_items;
    QList<QString> m_notes;
    QList<QLabel *> m_captions;
    bool m_fit = true;
    double m_scale = 1.0;        // device pixels per image pixel, when not fitting
    QPointF m_spot{0.5, 0.5};    // the middle of the view, as a fraction of the picture
};

} // namespace pl
