#pragma once

#include "net/INatClient.h"

#include <QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace pl {

class Application;

// Targets: what could be photographed at a place in a month. The species of
// the current reference tree that iNaturalist has records of within a radius
// of the place, in that month, that you haven't photographed yet -- the most
// recorded first, with your Seen ticks. Only reads from iNaturalist.
class TargetsPage : public QWidget
{
    Q_OBJECT

public:
    explicit TargetsPage(Application &app, QWidget *parent = nullptr);
    // The tree whose species are looked for.
    void setProject(int projectId);

signals:
    // Show this taxon in the reference tree.
    void showTaxon(qint64 inatId);

private:
    void findPlace();
    void find();
    void fill();

    Application &m_app;
    int m_projectId = 0;
    QLineEdit *m_where = nullptr;
    QLabel *m_place = nullptr;
    QSpinBox *m_radius = nullptr;
    QComboBox *m_month = nullptr;
    QCheckBox *m_hideSeen = nullptr;
    QPushButton *m_find = nullptr;
    QLabel *m_status = nullptr;
    QTreeWidget *m_tree = nullptr;
    std::optional<QPointF> m_at;   // x = latitude, y = longitude
    QList<net::SpeciesCount> m_results;   // the tree's, unphotographed, most recorded first
    int m_generation = 0;
};

} // namespace pl
