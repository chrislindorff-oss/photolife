#include "util/PathSanitize.h"

namespace pl::util {

QString sanitizeFilenameComponent(const QString &text)
{
    static const QString kIllegal = QStringLiteral("<>:\"/\\|?*");
    QString out;
    out.reserve(text.size());
    for (const QChar &ch : text)
        out += (ch.unicode() < 0x20 || kIllegal.contains(ch)) ? QChar(u' ') : ch;
    return out.simplified();
}

} // namespace pl::util
