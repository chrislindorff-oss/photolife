#pragma once

#include "coverage/CoverageCalculator.h"

#include <QWidget>
#include <QtGlobal>

class QLabel;
class QProgressBar;
class QFormLayout;

namespace pl {

// Compact per-project coverage summary: species photographed / total with a
// progress bar, threatened-taxon coverage broken out by status tier, the most
// recent capture date, and how much of the on-disk reference-photo cache this
// project's tree accounts for.
class CoveragePanel : public QWidget
{
    Q_OBJECT

public:
    explicit CoveragePanel(QWidget *parent = nullptr);

    void setCoverage(const coverage::ProjectCoverage &coverage);

    // `cachedCount` of `totalWithUrl` reference photos for this tree are
    // downloaded, taking `bytes` on disk. Pass totalWithUrl == 0 when the
    // project has no fetched reference photos yet.
    void setCacheUsage(int cachedCount, int totalWithUrl, qint64 bytes);

    void clear();

private:
    QLabel *m_headline;
    QProgressBar *m_bar;
    QLabel *m_threatened;
    QLabel *m_newest;
    QLabel *m_cacheUsage;
    QFormLayout *m_tiers;
};

} // namespace pl
