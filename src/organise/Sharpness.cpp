#include "organise/Sharpness.h"

#include "raw/RawPreview.h"

#include <QImageReader>

namespace pl::organise {
namespace {
constexpr int kWorkingEdge = 1000;
}

double sharpness(const QImage &source)
{
    if (source.isNull())
        return 0;
    QImage grey = source;
    if (std::max(grey.width(), grey.height()) > kWorkingEdge)
        grey = grey.scaled(kWorkingEdge, kWorkingEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    grey = grey.convertToFormat(QImage::Format_Grayscale8);
    // The middle half each way.
    const int x0 = grey.width() / 4, x1 = grey.width() * 3 / 4;
    const int y0 = grey.height() / 4, y1 = grey.height() * 3 / 4;
    if (x1 - x0 < 3 || y1 - y0 < 3)
        return 0;
    double sum = 0, sumSq = 0;
    qint64 n = 0;
    for (int y = std::max(1, y0); y < std::min(grey.height() - 1, y1); ++y) {
        const uchar *above = grey.constScanLine(y - 1);
        const uchar *row = grey.constScanLine(y);
        const uchar *below = grey.constScanLine(y + 1);
        for (int x = std::max(1, x0); x < std::min(grey.width() - 1, x1); ++x) {
            const double lap = double(above[x]) + below[x] + row[x - 1] + row[x + 1] - 4.0 * row[x];
            sum += lap;
            sumSq += lap * lap;
            ++n;
        }
    }
    if (n == 0)
        return 0;
    const double mean = sum / double(n);
    return sumSq / double(n) - mean * mean;
}

std::optional<double> sharpnessOfFile(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image;
    if (reader.canRead()) {
        const QSize full = reader.size();
        if (full.isValid() && std::max(full.width(), full.height()) > kWorkingEdge * 2)
            reader.setScaledSize(full.scaled(kWorkingEdge * 2, kWorkingEdge * 2, Qt::KeepAspectRatio));
        image = reader.read();
    }
    if (image.isNull() && raw::isAvailable())
        image = raw::extractPreview(path, kWorkingEdge);
    if (image.isNull())
        return std::nullopt;
    return sharpness(image);
}

} // namespace pl::organise
