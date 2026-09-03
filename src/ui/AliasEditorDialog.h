#pragma once

#include <QDialog>

class QTableWidget;

namespace pl {
class Database;
}

namespace pl {

// Shows the name_alias entries the reviewer has taught PhotoLife and lets the
// user delete ones that were wrong. Deleted aliases are reconsidered on the
// next Match Library run.
class AliasEditorDialog : public QDialog
{
    Q_OBJECT

public:
    AliasEditorDialog(pl::Database &db, QWidget *parent = nullptr);

signals:
    void aliasesChanged();

private:
    void reload();
    void deleteSelected();

    pl::Database &m_db;
    QTableWidget *m_table;
    bool m_dirty = false;
};

} // namespace pl
