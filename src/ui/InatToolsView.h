#pragma once

#include <QList>
#include <QString>
#include <QWidget>

#include <utility>

class QLineEdit;
class QTabWidget;

namespace pl {

class Application;
class InatComparePage;
class InatDownloadPage;

// The iNaturalist account every iNaturalist tool acts as: a username and an
// optional personal API token (from inaturalist.org/users/api_token), kept in
// Settings. Shared by all the tools' tabs rather than repeated on each.
class InatAccountBar : public QWidget
{
    Q_OBJECT

public:
    explicit InatAccountBar(Application &app, QWidget *parent = nullptr);

    QString username() const;

    // Saves both fields to Settings and applies the token to the shared
    // INatClient. Tools call this before any request, so whatever is typed
    // right now is what gets used even if the field never lost focus.
    void commit();
    void focusUsername();

signals:
    // After commit(), with the saved username.
    void usernameCommitted(const QString &username);

private:
    Application &m_app;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_token = nullptr;
};

// The central "iNaturalist" view mode: the account bar above one tab per
// iNaturalist tool. None of these tools belong to a particular reference
// tree, which is why they live here rather than under the Reference Tree
// view's tabs.
class InatToolsView : public QWidget
{
    Q_OBJECT

public:
    explicit InatToolsView(Application &app, QWidget *parent = nullptr);

    InatAccountBar *accountBar() const { return m_account; }
    InatDownloadPage *downloadPage() const { return m_downloadPage; }
    InatComparePage *comparePage() const { return m_comparePage; }
    void showDownloadPage();
    void showComparePage();

    // Passed on to every tool that works with reference trees.
    void setTrees(const QList<std::pair<int, QString>> &trees, int activeProjectId);
    void followActiveTree(int projectId);
    void setTreeSelection(qint64 inatId, const QString &name);

private:
    InatAccountBar *m_account = nullptr;
    QTabWidget *m_tabs = nullptr;
    InatDownloadPage *m_downloadPage = nullptr;
    InatComparePage *m_comparePage = nullptr;
};

} // namespace pl
