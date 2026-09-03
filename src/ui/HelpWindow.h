#pragma once

#include <QWidget>

class QTextBrowser;

namespace pl {

// A small offline help browser over the bundled :/help/ pages.
class HelpWindow : public QWidget
{
    Q_OBJECT

public:
    explicit HelpWindow(QWidget *parent = nullptr);

    // Shows the window, raising it, and scrolls to `anchor` if given.
    void showPage(const QString &anchor = {});

private:
    QTextBrowser *m_browser;
};

} // namespace pl
