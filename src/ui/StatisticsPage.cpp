#include "ui/StatisticsPage.h"

#include "app/Application.h"
#include "coverage/LifeList.h"
#include "db/Database.h"
#include "ui/Theme.h"

#include <QCoreApplication>
#include <QDate>
#include <QSet>
#include <QHash>
#include <QLabel>
#include <QLocale>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

namespace pl {

BarChart::BarChart(const QString &title, bool horizontal, QWidget *parent)
    : QWidget(parent), m_title(title), m_horizontal(horizontal)
{
    setMinimumHeight(horizontal ? 120 : 220);
}

void BarChart::setData(const QStringList &labels, const QList<int> &values, const QList<int> &line,
                       const QString &lineName)
{
    m_labels = labels;
    m_values = values;
    m_line = line;
    m_lineName = lineName;
    if (m_horizontal)
        setMinimumHeight(40 + int(labels.size()) * 24);
    updateGeometry();
    update();
}

QSize BarChart::sizeHint() const
{
    return {600, m_horizontal ? 40 + int(m_labels.size()) * 24 : 260};
}

void BarChart::paintEvent(QPaintEvent *)
{
    const ThemeColors &t = themeColors(currentThemeVariant());
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QFont titleFont = font();
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(t.text);
    p.drawText(QRect(0, 0, width(), 22), Qt::AlignLeft | Qt::AlignVCenter, m_title);
    p.setFont(font());
    const QFontMetrics fm(font());
    if (m_values.isEmpty()) {
        p.setPen(t.mutedText);
        p.drawText(rect().adjusted(0, 22, 0, 0), Qt::AlignCenter, tr("Nothing to show yet."));
        return;
    }
    const int maxValue = std::max(1, *std::max_element(m_values.cbegin(), m_values.cend()));
    QColor bar = t.primaryAccent;
    if (m_horizontal) {
        int labelWidth = 0;
        for (const QString &l : m_labels)
            labelWidth = std::max(labelWidth, fm.horizontalAdvance(l));
        labelWidth = std::min(labelWidth + 8, width() / 2);
        const int rowH = 24;
        const int plotW = width() - labelWidth - 50;
        for (int i = 0; i < m_values.size(); ++i) {
            const int y = 30 + i * rowH;
            p.setPen(t.text);
            p.drawText(QRect(0, y, labelWidth - 8, rowH), Qt::AlignRight | Qt::AlignVCenter,
                       fm.elidedText(m_labels.value(i), Qt::ElideRight, labelWidth - 8));
            const int w = std::max(2, int(double(plotW) * m_values.at(i) / maxValue));
            p.setPen(Qt::NoPen);
            p.setBrush(bar);
            p.drawRoundedRect(QRect(labelWidth, y + 4, w, rowH - 8), 3, 3);
            p.setPen(t.mutedText);
            p.drawText(QRect(labelWidth + w + 6, y, 44, rowH), Qt::AlignLeft | Qt::AlignVCenter,
                       QString::number(m_values.at(i)));
        }
        return;
    }
    // Vertical bars, labels underneath; the line on its own scale.
    const int top = 30, bottom = height() - fm.height() - 8;
    const int left = 8, right = width() - (m_line.isEmpty() ? 8 : 48);
    const int n = int(m_values.size());
    const double slot = double(right - left) / n;
    const double barW = std::max(3.0, slot * 0.7);
    const int every = std::max(1, int(std::ceil(fm.horizontalAdvance(QStringLiteral("0000")) * 1.4 / slot)));
    for (int i = 0; i < n; ++i) {
        const double h = double(bottom - top - 14) * m_values.at(i) / maxValue;
        const QRectF r(left + i * slot + (slot - barW) / 2, bottom - h, barW, h);
        p.setPen(Qt::NoPen);
        p.setBrush(bar);
        p.drawRoundedRect(r, 2, 2);
        if (m_values.at(i) > 0 && slot >= fm.horizontalAdvance(QString::number(m_values.at(i))) + 2) {
            p.setPen(t.mutedText);
            p.drawText(QRectF(r.left() - 10, r.top() - 14, r.width() + 20, 14), Qt::AlignCenter,
                       QString::number(m_values.at(i)));
        }
        if (i % every == 0) {
            p.setPen(t.text);
            p.drawText(QRectF(left + i * slot - 20, bottom + 4, slot + 40, fm.height()), Qt::AlignHCenter | Qt::AlignTop,
                       m_labels.value(i));
        }
    }
    p.setPen(QPen(t.border, 1));
    p.drawLine(left, bottom, right, bottom);
    if (!m_line.isEmpty()) {
        const int maxLine = std::max(1, *std::max_element(m_line.cbegin(), m_line.cend()));
        QPainterPath path;
        for (int i = 0; i < m_line.size() && i < n; ++i) {
            const QPointF pt(left + i * slot + slot / 2, bottom - double(bottom - top - 14) * m_line.at(i) / maxLine);
            if (i == 0)
                path.moveTo(pt);
            else
                path.lineTo(pt);
        }
        p.setPen(QPen(t.secondaryAccent, 2.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(t.secondaryAccent);
        p.drawText(QRect(right + 4, top - 4, 44, fm.height()), Qt::AlignLeft, QString::number(maxLine));
        p.drawText(QRect(width() - 260, 0, 260, 22), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("— ") + m_lineName);
    }
}

namespace {
// The town, so a place typed in file names ("Anakie") and the same place
// looked up ("Anakie, Victoria, Australia") count together -- but one known
// only to the state ("Victoria, Australia": a country after it) says so.
QString placeOf(const QString &locality)
{
    static const QSet<QString> countries = [] {
        QSet<QString> out;
        for (int t = 1; t <= int(QLocale::LastTerritory); ++t)
            out.insert(QLocale::territoryToString(QLocale::Territory(t)).toLower());
        return out;
    }();
    const QStringList parts = locality.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    const QString first = parts.first().trimmed();
    if (parts.size() == 2 && countries.contains(parts.at(1).trimmed().toLower()))
        return QCoreApplication::translate("pl::StatisticsPage", "%1 (no town)").arg(first);
    return first;
}
} // namespace

StatisticsPage::StatisticsPage(Application &app, QWidget *parent) : QWidget(parent), m_app(app)
{
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);
    m_years = new BarChart(tr("New species each year"), false, this);
    m_months = new BarChart(tr("Species photographed in each month (any year)"), false, this);
    m_places = new BarChart(tr("Where new species were first photographed"), true, this);
    auto *content = new QWidget(this);
    auto *inner = new QVBoxLayout(content);
    inner->addWidget(m_summary);
    inner->addSpacing(8);
    inner->addWidget(m_years);
    inner->addSpacing(12);
    inner->addWidget(m_months);
    inner->addSpacing(12);
    inner->addWidget(m_places);
    inner->addStretch(1);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

void StatisticsPage::reload(int projectId)
{
    const bool open = projectId > 0 && m_app.database().isOpen();
    const QList<coverage::LifeListEntry> all =
        open ? coverage::numberedLifeList(coverage::loadLifeList(m_app.database().connectionName(), projectId), false)
             : QList<coverage::LifeListEntry>{};
    QMap<int, int> perYear;
    QHash<QString, int> perPlace;
    for (const coverage::LifeListEntry &e : all) {
        const QDate d = QDate::fromString(e.firstOn.left(10), Qt::ISODate);
        if (!d.isValid())
            continue;
        ++perYear[d.year()];
        const QString place = placeOf(e.locality);
        if (!place.isEmpty())
            ++perPlace[place];
    }
    QStringList yearLabels;
    QList<int> yearValues, running;
    if (!perYear.isEmpty()) {
        int total = 0;
        for (int y = perYear.firstKey(); y <= QDate::currentDate().year() && y <= perYear.lastKey() + 0; ++y) {
            yearLabels << QString::number(y);
            yearValues << perYear.value(y);
            total += perYear.value(y);
            running << total;
        }
    }
    m_years->setData(yearLabels, yearValues, running, tr("running total"));

    const QList<int> months = open ? coverage::speciesByMonth(m_app.database().connectionName(), projectId) : QList<int>{};
    QStringList monthLabels;
    for (int m = 1; m <= 12; ++m)
        monthLabels << QLocale().standaloneMonthName(m, QLocale::ShortFormat);
    m_months->setData(monthLabels, months);

    QList<QPair<QString, int>> places;
    for (auto it = perPlace.cbegin(); it != perPlace.cend(); ++it)
        places.append({it.key(), it.value()});
    std::sort(places.begin(), places.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    QStringList placeLabels;
    QList<int> placeValues;
    for (const auto &[place, n] : places.mid(0, 12)) {
        placeLabels << place;
        placeValues << n;
    }
    m_places->setData(placeLabels, placeValues);

    if (all.isEmpty()) {
        m_summary->setText(open ? tr("Nothing in this tree photographed yet.") : tr("Select a reference tree."));
        return;
    }
    int best = 0, bestYear = 0;
    for (auto it = perYear.cbegin(); it != perYear.cend(); ++it) {
        if (it.value() > best) {
            best = it.value();
            bestYear = it.key();
        }
    }
    const int thisYear = perYear.value(QDate::currentDate().year());
    m_summary->setText(tr("<b>%n species</b> photographed", nullptr, int(all.size()))
                       + (perYear.isEmpty() ? QString()
                                            : tr(" over %n year(s)", nullptr, perYear.lastKey() - perYear.firstKey() + 1))
                       + (bestYear > 0 ? tr("; the best year was %1, with %n new", nullptr, best).arg(bestYear) : QString())
                       + tr("; %n new so far this year.", nullptr, thisYear));
}

} // namespace pl
