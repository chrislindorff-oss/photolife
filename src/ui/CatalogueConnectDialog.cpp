#include "ui/CatalogueConnectDialog.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace pl {

CatalogueConnectDialog::CatalogueConnectDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("PhotoLife"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setFixedWidth(380);

    m_status = new QLabel(tr("Connecting to shared catalogue…"), this);
    m_status->setWordWrap(true);

    m_useLocal = new QPushButton(tr("Use Local Catalogue Instead"), this);
    connect(m_useLocal, &QPushButton::clicked, this, &CatalogueConnectDialog::useLocalRequested);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_status);
    layout->addWidget(m_useLocal, 0, Qt::AlignRight);
}

void CatalogueConnectDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void CatalogueConnectDialog::hideUseLocalButton()
{
    m_useLocal->setVisible(false);
}

} // namespace pl
