#pragma once

#include <QDialog>

#include "taxonomy/ProjectBuilder.h"

class QLineEdit;
class QComboBox;
class QDialogButtonBox;

namespace pl {

// Collects what ProjectBuilder needs to build a reference tree: a project name,
// a root taxon (with optional rank), and a region.
class NewProjectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewProjectDialog(QWidget *parent = nullptr);

    taxonomy::ProjectBuilder::Request request() const;

private:
    void updateOkState();

    QLineEdit *m_name;
    QLineEdit *m_taxon;
    QComboBox *m_rank;
    QLineEdit *m_place;
    QDialogButtonBox *m_buttons;
};

} // namespace pl
