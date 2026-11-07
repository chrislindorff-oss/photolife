#include "ui/Theme.h"

#include <QApplication>
#include <algorithm>
#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTextStream>

namespace pl {

namespace {

ThemeVariant &currentVariantStorage()
{
    static ThemeVariant variant = ThemeVariant::Light;
    return variant;
}

ThemeColors makeLightColors()
{
    ThemeColors c;
    c.background      = QColor(0xF5, 0xF7, 0xFB);
    c.panel           = QColor(0xFF, 0xFF, 0xFF);
    c.border          = QColor(0xDD, 0xE5, 0xEE);
    c.primaryAccent   = QColor(0x2F, 0x6F, 0xED);
    c.secondaryAccent = QColor(0x2E, 0x7D, 0x68);
    c.text            = QColor(0x1F, 0x29, 0x37);
    c.mutedText       = QColor(0x6B, 0x72, 0x80);
    c.selectedRow     = QColor(0xDF, 0xEA, 0xFD);
    c.indicatorBorder = c.border;
    c.indicatorCheckedBorder = c.primaryAccent;
    c.indicatorDisabledBorder = c.border;
    c.radius          = 6;

    c.statusAuto      = QColor(0x2E, 0x7D, 0x32);
    c.statusConfirmed = QColor(0x15, 0x65, 0xC0);
    c.statusPending   = QColor(0xE6, 0x9A, 0x00);
    c.statusUnmatched = QColor(0xC6, 0x28, 0x28);

    c.bestShotGold    = QColor(0xFF, 0xC1, 0x07);
    c.badgeBackground = QColor(0, 0, 0, 200);
    c.badgeText       = Qt::white;

    c.warning         = QColor(0xB0, 0x50, 0x00);
    c.positive        = QColor(0x2E, 0x7D, 0x32);
    c.danger          = QColor(0xC6, 0x28, 0x28);
    c.introduced      = QColor(0x1E, 0x63, 0xC8);
    return c;
}

ThemeColors makeDarkColors()
{
    ThemeColors c;
    c.background      = QColor(0x1B, 0x1F, 0x24);
    c.panel           = QColor(0x23, 0x28, 0x30);
    c.border          = QColor(0x33, 0x3B, 0x44);
    c.primaryAccent   = QColor(0x5B, 0x9C, 0xFF);   // lightened for contrast on dark bg
    c.secondaryAccent = QColor(0x4C, 0xB0, 0x96);
    c.text            = QColor(0xE6, 0xE9, 0xEC);
    c.mutedText       = QColor(0x8A, 0x93, 0xA0);
    c.selectedRow     = QColor(0x2C, 0x3B, 0x52);   // desaturated blue-gray
    c.indicatorBorder = QColor(0xFF, 0xFF, 0xFF);   // checkboxes vanish on dark panels otherwise
    c.indicatorCheckedBorder = c.indicatorBorder;
    c.indicatorDisabledBorder = c.mutedText;
    c.radius          = 6;

    c.statusAuto      = QColor(0x4C, 0xAF, 0x50);
    c.statusConfirmed = QColor(0x42, 0xA5, 0xF5);
    c.statusPending   = QColor(0xFF, 0xB7, 0x4D);
    c.statusUnmatched = QColor(0xEF, 0x53, 0x50);

    c.bestShotGold    = QColor(0xFF, 0xCA, 0x28);
    c.badgeBackground = QColor(0, 0, 0, 200);
    c.badgeText       = Qt::white;

    c.warning         = QColor(0xE0, 0x8A, 0x3D);
    c.positive        = QColor(0x4C, 0xAF, 0x50);
    c.danger          = QColor(0xEF, 0x53, 0x50);
    c.introduced      = QColor(0x64, 0xB5, 0xF6);   // lightened for contrast on dark bg
    return c;
}

QPalette paletteFor(const ThemeColors &c)
{
    QPalette p;
    p.setColor(QPalette::Window, c.background);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.panel);
    p.setColor(QPalette::AlternateBase, c.background);
    p.setColor(QPalette::ToolTipBase, c.panel);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.panel);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::BrightText, c.danger);
    p.setColor(QPalette::Link, c.primaryAccent);
    p.setColor(QPalette::Highlight, c.primaryAccent);
    p.setColor(QPalette::HighlightedText, c.panel);

    p.setColor(QPalette::Disabled, QPalette::WindowText, c.mutedText);
    p.setColor(QPalette::Disabled, QPalette::Text, c.mutedText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.mutedText);
    return p;
}

QString loadQssTemplate()
{
    QFile file(QStringLiteral(":/styles/theme.qss"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream in(&file);
    return in.readAll();
}

QString substituteTokens(QString qss, const ThemeColors &c)
{
    const QList<QPair<QString, QString>> tokens = {
        {QStringLiteral("@background@"), c.background.name()},
        {QStringLiteral("@panel@"), c.panel.name()},
        {QStringLiteral("@border@"), c.border.name()},
        {QStringLiteral("@primaryAccent@"), c.primaryAccent.name()},
        {QStringLiteral("@secondaryAccent@"), c.secondaryAccent.name()},
        {QStringLiteral("@text@"), c.text.name()},
        {QStringLiteral("@mutedText@"), c.mutedText.name()},
        {QStringLiteral("@selectedRow@"), c.selectedRow.name()},
        {QStringLiteral("@indicatorBorder@"), c.indicatorBorder.name()},
        {QStringLiteral("@indicatorCheckedBorder@"), c.indicatorCheckedBorder.name()},
        {QStringLiteral("@indicatorDisabledBorder@"), c.indicatorDisabledBorder.name()},
        {QStringLiteral("@radius@"), QString::number(c.radius)},
    };
    for (const auto &[token, value] : tokens)
        qss.replace(token, value);
    return qss;
}

// Checkboxes in lists and trees (and menus) are drawn by Fusion, whose
// outline is the window colour darkened -- next to invisible on a dark panel,
// where a style sheet can't reach it without losing the tick. In dark mode
// they're drawn here instead: a white outline, the tick in white on the
// accent colour, and grey when disabled (a tick that can't be changed).
class IndicatorStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const override
    {
        if ((pe != PE_IndicatorItemViewItemCheck && pe != PE_IndicatorCheckBox)
            || currentThemeVariant() != ThemeVariant::Dark) {
            QProxyStyle::drawPrimitive(pe, opt, p, w);
            return;
        }
        const ThemeColors &c = themeColors(ThemeVariant::Dark);
        const bool enabled = opt->state & State_Enabled;
        const bool on = opt->state & State_On;
        const bool partly = opt->state & State_NoChange;
        const int side = std::min(opt->rect.width(), opt->rect.height()) - 1;
        const QRectF box(opt->rect.x() + (opt->rect.width() - side) / 2.0 + 0.5,
                         opt->rect.y() + (opt->rect.height() - side) / 2.0 + 0.5, side - 1, side - 1);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QColor outline = enabled ? c.indicatorBorder : c.indicatorDisabledBorder;
        const QColor fill = (on || partly) ? (enabled ? c.primaryAccent : c.border) : c.panel;
        p->setPen(QPen(outline, 1.2));
        p->setBrush(fill);
        p->drawRoundedRect(box, 3, 3);
        p->setPen(QPen(enabled ? QColor(Qt::white) : c.mutedText, std::max(1.5, side / 7.0), Qt::SolidLine,
                       Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        if (on) {
            QPainterPath tick;
            tick.moveTo(box.left() + box.width() * 0.22, box.top() + box.height() * 0.52);
            tick.lineTo(box.left() + box.width() * 0.42, box.top() + box.height() * 0.72);
            tick.lineTo(box.left() + box.width() * 0.78, box.top() + box.height() * 0.30);
            p->drawPath(tick);
        } else if (partly) {
            p->drawLine(QPointF(box.left() + box.width() * 0.25, box.center().y()),
                        QPointF(box.right() - box.width() * 0.25, box.center().y()));
        }
        p->restore();
    }
};

} // namespace

const ThemeColors &themeColors(ThemeVariant variant)
{
    static const ThemeColors light = makeLightColors();
    static const ThemeColors dark = makeDarkColors();
    return variant == ThemeVariant::Dark ? dark : light;
}

ThemeVariant currentThemeVariant()
{
    return currentVariantStorage();
}

void applyTheme(QApplication &app, ThemeVariant variant)
{
    currentVariantStorage() = variant;

    app.setStyle(new IndicatorStyle(QStyleFactory::create(QStringLiteral("Fusion"))));

    const ThemeColors &c = themeColors(variant);
    app.setPalette(paletteFor(c));

    const QString qss = substituteTokens(loadQssTemplate(), c);
    app.setStyleSheet(qss);
}

} // namespace pl
