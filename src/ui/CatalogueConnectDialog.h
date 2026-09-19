#pragma once

#include <QDialog>

class QLabel;
class QPushButton;

namespace pl {

// Shown while opening a shared Postgres catalogue at startup -- a network
// round-trip, or waking a suspended database, can take several seconds with
// nothing on screen otherwise, which looks exactly like PhotoLife has hung.
// setStatus() reflects real progress as Database::open() works through
// connecting and migrating; the "Use Local Catalogue" button lets the user
// bail out immediately instead of waiting out a stalled or dead connection.
class CatalogueConnectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CatalogueConnectDialog(QWidget *parent = nullptr);

    void setStatus(const QString &text);

    // Hides the escape hatch once the outcome (reachable, unreachable, or
    // "use local") has been decided and the dialog is only showing final
    // migration progress -- backing out no longer makes sense past that point.
    void hideUseLocalButton();

signals:
    void useLocalRequested();

private:
    QLabel *m_status = nullptr;
    QPushButton *m_useLocal = nullptr;
};

} // namespace pl
