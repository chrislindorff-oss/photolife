#pragma once

#include "inat/PhotoIdentifier.h"

#include <QDialog>
#include <QList>
#include <QPair>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace pl {

class Application;
namespace organise {
class OrganiseSession;
}

// Identify Runs (Organise): proposes where new runs start among the photos
// no name covers yet -- a gap of more than N seconds between shots -- asks
// iNaturalist's computer vision about the first photo of each, one at a
// time, and lists them with the best suggestion chosen. Each can be kept,
// changed (another suggestion, or typed) or left out; names() then gives the
// photos to name. Photos already named aren't touched.
class IdentifyRunsDialog : public QDialog
{
    Q_OBJECT

public:
    IdentifyRunsDialog(Application &app, const organise::OrganiseSession &session, QWidget *parent = nullptr);

    // The photos to name, by index, with their names: the ticked rows.
    QList<QPair<int, QString>> names() const;

private:
    void propose();
    void start();
    void stop();
    void next();
    void finished(const QString &key, const std::optional<QRectF> &crop, const inat::Identification &result);
    void updateButtons();
    void thumbnailReady();
    // Fills a row's name box from its suggestions, by the common-names choice.
    void fillNames(int row);

    Application &m_app;
    const organise::OrganiseSession &m_session;
    QSpinBox *m_gap = nullptr;
    QCheckBox *m_commonNames = nullptr;
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_identify = nullptr;
    QPushButton *m_getToken = nullptr;
    QPushButton *m_ok = nullptr;

    QList<int> m_starts;      // photo index per row
    QHash<int, QList<taxonomy::Taxon>> m_suggested;   // per row, best first
    int m_asking = -1;        // the row being identified; -1 when not running
    bool m_running = false;
};

} // namespace pl
