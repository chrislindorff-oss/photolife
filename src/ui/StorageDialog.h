#pragma once

#include <QDialog>
#include <QList>
#include <QString>

class QComboBox;
class QLabel;
class QPushButton;

namespace pl {

class Application;

// Storage Usage: how much disk the catalogue and each of PhotoLife's caches
// take, with a way to clear what can be fetched or made again -- map tiles,
// iNaturalist's reference photos, and thumbnails of photos no longer in any
// catalogue on this computer. Photos themselves are never touched.
class StorageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit StorageDialog(Application &app, QWidget *parent = nullptr);

private:
    void measure();                       // sizes, on a worker
    void clearCache(int row);
    void removeUnusedThumbnails();
    void setBusy(bool busy);

    struct Row
    {
        QString name;
        QString dir;          // empty for the catalogue
        QLabel *size = nullptr;
        QPushButton *button = nullptr;
    };

    Application &m_app;
    QList<Row> m_rows;
    QLabel *m_total = nullptr;
    QLabel *m_note = nullptr;
    QComboBox *m_tileLimit = nullptr;
    int m_generation = 0;     // each measure() bumps it: older answers are dropped
};

} // namespace pl
