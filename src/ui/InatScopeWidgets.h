#pragma once

#include <QComboBox>
#include <QList>
#include <QString>
#include <QWidget>

#include <utility>

class QLabel;
class QPushButton;

namespace pl::net {
class INatClient;
}

namespace pl {

// Picks a reference tree for an iNaturalist tool. Follows the Reference
// Trees dock's active tree until the user picks one here themselves.
// Optionally starts with an "all trees" entry.
class ReferenceTreeCombo : public QComboBox
{
    Q_OBJECT

public:
    // `allTreesLabel` non-empty adds that entry first, as projectId() 0.
    explicit ReferenceTreeCombo(const QString &allTreesLabel = QString(),
                                QWidget *parent = nullptr);

    // Repopulates from (project id, display name) pairs, keeping the current
    // choice where it still exists.
    void setTrees(const QList<std::pair<int, QString>> &trees, int activeProjectId);
    void followActiveTree(int projectId);

    // > 0 for a tree, 0 for the "all trees" entry, -1 when there's nothing.
    int projectId() const;
    // Every tree's id, whatever is selected.
    QList<int> allProjectIds() const;

private:
    QString m_allTreesLabel;
    bool m_chosenByUser = false;
};

// "Limit to taxon: <group> [Choose Taxon…] [Use Tree Selection] [All Taxa]"
// -- narrows an iNaturalist tool to one taxon and everything beneath it.
class TaxonLimitRow : public QWidget
{
    Q_OBJECT

public:
    explicit TaxonLimitRow(net::INatClient &inat, QWidget *parent = nullptr);

    qint64 taxonId() const { return m_taxonId; }   // 0 = every taxon
    QString label() const { return m_label; }
    void setTaxon(qint64 inatId, const QString &label);

    // The taxon selected in the Reference Trees dock (0 = none), offered by
    // the Use Tree Selection button.
    void setTreeSelection(qint64 inatId, const QString &name);

signals:
    void changed();

private:
    void chooseTaxon();
    void updateButtons();

    net::INatClient &m_inat;
    qint64 m_taxonId = 0;
    QString m_label;
    qint64 m_treeSelectionId = 0;
    QString m_treeSelectionName;
    QLabel *m_text = nullptr;
    QPushButton *m_useTreeSelectionButton = nullptr;
    QPushButton *m_clearButton = nullptr;
};

} // namespace pl
