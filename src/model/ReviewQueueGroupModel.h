#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QString>

namespace pl::model {

// A two-level tree over the flat review queue (usually a QSortFilterProxyModel
// on ReviewQueueModel): captures that share a display name are gathered under a
// collapsible group row; a name with only one capture stays a plain top-level
// row. Leaf rows forward every role straight through to the source, so the
// existing ReviewQueueModel::Roles keep working; group rows carry a "Name (n)"
// label and the first member's thumbnail.
//
// The structure is rebuilt whenever the source's row set changes; per-row data
// updates (thumbnails arriving, etc.) are forwarded in place without a reset.
class ReviewQueueGroupModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit ReviewQueueGroupModel(QObject *parent = nullptr);

    void setSourceModel(QAbstractItemModel *source);
    QAbstractItemModel *sourceModel() const { return m_source; }

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QHash<int, QByteArray> roleNames() const override;

    // True for a top-level row that stands for >1 capture (has an expander).
    bool isGroup(const QModelIndex &index) const;

    // The display name behind any row (group header, grouped leaf, or lone row).
    QString groupName(const QModelIndex &index) const;

    // Flat walk of every capture leaf, top to bottom, ignoring expansion — for
    // queue navigation (next / previous / skip).
    int captureCount() const;
    QModelIndex captureAt(int n) const;
    int captureNumberOf(const QModelIndex &index) const;   // -1 if not a capture leaf

    // The group header a grouped leaf sits under; invalid for a lone row.
    QModelIndex groupParentOf(const QModelIndex &index) const;

private:
    struct Group
    {
        QString name;
        QList<int> sourceRows;   // >= 1; size 1 means a lone top-level row
    };

    void rebuild();
    void onSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                             const QList<int> &roles);

    QAbstractItemModel *m_source = nullptr;
    QList<Group> m_groups;
};

} // namespace pl::model
