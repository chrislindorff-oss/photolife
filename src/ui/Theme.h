#pragma once

#include <QColor>

class QApplication;

namespace pl {

// Which built-in palette to apply. Light is the default and the only one
// applied automatically at startup; Dark is reachable via the View menu's
// "Dark Mode" toggle in MainWindow, persisted through Settings.
enum class ThemeVariant {
    Light,
    Dark,
};

// All named colors the app's styling (QSS + hand-painted widgets) draws
// from. Keeping every color as a named field here -- rather than scattered
// literal QColor/hex-string constants throughout MainWindow.cpp,
// ReviewPane.cpp, TaxonomyTreeModel.cpp, MapView.cpp -- is what makes the
// whole app re-themeable between Light and Dark from one place.
//
// Field groups:
//  - Chrome: background/panel/border/text/mutedText/selectedRow plus the two
//    accent colors, consumed by theme.qss token substitution.
//  - Semantic status colors: consumed directly by CaptureDelegate's
//    hand-painted status dot, TaxonomyTreeModel's coverage-column foreground,
//    the Postgres connection indicator, and the "Missing Taxa" warning color --
//    all places QSS cannot reach because the color is baked into a paint()
//    call or an inline rich-text HTML string rather than a QWidget property.
struct ThemeColors {
    // Chrome
    QColor background;
    QColor panel;
    QColor border;
    QColor primaryAccent;
    QColor secondaryAccent;
    QColor text;
    QColor mutedText;
    QColor selectedRow;
    int radius = 6;

    // Match-status semantics (CaptureDelegate status dot; same hues reused
    // for TaxonomyTreeModel's coverage tick / Postgres indicator where the
    // meaning lines up: "auto" and "positive/connected" share a color family,
    // "unmatched" and "disconnected" share another).
    QColor statusAuto;         // green - auto-matched
    QColor statusConfirmed;    // blue  - confirmed
    QColor statusPending;      // amber - needs review
    QColor statusUnmatched;    // red   - unmatched / disconnected

    QColor bestShotGold;
    QColor badgeBackground;    // file-type/GPS pill background
    QColor badgeText;          // file-type/GPS pill text

    QColor warning;            // Missing Taxa conservation-status rows
    QColor positive;           // Postgres connected / "has photos in subtree"
    QColor danger;             // Postgres disconnected
};

// Returns the fixed color set for a variant. The returned reference is to
// a static, program-lifetime value -- safe to keep around, no ownership.
const ThemeColors &themeColors(ThemeVariant variant);

// The variant most recently applied via applyTheme(), so call sites outside
// Theme.cpp (delegates, models, panes) can pick up the *current* theme's
// colors rather than being hardcoded to Light. Defaults to Light before
// applyTheme() has ever been called (e.g. in unit tests that construct
// widgets without a full app startup).
ThemeVariant currentThemeVariant();

// Applies Fusion style + palette + QSS for the given variant to the whole
// application, and records it as currentThemeVariant(). Safe to call once,
// early in main(), before any windows are constructed, and safe to call
// again later (e.g. from the "Dark Mode" menu toggle) to re-theme live.
// Also safe for headless CLI invocations (scan/build/match) that still
// construct a QApplication but never show a window -- it only sets
// style/palette/stylesheet properties on the QApplication object, which
// cost nothing if unused.
void applyTheme(QApplication &app, ThemeVariant variant);

} // namespace pl
