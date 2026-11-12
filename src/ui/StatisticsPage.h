#pragma once

#include <QWidget>

class QLabel;

namespace pl {

class Application;
class BarChart;

// Statistics for a reference tree's photographic life list: how many new
// species each year (and the running total), which months species are
// photographed in, and where new species were first photographed.
class StatisticsPage : public QWidget
{
    Q_OBJECT

public:
    explicit StatisticsPage(Application &app, QWidget *parent = nullptr);
    void reload(int projectId);

private:
    Application &m_app;
    QLabel *m_summary = nullptr;
    BarChart *m_years = nullptr;
    BarChart *m_months = nullptr;
    BarChart *m_places = nullptr;
};

// A simple bar chart in the theme's colours: labelled bars, with the value
// on each, and optionally a line (a running total) on its own scale.
// Horizontal bars suit long labels (places).
class BarChart : public QWidget
{
    Q_OBJECT

public:
    explicit BarChart(const QString &title, bool horizontal = false, QWidget *parent = nullptr);
    void setData(const QStringList &labels, const QList<int> &values, const QList<int> &line = {},
                 const QString &lineName = QString());
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_title;
    bool m_horizontal;
    QStringList m_labels;
    QList<int> m_values;
    QList<int> m_line;
    QString m_lineName;
};

} // namespace pl
