#include "ui/InatToolsView.h"

#include "app/Application.h"
#include "net/INatClient.h"
#include "settings/Settings.h"
#include "ui/InatComparePage.h"
#include "ui/InatDownloadPage.h"
#include "ui/InatTokenGetter.h"
#include "ui/InatUploadPage.h"
#include "ui/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

namespace pl {

InatAccountBar::InatAccountBar(Application &app, QWidget *parent)
    : QWidget(parent), m_app(app)
{
    m_username = new QLineEdit(this);
    m_username->setPlaceholderText(tr("iNaturalist username"));
    m_username->setText(m_app.settings().inatUsername());

    m_token = new QLineEdit(this);
    m_token->setPlaceholderText(tr("Personal API token (required for uploading)"));
    m_token->setText(m_app.settings().inatApiToken());
    m_token->setEchoMode(QLineEdit::Password);
    m_token->setToolTip(
        tr("Optional. Without one, geoprivacy-obscured species (common for rare orchids) come "
           "back with fuzzed coordinates even for your own observations. Get a token from "
           "inaturalist.org/users/api_token while logged in — it expires after about 24 hours, "
           "so you may need a fresh one each session. Get Token fetches it for you."));

    auto *getToken = new QPushButton(tr("Get Token…"), this);
    getToken->setToolTip(tr("Opens iNaturalist's token page in your browser. Sign in there if asked, "
                            "then press Ctrl+A and Ctrl+C — PhotoLife takes the token from what you "
                            "copied."));
    connect(getToken, &QPushButton::clicked, this, [this] { InatTokenGetter::start(m_app); });
    m_tokenStatus = new QLabel(this);
    QPalette pal = m_tokenStatus->palette();
    pal.setColor(QPalette::WindowText, themeColors(currentThemeVariant()).mutedText);
    m_tokenStatus->setPalette(pal);
    connect(&m_app, &Application::inatApiTokenChanged, this, [this](const QString &token) {
        m_token->setText(token);
        showTokenStatus();
    });
    connect(m_token, &QLineEdit::textChanged, this, [this] { showTokenStatus(); });
    showTokenStatus();

    // Saved as soon as either field is edited, not only when a tool runs:
    // the photo grids' "Attempt to fetch coordinates from iNat" reads them
    // straight from Settings.
    connect(m_username, &QLineEdit::editingFinished, this, &InatAccountBar::commit);
    connect(m_token, &QLineEdit::editingFinished, this, &InatAccountBar::commit);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QLabel(tr("iNaturalist account:"), this));
    layout->addWidget(m_username, 1);
    layout->addWidget(m_token, 1);
    layout->addWidget(getToken);
    layout->addWidget(m_tokenStatus);
}

void InatAccountBar::showTokenStatus()
{
    m_tokenStatus->setText(InatTokenGetter::status(m_token->text().trimmed()));
}

QString InatAccountBar::username() const
{
    return m_username->text().trimmed();
}

void InatAccountBar::commit()
{
    const QString token = m_token->text().trimmed();
    m_app.settings().setInatUsername(username());
    m_app.setInatApiToken(token);
    emit usernameCommitted(username());
}

void InatAccountBar::focusUsername()
{
    m_username->setFocus();
    m_username->selectAll();
}

InatToolsView::InatToolsView(Application &app, QWidget *parent)
    : QWidget(parent)
{
    m_account = new InatAccountBar(app, this);
    m_downloadPage = new InatDownloadPage(app, *m_account, this);
    m_comparePage = new InatComparePage(app, *m_account, this);
    m_uploadPage = new InatUploadPage(app, *m_account, this);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("inatTabs"));
    m_tabs->addTab(m_downloadPage, tr("Download My Observations"));
    m_tabs->addTab(m_comparePage, tr("Compare Species"));
    m_tabs->addTab(m_uploadPage, tr("Upload Observations"));

    // In a scroll area, so the view never asks for more width than the main
    // window has beside its docked panels: Qt 6.4 crashes laying out the
    // docks when the central widget's minimum doesn't fit. Too narrow, and
    // it scrolls instead.
    auto *content = new QWidget(this);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->addWidget(m_account);
    contentLayout->addWidget(m_tabs, 1);
    auto *scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

void InatToolsView::showDownloadPage()
{
    m_tabs->setCurrentWidget(m_downloadPage);
}

void InatToolsView::showComparePage()
{
    m_tabs->setCurrentWidget(m_comparePage);
}

void InatToolsView::showUploadPage()
{
    m_tabs->setCurrentWidget(m_uploadPage);
}

void InatToolsView::setTrees(const QList<std::pair<int, QString>> &trees, int activeProjectId)
{
    m_downloadPage->setTrees(trees, activeProjectId);
    m_comparePage->setTrees(trees, activeProjectId);
}

void InatToolsView::followActiveTree(int projectId)
{
    m_downloadPage->followActiveTree(projectId);
    m_comparePage->followActiveTree(projectId);
}

void InatToolsView::setTreeSelection(qint64 inatId, const QString &name)
{
    m_downloadPage->setTreeSelection(inatId, name);
    m_comparePage->setTreeSelection(inatId, name);
}

} // namespace pl
