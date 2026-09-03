#pragma once

#include "coverage/CoverageCalculator.h"

#include <QWidget>

class QLabel;
class QProgressBar;
class QFormLayout;

namespace pl {

// Compact per-project coverage summary: species photographed / total with a
// progress bar, threatened-taxon coverage broken out by status tier, and the
// most recent capture date.
class CoveragePanel : public QWidget
{
    Q_OBJECT

public:
    explicit CoveragePanel(QWidget *parent = nullptr);

    void setCoverage(const coverage::ProjectCoverage &coverage);
    void clear();

private:
    QLabel *m_headline;
    QProgressBar *m_bar;
    QLabel *m_threatened;
    QLabel *m_newest;
    QFormLayout *m_tiers;
};

} // namespace pl
