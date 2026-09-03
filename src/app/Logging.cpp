#include "app/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
#include <QtGlobal>

#include <cstdio>

namespace pl::logging {
namespace {

QMutex g_mutex;
QFile g_logFile;
QtMessageHandler g_previousHandler = nullptr;

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "DEBUG";
    case QtInfoMsg:     return "INFO ";
    case QtWarningMsg:  return "WARN ";
    case QtCriticalMsg: return "ERROR";
    case QtFatalMsg:    return "FATAL";
    }
    return "?????";
}

void handler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const QString line = QStringLiteral("%1 %2 %3")
                             .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                                  QString::fromLatin1(levelName(type)),
                                  message);

    QMutexLocker lock(&g_mutex);

    std::fputs(line.toLocal8Bit().constData(), stderr);
    std::fputc('\n', stderr);

    if (g_logFile.isOpen()) {
        QTextStream stream(&g_logFile);
        stream << line << '\n';
        stream.flush();
    }

    if (g_previousHandler)
        g_previousHandler(type, context, message);

    if (type == QtFatalMsg) {
        g_logFile.flush();
        abort();
    }
}

} // namespace

void install(const QString &logDir)
{
    QMutexLocker lock(&g_mutex);
    if (g_logFile.isOpen())
        return;

    QDir().mkpath(logDir);
    const QString path = QDir(logDir).filePath(
        QStringLiteral("photolife-%1.log")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd"))));

    g_logFile.setFileName(path);
    g_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);

    g_previousHandler = qInstallMessageHandler(handler);
}

QString currentLogFile()
{
    QMutexLocker lock(&g_mutex);
    return g_logFile.isOpen() ? g_logFile.fileName() : QString();
}

} // namespace pl::logging
