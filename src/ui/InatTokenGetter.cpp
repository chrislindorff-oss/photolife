#include "ui/InatTokenGetter.h"

#include "app/Application.h"
#include "inat/InatToken.h"
#include "net/INatClient.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace pl {

namespace {

QPointer<InatTokenGetter> &current()
{
    static QPointer<InatTokenGetter> getter;
    return getter;
}

QString keys(QKeySequence::StandardKey key)
{
    return QKeySequence(key).toString(QKeySequence::NativeText);
}

} // namespace

void InatTokenGetter::start(Application &app)
{
    if (!current())
        current() = new InatTokenGetter(app);
    InatTokenGetter *getter = current();
    getter->openPage();
    getter->watch();
    getter->show();
    getter->raise();
}

InatTokenGetter::InatTokenGetter(Application &app)
    : QDialog(nullptr, Qt::Dialog | Qt::WindowStaysOnTopHint), m_app(app)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_QuitOnClose, false);   // open or not, closing PhotoLife quits it
    connect(&app, &QObject::destroyed, this, [this] { delete this; });
    setWindowTitle(tr("Get Your iNaturalist Token"));

    auto *steps = new QLabel(
        tr("1. iNaturalist's token page is open in your browser. Sign in there if it asks.<br>"
           "2. On that page press <b>%1</b>, then <b>%2</b>.<br><br>"
           "PhotoLife takes the token from what you copied: there's nothing to paste.")
            .arg(keys(QKeySequence::SelectAll), keys(QKeySequence::Copy)),
        this);
    steps->setWordWrap(true);

    m_busy = new QProgressBar(this);
    m_busy->setRange(0, 0);   // busy, for as long as it's watching
    m_busy->setTextVisible(false);
    m_busy->setMaximumHeight(6);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::RichText);

    m_paste = new QLineEdit(this);
    m_paste->setPlaceholderText(tr("Or paste the token (or the whole page) here"));
    m_paste->setEchoMode(QLineEdit::Password);
    m_paste->setClearButtonEnabled(true);
    connect(m_paste, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (!text.trimmed().isEmpty())
            take(text, false);
    });

    m_again = new QPushButton(tr("Open the Page Again"), this);
    m_again->setToolTip(tr("Opens iNaturalist's token page again, and watches for five more minutes."));
    connect(m_again, &QPushButton::clicked, this, [this] {
        openPage();
        watch();
    });
    m_again->setAutoDefault(false);   // Enter in the paste box mustn't open the page
    m_close = new QPushButton(tr("Cancel"), this);
    m_close->setAutoDefault(false);
    connect(m_close, &QPushButton::clicked, this, &QDialog::close);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_again);
    buttons->addStretch(1);
    buttons->addWidget(m_close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(steps);
    layout->addSpacing(8);
    layout->addWidget(m_busy);
    layout->addWidget(m_status);
    layout->addSpacing(8);
    layout->addWidget(m_paste);
    layout->addLayout(buttons);
    resize(440, sizeHint().height());

    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &InatTokenGetter::tick);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &InatTokenGetter::clipboardChanged);
    // Some desktops (Wayland) only tell an application about the clipboard
    // while it's in front: look again on coming back to it.
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
            clipboardChanged();
    });
}

void InatTokenGetter::openPage()
{
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(inat::kApiTokenPage)));
}

void InatTokenGetter::watch(const QString &waiting)
{
    m_watching = true;
    m_secondsLeft = kWatchSeconds;
    m_waiting = waiting.isEmpty() ? tr("Watching the clipboard for your token…") : waiting;
    m_close->setText(tr("Cancel"));
    m_tick->start();
    tick();
}

void InatTokenGetter::tick()
{
    if (!m_watching)
        return;
    if (m_secondsLeft <= 0) {
        m_watching = false;
        m_tick->stop();
        say(tr("Stopped watching the clipboard. Press <b>Open the Page Again</b> to try again, or paste the token "
                "below."),
             themeColors(currentThemeVariant()).warning, false);
        return;
    }
    say(QStringLiteral("%1 <span style=\"color:%2\">(%3:%4 left)</span>")
             .arg(m_waiting, themeColors(currentThemeVariant()).mutedText.name())
             .arg(m_secondsLeft / 60)
             .arg(m_secondsLeft % 60, 2, 10, QLatin1Char('0')),
         QColor(), true);
    --m_secondsLeft;
}

void InatTokenGetter::clipboardChanged()
{
    if (!m_watching)
        return;
    const QString text = QGuiApplication::clipboard()->text();
    if (!text.trimmed().isEmpty())
        take(text, true);
}

void InatTokenGetter::take(const QString &text, bool copied)
{
    const QString token = inat::extractApiToken(text);
    if (token.isEmpty()) {
        if (copied) {
            // Still watching: say so, and what to copy instead.
            m_waiting = QStringLiteral("<span style=\"color:%1\">%2</span> ")
                            .arg(themeColors(currentThemeVariant()).warning.name(),
                                 tr("What you copied isn't a token. On iNaturalist's page press <b>%1</b>, then "
                                    "<b>%2</b>, to copy the whole page.")
                                     .arg(keys(QKeySequence::SelectAll), keys(QKeySequence::Copy)))
                        + tr("Still watching…");
            tick();
        } else {
            say(tr("That isn't a token. Copy it from iNaturalist's page, or the whole page."),
                 themeColors(currentThemeVariant()).warning, m_watching);
        }
        return;
    }
    m_watching = false;
    m_tick->stop();
    m_app.setInatApiToken(token);
    // Not left lying about for whatever is pasted next.
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (copied && clipboard->text() == text)
        clipboard->clear();
    check(token);
}

void InatTokenGetter::check(const QString &token)
{
    say(tr("Got your token. Checking it with iNaturalist…"), QColor(), true);
    m_app.inat().setAccessToken(token);
    QPointer<InatTokenGetter> self(this);
    m_app.inat().fetchMe([self, token](net::Outcome<QString> out) {
        if (!self)
            return;
        const ThemeColors &colors = themeColors(currentThemeVariant());
        if (out.status == 401) {
            self->watch(QStringLiteral("<span style=\"color:%1\">%2</span>")
                            .arg(colors.danger.name(),
                                 tr("iNaturalist didn't accept that token (it may be an old one). Copy it again "
                                    "from the page."))
                        + QStringLiteral("<br>") + tr("Watching the clipboard…"));
            return;
        }
        self->m_close->setText(tr("Done"));
        self->m_close->setDefault(true);
        QApplication::alert(self);
        if (!out.ok()) {
            self->say(tr("Got your token and saved it, but couldn't reach iNaturalist to check it (%1).")
                           .arg(out.error.toHtmlEscaped()),
                       colors.warning, false);
            return;
        }
        self->say(tr("&#10003; Got it: you're <b>@%1</b> on iNaturalist. %2.").arg(out.value.toHtmlEscaped(), status(token)),
                   colors.positive, false);
        QTimer::singleShot(6000, self, &QDialog::close);
    });
}

void InatTokenGetter::say(const QString &text, const QColor &color, bool busy)
{
    m_status->setText(color.isValid() ? QStringLiteral("<span style=\"color:%1\">%2</span>").arg(color.name(), text)
                                      : text);
    m_busy->setVisible(busy);
}

bool InatTokenGetter::isUsable(const QString &token)
{
    if (token.isEmpty())
        return false;
    const std::optional<QDateTime> expiry = inat::apiTokenExpiry(token);
    return !expiry || *expiry > QDateTime::currentDateTimeUtc();
}

QString InatTokenGetter::status(const QString &token)
{
    if (token.isEmpty())
        return {};
    const std::optional<QDateTime> expiry = inat::apiTokenExpiry(token);
    if (!expiry)
        return tr("Token set");
    if (*expiry <= QDateTime::currentDateTimeUtc())
        return tr("Your token has expired");
    const QDateTime local = expiry->toLocalTime();
    const QString time = QLocale().toString(local.time(), QLocale::ShortFormat);
    const qint64 days = QDate::currentDate().daysTo(local.date());
    return days == 0   ? tr("Signed in until %1 today").arg(time)
           : days == 1 ? tr("Signed in until %1 tomorrow").arg(time)
                       : tr("Signed in until %1").arg(QLocale().toString(local, QLocale::ShortFormat));
}

} // namespace pl
