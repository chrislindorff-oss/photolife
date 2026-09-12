#pragma once

#include <QDialog>
#include <QPixmap>
#include <QString>

class QLabel;
class QPushButton;

namespace pl::net {
class PhotoCache;
}

namespace pl {

// Shows one taxon's iNaturalist reference photo full-page -- the same cached
// image used in the Reference Photos grid and the tree's taxon info panel,
// just enlarged to fill the window instead of a thumbnail. Not modal, and
// reused across calls the same way ImageViewer is: keep an instance around
// and call showTaxon() again to switch species.
class ReferencePhotoDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ReferencePhotoDialog(net::PhotoCache &photos, QWidget *parent = nullptr);

    // Displays (or, if not yet downloaded, schedules and waits for) the
    // reference photo for one taxon. `photoUrl` may be empty if the tree's
    // "Fetch Reference Photos" hasn't been run for it yet.
    void showTaxon(qint64 inatId, const QString &name, const QString &commonName,
                   const QString &photoUrl, const QString &attribution);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void applyPhoto(const QPixmap &pixmap);
    void rescale();

    net::PhotoCache &m_photos;
    QString m_url;
    qint64 m_inatId = 0;

    QLabel *m_image;
    QLabel *m_caption;
    QPushButton *m_openInat;

    QPixmap m_current;
};

} // namespace pl
