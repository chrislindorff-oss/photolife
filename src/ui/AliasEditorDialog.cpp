#include "ui/AliasEditorDialog.h"

#include "db/Database.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pl {

AliasEditorDialog::AliasEditorDialog(pl::Database &db, QWidget *parent)
    : QDialog(parent), m_db(db)
{
    setWindowTitle(tr("Learned Names"));
    resize(640, 420);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels(
        {tr("Name in your files"), tr("Means"), tr("Scope"), tr("Kind")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);

    auto *deleteButton = new QPushButton(tr("&Forget selected"), this);
    connect(deleteButton, &QPushButton::clicked, this, &AliasEditorDialog::deleteSelected);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(this, &QDialog::finished, this, [this] {
        if (m_dirty)
            emit aliasesChanged();
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        tr("These are the corrections PhotoLife has learned. Forgetting one puts "
           "the affected photos back in the review queue on the next match."),
        this));
    layout->addWidget(m_table, 1);
    layout->addWidget(deleteButton);
    layout->addWidget(buttons);

    reload();
}

void AliasEditorDialog::reload()
{
    m_table->setRowCount(0);
    if (!m_db.isOpen())
        return;

    QSqlQuery q(QSqlDatabase::database(m_db.connectionName(), false));
    q.exec(QStringLiteral(
        "SELECT na.id, na.raw_text, t.name, na.scope, na.not_a_taxon "
        "FROM name_alias na LEFT JOIN taxon t ON t.id = na.taxon_id "
        "ORDER BY na.raw_text"));
    while (q.next()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        auto *first = new QTableWidgetItem(q.value(1).toString());
        first->setData(Qt::UserRole, q.value(0).toInt());   // alias id
        m_table->setItem(row, 0, first);

        const bool notTaxon = q.value(4).toInt() != 0;
        m_table->setItem(row, 1, new QTableWidgetItem(
                                     notTaxon ? tr("(not a taxon)") : q.value(2).toString()));
        m_table->setItem(row, 2, new QTableWidgetItem(
                                     q.value(3).toString() == QLatin1String("global")
                                         ? tr("everywhere")
                                         : tr("one folder")));
        m_table->setItem(row, 3, new QTableWidgetItem(notTaxon ? tr("locality/staging")
                                                              : tr("name")));
    }
}

void AliasEditorDialog::deleteSelected()
{
    const auto rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return;

    QSqlDatabase db = QSqlDatabase::database(m_db.connectionName(), false);
    for (const QModelIndex &idx : rows) {
        const int aliasId = m_table->item(idx.row(), 0)->data(Qt::UserRole).toInt();
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM name_alias WHERE id = ?"));
        del.addBindValue(aliasId);
        if (del.exec())
            m_dirty = true;
    }
    reload();
}

} // namespace pl
