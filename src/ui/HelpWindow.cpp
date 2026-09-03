#include "ui/HelpWindow.h"

#include "pl/Version.h"

#include <QAction>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QScreen>
#include <QTextBrowser>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {

HelpWindow::HelpWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("%1 Help").arg(QString::fromLatin1(kAppName)));

    m_browser = new QTextBrowser(this);
    m_browser->setOpenExternalLinks(false);
    m_browser->setSearchPaths({QStringLiteral(":/help")});
    m_browser->document()->setDefaultStyleSheet(QStringLiteral(
        "body { font-size: 10.5pt; }"
        "h1 { font-size: 17pt; }"
        "h2 { font-size: 13pt; margin-top: 18px; }"
        "tt, pre { font-family: monospace; }"
        "pre { background: palette(alternate-base); padding: 8px; }"
        "table { margin: 6px 0; }"
        "a { text-decoration: none; }"));

    // Open http(s) links in the system browser; keep qrc: navigation in-window.
    connect(m_browser, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.scheme().startsWith(QLatin1String("http"))) {
            QDesktopServices::openUrl(url);
        } else {
            m_browser->setSource(url);
        }
    });

    auto *bar = new QToolBar(this);
    QAction *back = bar->addAction(tr("Back"));
    QAction *forward = bar->addAction(tr("Forward"));
    QAction *home = bar->addAction(tr("Contents"));
    back->setShortcut(QKeySequence::Back);
    home->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Home));
    connect(back, &QAction::triggered, m_browser, &QTextBrowser::backward);
    connect(forward, &QAction::triggered, m_browser, &QTextBrowser::forward);
    connect(home, &QAction::triggered, this, [this] {
        m_browser->setSource(QUrl(QStringLiteral("qrc:/help/help.html")));
    });
    connect(m_browser, &QTextBrowser::backwardAvailable, back, &QAction::setEnabled);
    connect(m_browser, &QTextBrowser::forwardAvailable, forward, &QAction::setEnabled);
    back->setEnabled(false);
    forward->setEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(bar);
    layout->addWidget(m_browser, 1);

    m_browser->setSource(QUrl(QStringLiteral("qrc:/help/help.html")));

    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QSize s = screen->availableSize();
        resize(qMin(820, s.width() - 80), qMin(720, s.height() - 80));
    } else {
        resize(760, 640);
    }
}

void HelpWindow::showPage(const QString &anchor)
{
    show();
    raise();
    activateWindow();
    if (!anchor.isEmpty())
        m_browser->scrollToAnchor(anchor);
}

} // namespace pl
