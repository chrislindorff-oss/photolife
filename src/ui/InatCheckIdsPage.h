#pragma once

#include "inat/IdentificationCheck.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace pl {

class Application;
class InatAccountBar;

// Check Identifications: the observations your photos went up as (or came
// down from), set against what the iNaturalist community has made of them
// since -- narrowed down to a species, taken up a level, or changed. Each can
// take iNaturalist's name (the photos here get it, as Match with iNat
// Observation gives it), or keep yours, which is remembered until the
// community's name changes again. Only reads from iNaturalist.
class InatCheckIdsPage : public QWidget
{
    Q_OBJECT

public:
    InatCheckIdsPage(Application &app, InatAccountBar &account, QWidget *parent = nullptr);

    // The photos' names here may have changed: compared again, without asking
    // iNaturalist again.
    void refreshLocal();

signals:
    // Give these photos the observation's taxon.
    void useInaturalistName(const QList<int> &captureIds, const pl::taxonomy::Observation &observation);

private:
    void check();
    void fetchNext();
    void fill();
    void useSelected();
    void keepSelected();
    void openSelected();
    QList<QTreeWidgetItem *> selectedRows() const;
    static QString keptKey(qint64 observationId, qint64 taxonId);

    Application &m_app;
    InatAccountBar &m_account;
    QPushButton *m_check = nullptr;
    QCheckBox *m_differencesOnly = nullptr;
    QLabel *m_status = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_use = nullptr;
    QPushButton *m_keep = nullptr;
    QPushButton *m_open = nullptr;

    QList<inat::LinkedObservation> m_linked;
    QHash<qint64, taxonomy::Observation> m_theirs;   // by observation id, from the last check
    QList<qint64> m_toFetch;
    int m_generation = 0;
    bool m_checking = false;
};

} // namespace pl
