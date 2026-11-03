#pragma once

#include <QDialog>
#include <QString>

class QColor;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTimer;

namespace pl {

class Application;

// Get Token: opens iNaturalist's token page in the browser (where the user is
// usually still signed in) and, for the next five minutes, takes the token
// from the clipboard as soon as it's copied there -- Ctrl+A, Ctrl+C on that
// page -- so nothing needs pasting. A small window, kept above the browser,
// says what it's doing all along: watching (and for how much longer), that
// what was copied isn't a token, that it has one and is checking it with
// iNaturalist, and whose account it's for. The token can be pasted into it
// too. The token is saved and announced through Application::setInatApiToken(),
// then cleared from the clipboard.
class InatTokenGetter : public QDialog
{
    Q_OBJECT

public:
    // Opens the page and starts watching -- again, if already watching,
    // in the window that's up.
    static void start(Application &app);

    // Set, and not past its expiry (when it says).
    static bool isUsable(const QString &token);
    // "Signed in until 3:40 pm tomorrow", "Your token has expired", or empty
    // when there is no token.
    static QString status(const QString &token);

    static constexpr int kWatchSeconds = 5 * 60;

private:
    explicit InatTokenGetter(Application &app);
    void openPage();
    // (Re)starts the five minutes; `waiting` is what the status says meanwhile.
    void watch(const QString &waiting = {});
    void tick();
    void clipboardChanged();
    // `text` copied or pasted: the token in it, or why there isn't one.
    void take(const QString &text, bool copied);
    void check(const QString &token);
    void say(const QString &text, const QColor &color, bool busy);

    Application &m_app;
    QTimer *m_tick = nullptr;
    int m_secondsLeft = 0;
    bool m_watching = false;
    QString m_waiting;   // what the status says while watching

    QLabel *m_status = nullptr;
    QProgressBar *m_busy = nullptr;
    QLineEdit *m_paste = nullptr;
    QPushButton *m_again = nullptr;
    QPushButton *m_close = nullptr;
};

} // namespace pl
