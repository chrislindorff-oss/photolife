#pragma once

#include <QDateTime>
#include <QDialog>
#include <QList>
#include <QString>

#include <optional>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QTimer;
class QToolButton;

namespace pl::geo {
class GpxTrack;
}

namespace pl {

class Application;
class LocationPickerMap;

// Set Location on Map: the selected photos on the left, a map on the right
// to put a pin where they were taken -- found by searching for a place,
// pasting coordinates, or from other photos taken around the same time.
// Nothing is written here: exec it, then read latitude()/longitude() and the
// choices if it returns Accepted.
class GeotagDialog : public QDialog
{
    Q_OBJECT

public:
    struct Photo
    {
        QString path;          // its preview file
        QString contentHash;   // its thumbnail's key, when catalogued
        QString label;         // its file name
        QDateTime taken;
        std::optional<double> latitude;   // where it is now, if anywhere
        std::optional<double> longitude;
        bool isRaw = false;    // a RAW without a JPEG
    };
    // Another photo with a location, as a clue: one taken around the same time.
    struct Clue
    {
        double latitude = 0;
        double longitude = 0;
        QString label;
        QDateTime taken;
    };

    GeotagDialog(Application &app, const QList<Photo> &photos, const QList<Clue> &clues,
                 QWidget *parent = nullptr);

    // The folder's GPS track: drawn on the map, with a ring where each
    // photo's time falls on it. `offsetSeconds` is added to the photos' times
    // first (the camera clock's correction). Call before exec().
    void setTrack(const geo::GpxTrack &track, int offsetSeconds);

    double latitude() const;
    double longitude() const;
    // Whether the photos that already have a location get this one instead.
    bool replaceExisting() const;
    bool writeIntoRaw() const;

    // "-25.954, 153.105", "-25.954 153.105" or "25.954 S, 153.105 E", as maps
    // copy them; nullopt when it isn't a coordinate pair.
    static std::optional<QPointF> parseCoordinates(const QString &text);   // x = latitude, y = longitude

    void accept() override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    // Enter in the search or coordinates box finds the place; it never sets the location.
    void keyPressEvent(QKeyEvent *event) override;

private:
    void showOwnPhoto();
    // Until the user places the pin, it follows the photo shown: on that
    // photo's own location, or gone when it has none. `stepped`: the user
    // moved to this photo (not the dialog's first showing).
    void showOwnLocation(bool stepped);
    void pinMoved(double latitude, double longitude);
    void lookUpPlace();
    void search();
    void updateOk();

    Application &m_app;
    QList<Photo> m_photos;
    int m_ownIndex = 0;
    bool m_userPlaced = false;      // the pin is where the user put it, so it stays
    QList<std::optional<QPointF>> m_trackSpots;   // per photo: x = latitude, y = longitude on the GPS track
    QLabel *m_ownPhoto = nullptr;
    QLabel *m_ownCaption = nullptr;
    QToolButton *m_ownPrevious = nullptr;
    QToolButton *m_ownNext = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_results = nullptr;
    QLineEdit *m_coordinates = nullptr;
    LocationPickerMap *m_map = nullptr;
    QLabel *m_place = nullptr;
    QTimer *m_placeTimer = nullptr;
    int m_placeRequest = 0;         // the latest lookup: earlier answers are dropped
    QCheckBox *m_replace = nullptr;
    QCheckBox *m_intoRaw = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace pl
