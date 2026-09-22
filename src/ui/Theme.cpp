#include "ui/Theme.h"

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QStyleFactory>
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
        {QStringLiteral("@radius@"), QString::number(c.radius)},
    };
    for (const auto &[token, value] : tokens)
        qss.replace(token, value);
    return qss;
}

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

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    const ThemeColors &c = themeColors(variant);
    app.setPalette(paletteFor(c));

    const QString qss = substituteTokens(loadQssTemplate(), c);
    app.setStyleSheet(qss);
}

} // namespace pl
