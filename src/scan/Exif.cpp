#include "scan/Exif.h"

#include <QFile>
#include <QIODevice>
#include <QtEndian>

namespace pl::scan {
namespace {

constexpr int kMaxScanBytes = 256 * 1024;

// EXIF field type sizes, indexed by the TIFF type code (1..12).
int typeSize(quint16 type)
{
    switch (type) {
    case 1: case 2: case 6: case 7: return 1;   // BYTE, ASCII, SBYTE, UNDEFINED
    case 3: case 8:                 return 2;   // SHORT, SSHORT
    case 4: case 9: case 11:        return 4;   // LONG, SLONG, FLOAT
    case 5: case 10: case 12:       return 8;   // RATIONAL, SRATIONAL, DOUBLE
    default:                        return 0;
    }
}

template <typename T>
T read(const QByteArray &d, int offset, bool bigEndian, bool *ok)
{
    if (offset < 0 || offset + int(sizeof(T)) > d.size()) {
        *ok = false;
        return T{};
    }
    const uchar *p = reinterpret_cast<const uchar *>(d.constData()) + offset;
    return bigEndian ? qFromBigEndian<T>(p) : qFromLittleEndian<T>(p);
}

QDateTime parseExifDateTime(const QString &s)
{
    // "YYYY:MM:DD HH:MM:SS", with a trailing NUL already stripped.
    QDateTime dt = QDateTime::fromString(s.trimmed(), QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    dt.setTimeSpec(Qt::LocalTime);
    return dt;
}

struct IfdReader
{
    const QByteArray &tiff;
    bool bigEndian = false;
    bool ok = true;

    QString ascii(int valueOffset, int count)
    {
        if (valueOffset < 0 || count <= 0 || valueOffset + count > tiff.size())
            return {};
        QByteArray raw = tiff.mid(valueOffset, count);
        const int nul = raw.indexOf('\0');
        if (nul >= 0)
            raw.truncate(nul);
        return QString::fromLatin1(raw).trimmed();
    }

    // Walks one IFD at `ifdOffset`. Invokes `onTag(tag, type, count, valuePos)`
    // for each entry, where valuePos is the absolute offset of the value (inline
    // for <=4 bytes, otherwise the pointed-to location). Returns the offset of
    // the next IFD, or 0.
    template <typename Fn>
    quint32 walk(quint32 ifdOffset, Fn onTag)
    {
        bool localOk = true;
        const quint16 count = read<quint16>(tiff, ifdOffset, bigEndian, &localOk);
        if (!localOk)
            return 0;

        int entry = int(ifdOffset) + 2;
        for (int i = 0; i < count; ++i, entry += 12) {
            const quint16 tag = read<quint16>(tiff, entry, bigEndian, &localOk);
            const quint16 type = read<quint16>(tiff, entry + 2, bigEndian, &localOk);
            const quint32 num = read<quint32>(tiff, entry + 4, bigEndian, &localOk);
            if (!localOk)
                break;

            const int bytes = typeSize(type) * int(num);
            int valuePos = entry + 8;
            if (bytes > 4) {
                valuePos = int(read<quint32>(tiff, entry + 8, bigEndian, &localOk));
                if (!localOk)
                    break;
            }
            onTag(tag, type, num, valuePos);
        }

        return read<quint32>(tiff, entry, bigEndian, &localOk);
    }
};

} // namespace

ExifData parseExifSegment(const QByteArray &tiff)
{
    ExifData out;
    if (tiff.size() < 8)
        return out;

    const QByteArray order = tiff.left(2);
    bool bigEndian;
    if (order == "II")
        bigEndian = false;
    else if (order == "MM")
        bigEndian = true;
    else
        return out;

    bool ok = true;
    if (read<quint16>(tiff, 2, bigEndian, &ok) != 0x002A || !ok)
        return out;

    const quint32 ifd0 = read<quint32>(tiff, 4, bigEndian, &ok);
    if (!ok || ifd0 >= quint32(tiff.size()))
        return out;

    IfdReader reader{tiff, bigEndian};
    quint32 exifIfd = 0;

    reader.walk(ifd0, [&](quint16 tag, quint16 type, quint32 count, int valuePos) {
        switch (tag) {
        case 0x010F:  // Make
            if (type == 2)
                out.cameraMake = reader.ascii(valuePos, int(count));
            break;
        case 0x0110:  // Model
            if (type == 2)
                out.cameraModel = reader.ascii(valuePos, int(count));
            break;
        case 0x8769:  // Exif IFD pointer
            exifIfd = read<quint32>(tiff, valuePos, bigEndian, &ok);
            break;
        default:
            break;
        }
    });

    if (exifIfd != 0 && exifIfd < quint32(tiff.size())) {
        reader.walk(exifIfd, [&](quint16 tag, quint16 type, quint32 count, int valuePos) {
            if ((tag == 0x9003 || tag == 0x0132) && type == 2 && !out.dateTimeOriginal.isValid()) {
                const QDateTime dt = parseExifDateTime(reader.ascii(valuePos, int(count)));
                if (dt.isValid())
                    out.dateTimeOriginal = dt;
            }
        });
    }

    return out;
}

ExifData readJpegExif(QIODevice &device)
{
    const QByteArray head = device.read(kMaxScanBytes);
    if (head.size() < 4 || quint8(head[0]) != 0xFF || quint8(head[1]) != 0xD8)
        return {};  // not a JPEG

    int pos = 2;
    while (pos + 4 <= head.size()) {
        if (quint8(head[pos]) != 0xFF)
            break;
        const quint8 marker = quint8(head[pos + 1]);
        if (marker == 0xD9 || marker == 0xDA)  // EOI / start of scan
            break;

        const int segLen = (quint8(head[pos + 2]) << 8) | quint8(head[pos + 3]);
        if (segLen < 2)
            break;
        const int segStart = pos + 4;
        const int segEnd = pos + 2 + segLen;

        if (marker == 0xE1 && segEnd <= head.size()) {  // APP1
            const QByteArray body = head.mid(segStart, segEnd - segStart);
            if (body.startsWith(QByteArray("Exif\0\0", 6)))
                return parseExifSegment(body.mid(6));
        }

        pos = segEnd;
    }

    return {};
}

ExifData readJpegExif(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return readJpegExif(file);
}

} // namespace pl::scan
