#pragma once

#include <QObject>
#include <QString>

class QTimer;

namespace pl {

class Application;

// Get Token: opens iNaturalist's token page in the browser (where the user is
// usually still signed in) and, for the next five minutes, takes the token
// from the clipboard as soon as it's copied there -- Ctrl+A, Ctrl+C on that
// page -- so nothing needs pasting. The token is saved and announced through
// Application::setInatApiToken(), then cleared from the clipboard.
class InatTokenGetter : public QObject
{
    Q_OBJECT

public:
    // Opens the page and starts watching (again, if already watching).
    static void start(Application &app);

    // Set, and not past its expiry (when it says).
    static bool isUsable(const QString &token);
    // "Signed in until 3:40 pm tomorrow", "Your token has expired", or empty
    // when there is no token.
    static QString status(const QString &token);

private:
    explicit InatTokenGetter(Application &app);
    void clipboardChanged();

    Application &m_app;
    QTimer *m_giveUp = nullptr;
};

} // namespace pl
