#include "ui/ServerBar.h"

#include "ui/Theme.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QFontMetrics>
#include <QResizeEvent>
#include <QToolButton>

namespace pl {

ServerBar::ServerBar(QMenu *serverMenu, QWidget *parent) : QWidget(parent)
{
    m_badge = new QToolButton(this);
    m_badge->setObjectName(QStringLiteral("serverBadge"));
    m_badge->setToolTip(tr("Shared catalogue status — click for details"));
    m_badge->setCursor(Qt::PointingHandCursor);
    connect(m_badge, &QToolButton::clicked, this, &ServerBar::statusRequested);

    m_startEditing = button(tr("Start Editing"), tr("Take the edit lock so you can change the catalogue"));
    m_getLatest = button(tr("Get Latest"), tr("Check the server for a newer catalogue"));
    m_takeOver = button(tr("Take Over…"), tr("Take the edit lock from whoever holds it"));
    m_changes = button(tr("Changes"), tr("Review what you've changed since the last sync"));
    m_sync = button(tr("Sync"), tr("Send your changes to the server (you keep editing)"));
    m_finish = button(tr("Finish Editing"), tr("Send your changes and let others edit"));
    m_retry = button(tr("Retry"), tr("Try to reach the server again"));
    connect(m_startEditing, &QToolButton::clicked, this, &ServerBar::startEditingRequested);
    connect(m_getLatest, &QToolButton::clicked, this, &ServerBar::getLatestRequested);
    connect(m_takeOver, &QToolButton::clicked, this, &ServerBar::takeOverRequested);
    connect(m_changes, &QToolButton::clicked, this, &ServerBar::reviewChangesRequested);
    connect(m_sync, &QToolButton::clicked, this, &ServerBar::syncRequested);
    connect(m_finish, &QToolButton::clicked, this, &ServerBar::finishEditingRequested);
    connect(m_retry, &QToolButton::clicked, this, &ServerBar::retryRequested);

    m_menuButton = new QToolButton(this);
    m_menuButton->setText(QStringLiteral("▾"));
    m_menuButton->setToolTip(tr("All server commands"));
    m_menuButton->setAutoRaise(true);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    m_menuButton->setMenu(serverMenu);
    m_menuButton->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0; }"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 6, 0);
    layout->setSpacing(4);
    layout->addWidget(m_badge);
    for (QToolButton *b : {m_startEditing, m_getLatest, m_takeOver, m_changes, m_sync, m_finish,
                           m_retry})
        layout->addWidget(b);
    layout->addWidget(m_menuButton);
}

QToolButton *ServerBar::button(const QString &text, const QString &tip)
{
    auto *b = new QToolButton(this);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

QString ServerBar::modeText(const ServerView &v)
{
    if (v.running)
        return v.runningPhase.isEmpty() ? tr("SYNCING…") : tr("SYNCING — %1").arg(v.runningPhase);
    if (v.offline)
        return v.editing ? tr("EDITING OFFLINE · %n change(s)", nullptr, v.pending())
                         : tr("OFFLINE");
    if (v.editing)
        return v.pending() > 0 ? tr("EDITING · %n change(s)", nullptr, v.pending()) : tr("EDITING");
    if (v.lockedByOther())
        return tr("VIEWING — %1 is editing").arg(v.state->lockedBy);
    if (v.newerAvailable())
        return tr("NEWER VERSION AVAILABLE");
    return v.checked ? tr("VIEWING") : tr("VIEWING — checking server…");
}

QString ServerBar::photosText(const ServerView &v)
{
    if (!v.photosEnabled)
        return {};
    if (v.photosRunning)
        return v.photosPhase.isEmpty() ? tr("Photos: syncing…") : tr("Photos: %1").arg(v.photosPhase);
    if (!v.photosError.isEmpty())
        return tr("Photos: couldn't sync — %1").arg(v.photosError);
    if (v.photosStopped)
        return tr("Photos: stopped — Sync Photos carries on where it left off");
    if (!v.photosPlan)
        return tr("Photos: not checked yet");
    const libsync::LibraryPlan &p = *v.photosPlan;
    QStringList parts;
    if (const int down = int(p.toDownload.size() + p.conflicts.size()); down > 0)
        parts << tr("%Ln to download", nullptr, down);
    if (const int up = int(p.toUpload.size()); up > 0)
        parts << tr("%Ln to upload", nullptr, up);
    if (v.photosFailed > 0)
        parts << tr("%Ln couldn't be synced", nullptr, v.photosFailed);
    if (v.photosOnlineOnly > 0)
        parts << tr("%Ln online only", nullptr, v.photosOnlineOnly);
    return parts.isEmpty() ? tr("Photos: up to date") : tr("Photos: %1").arg(parts.join(QStringLiteral(", ")));
}

QColor ServerBar::modeColor(const ServerView &v)
{
    const ThemeColors &t = themeColors(currentThemeVariant());
    if (v.running)
        return t.primaryAccent;
    if (v.offline)
        return t.danger;
    if (v.editing)
        return t.positive;
    if (v.lockedByOther())
        return t.warning;
    if (v.newerAvailable())
        return t.primaryAccent;
    return t.mutedText;
}

void ServerBar::setView(const ServerView &v)
{
    const QColor color = modeColor(v);
    const QString text = QStringLiteral("⬤  ") + modeText(v);
    QFont bold = m_badge->font();
    bold.setBold(true);
    m_badge->setFont(bold);
    m_badge->setText(text);
    m_badge->setStyleSheet(QStringLiteral(
        "QToolButton#serverBadge { color: %1; border: 1px solid %1; border-radius: 9px; "
        "padding: 1px 10px; background: transparent; }")
                               .arg(color.name()));
    // Wide enough for the whole text: the menu bar sizes its corner once.
    m_badge->setMinimumWidth(QFontMetrics(bold).horizontalAdvance(text) + 28);

    const bool idle = !v.running;
    auto show = [idle](QToolButton *b, bool visible) {
        b->setVisible(visible);
        b->setEnabled(idle);
    };
    show(m_startEditing, !v.editing && !v.offline);
    show(m_getLatest, !v.editing && !v.offline);
    show(m_takeOver, !v.editing && !v.offline && v.lockedByOther());
    show(m_changes, v.editing);
    show(m_sync, v.editing && !v.offline);
    show(m_finish, v.editing && !v.offline);
    show(m_retry, v.offline);
    m_changes->setText(v.pending() > 0 ? tr("Changes (%L1)").arg(v.pending()) : tr("Changes"));
    // The obvious next step stands out.
    m_getLatest->setFont(v.newerAvailable() ? bold : font());
    m_sync->setFont(v.pending() > 0 ? bold : font());

    // The menu bar only lays out its corner widget when it's resized: nudge
    // it, so a longer badge or another button isn't clipped.
    adjustSize();
    for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
        if (w->parentWidget())
            w->adjustSize();
        if (auto *bar = qobject_cast<QMenuBar *>(w)) {
            QResizeEvent resize(bar->size(), bar->size());
            QApplication::sendEvent(bar, &resize);
            break;
        }
    }
}

} // namespace pl
