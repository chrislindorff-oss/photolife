#include "ui/CoveragePanel.h"

#include "ui/Theme.h"

#include <QFormLayout>
#include <QLabel>
#include <QLocale>
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
    m_threatened->setTextFormat(Qt::RichText);
    connect(m_threatened, &QLabel::linkActivated, this, [this](const QString &) {
        emit anyThreatenedClicked();
    });

    m_clearFilterLink = new QLabel(
        tr("<a href=\"clear\">Clear filter</a>"), this);
    m_clearFilterLink->setTextFormat(Qt::RichText);
    m_clearFilterLink->setVisible(false);
    connect(m_clearFilterLink, &QLabel::linkActivated, this, [this](const QString &) {
        emit clearFilterRequested();
    });

    m_newest = new QLabel(this);
    m_newest->setEnabled(false);
    m_cacheUsage = new QLabel(this);
    m_cacheUsage->setEnabled(false);

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
    layout->addWidget(m_clearFilterLink);
    layout->addWidget(m_newest);
    layout->addWidget(m_cacheUsage);
    layout->addStretch(1);

    clear();
}

void CoveragePanel::clear()
{
    m_headline->setText(tr("No reference tree selected."));
    m_bar->setValue(0);
    m_threatened->clear();
    m_newest->clear();
    m_cacheUsage->clear();
    while (m_tiers->rowCount() > 0)
        m_tiers->removeRow(0);
    m_tierLabels.clear();
    m_activeAny = false;
    m_activeTier.clear();
    m_threatenedWithPhotos = 0;
    m_threatenedTotal = 0;
    m_clearFilterLink->setVisible(false);
}

void CoveragePanel::setCoverage(const coverage::ProjectCoverage &coverage)
{
    while (m_tiers->rowCount() > 0)
        m_tiers->removeRow(0);
    m_tierLabels.clear();

    if (coverage.speciesTotal == 0) {
        m_headline->setText(tr("No species in this tree yet."));
        m_bar->setValue(0);
        m_threatened->clear();
        m_newest->clear();
        m_cacheUsage->clear();
        m_threatenedWithPhotos = 0;
        m_threatenedTotal = 0;
        m_clearFilterLink->setVisible(false);
        return;
    }

    const int pct = int(std::lround(coverage.photographedFraction() * 100.0));
    m_headline->setText(tr("%1 of %2 species photographed")
                            .arg(coverage.speciesWithPhotos)
                            .arg(coverage.speciesTotal));
    m_bar->setValue(pct);
    m_bar->setFormat(QStringLiteral("%1%").arg(pct));

    m_threatenedWithPhotos = coverage.threatenedWithPhotos;
    m_threatenedTotal = coverage.threatenedTotal;

    if (coverage.threatenedTotal > 0) {
        QList<QString> tiers = coverage.byStatus.keys();
        std::sort(tiers.begin(), tiers.end());
        for (const QString &tier : tiers) {
            const auto t = coverage.byStatus.value(tier);
            auto *label = new QLabel(this);
            label->setTextFormat(Qt::RichText);
            connect(label, &QLabel::linkActivated, this, [this, tier](const QString &) {
                emit tierClicked(tier);
            });
            m_tierLabels.insert(tier, label);
            m_tiers->addRow(label, new QLabel(tr("%1 of %2").arg(t.withPhotos).arg(t.total), this));
        }
    } else {
        m_threatened->setText(tr("No threatened species listed."));
    }

    m_newest->setText(coverage.newestCapture.isEmpty()
                          ? tr("No dated captures.")
                          : tr("Most recent capture: %1").arg(coverage.newestCapture));

    applyHighlight();
}

void CoveragePanel::setActiveStatusFilter(bool anyThreatened, const QString &tier)
{
    m_activeAny = anyThreatened;
    m_activeTier = tier;
    applyHighlight();
}

void CoveragePanel::applyHighlight()
{
    const pl::ThemeColors &theme = pl::themeColors(pl::currentThemeVariant());
    const QString color = theme.warning.name();

    for (auto it = m_tierLabels.constBegin(); it != m_tierLabels.constEnd(); ++it) {
        const QString &tier = it.key();
        const QString text = tier.toHtmlEscaped();
        it.value()->setText(tier == m_activeTier
                                 ? QStringLiteral("<a href=\"%1\" style=\"color:%2;\"><b>%3</b></a>")
                                       .arg(text, color, text)
                                 : QStringLiteral("<a href=\"%1\">%2</a>").arg(text, text));
    }

    if (m_threatenedTotal > 0) {
        const QString summary =
            tr("%1 of %2 threatened species photographed")
                .arg(m_threatenedWithPhotos)
                .arg(m_threatenedTotal);
        m_threatened->setText(m_activeAny
                                   ? QStringLiteral("<a href=\"any\" style=\"color:%1;\"><b>%2</b></a>")
                                         .arg(color, summary)
                                   : QStringLiteral("<a href=\"any\">%1</a>").arg(summary));
    }

    m_clearFilterLink->setVisible(m_activeAny || !m_activeTier.isEmpty());
}

void CoveragePanel::setCacheUsage(int cachedCount, int totalWithUrl, qint64 bytes)
{
    if (totalWithUrl == 0) {
        m_cacheUsage->setText(tr("No reference photos fetched yet."));
        return;
    }
    m_cacheUsage->setText(tr("Reference photo cache: %1 of %2 downloaded (%3)")
                              .arg(cachedCount)
                              .arg(totalWithUrl)
                              .arg(QLocale().formattedDataSize(bytes)));
}

} // namespace pl
