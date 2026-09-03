#include "ui/CoveragePanel.h"

#include <QFormLayout>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>

#include <algorithm>

namespace pl {

CoveragePanel::CoveragePanel(QWidget *parent)
    : QWidget(parent)
{
    m_headline = new QLabel(this);
    m_headline->setTextFormat(Qt::PlainText);
    QFont bold = m_headline->font();
    bold.setBold(true);
    m_headline->setFont(bold);

    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 100);
    m_bar->setTextVisible(true);

    m_threatened = new QLabel(this);
    m_newest = new QLabel(this);
    m_newest->setEnabled(false);

    auto *tierBox = new QWidget(this);
    m_tiers = new QFormLayout(tierBox);
    m_tiers->setContentsMargins(0, 0, 0, 0);
    m_tiers->setLabelAlignment(Qt::AlignLeft);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(m_headline);
    layout->addWidget(m_bar);
    layout->addWidget(m_threatened);
    layout->addWidget(tierBox);
    layout->addWidget(m_newest);
    layout->addStretch(1);

    clear();
}

void CoveragePanel::clear()
{
    m_headline->setText(tr("No reference tree selected."));
    m_bar->setValue(0);
    m_threatened->clear();
    m_newest->clear();
    while (m_tiers->rowCount() > 0)
        m_tiers->removeRow(0);
}

void CoveragePanel::setCoverage(const coverage::ProjectCoverage &coverage)
{
    while (m_tiers->rowCount() > 0)
        m_tiers->removeRow(0);

    if (coverage.speciesTotal == 0) {
        m_headline->setText(tr("No species in this tree yet."));
        m_bar->setValue(0);
        m_threatened->clear();
        m_newest->clear();
        return;
    }

    const int pct = int(std::lround(coverage.photographedFraction() * 100.0));
    m_headline->setText(tr("%1 of %2 species photographed")
                            .arg(coverage.speciesWithPhotos)
                            .arg(coverage.speciesTotal));
    m_bar->setValue(pct);
    m_bar->setFormat(QStringLiteral("%1%").arg(pct));

    if (coverage.threatenedTotal > 0) {
        m_threatened->setText(tr("%1 of %2 threatened species photographed")
                                  .arg(coverage.threatenedWithPhotos)
                                  .arg(coverage.threatenedTotal));

        QList<QString> tiers = coverage.byStatus.keys();
        std::sort(tiers.begin(), tiers.end());
        for (const QString &tier : tiers) {
            const auto t = coverage.byStatus.value(tier);
            m_tiers->addRow(tier, new QLabel(tr("%1 of %2").arg(t.withPhotos).arg(t.total), this));
        }
    } else {
        m_threatened->setText(tr("No threatened species listed."));
    }

    m_newest->setText(coverage.newestCapture.isEmpty()
                          ? tr("No dated captures.")
                          : tr("Most recent capture: %1").arg(coverage.newestCapture));
}

} // namespace pl
