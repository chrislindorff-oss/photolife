#pragma once

#include "coverage/CoverageCalculator.h"

#include <QHash>
#include <QString>
#include <QWidget>
#include <QtGlobal>

class QLabel;
class QProgressBar;
class QFormLayout;

namespace pl {

// Compact per-project coverage summary: species photographed / total with a
// progress bar, threatened-taxon coverage broken out by status tier (left)
// beside introduced-species coverage (right), the most
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

    // Highlights whichever status is the active tree filter (both
    // false/empty = no filter) and shows/hides the "Clear filter" link.
    void setActiveStatusFilter(bool anyThreatened, const QString &tier);
    // Highlights the introduced summary while its tree filter is on.
    void setActiveIntroducedFilter(bool active);

signals:
    // Emitted when the user clicks a specific tier's label (e.g. "Endangered").
    void tierClicked(const QString &status);
    // Emitted when the user clicks the overall "N of M threatened" summary.
    void anyThreatenedClicked();
    // Emitted when the user clicks the "N of M introduced" summary.
    void introducedClicked();
    // Emitted when the user clicks the "Clear filter" link.
    void clearFilterRequested();

private:
    void applyHighlight();

    QLabel *m_headline;
    QProgressBar *m_bar;
    QLabel *m_threatened;
    QLabel *m_introduced;
    QLabel *m_introducedDetail;
    QLabel *m_newest;
    QLabel *m_cacheUsage;
    QLabel *m_clearFilterLink;
    QFormLayout *m_tiers;
    QHash<QString, QLabel *> m_tierLabels;
    bool m_activeAny = false;
    QString m_activeTier;
    int m_threatenedWithPhotos = 0;
    int m_threatenedTotal = 0;
    bool m_activeIntroduced = false;
    int m_introducedWithPhotos = 0;
    int m_introducedTotal = 0;
    int m_speciesTotal = 0;
};

} // namespace pl
