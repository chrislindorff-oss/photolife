#include "collection/ContactSheetWriter.h"

#include <QCollator>
#include <QDate>
#include <QFontMetricsF>
#include <QLocale>
#include <QPainter>
#include <QPdfWriter>

#include <algorithm>
#include <cmath>

namespace pl::collection {
namespace {

// Fixed geometry, in millimetres.
constexpr double kMargin = 12.0;
constexpr double kGutter = 4.0;
constexpr double kTitleLine = 7.0;
constexpr double kSubtitleLine = 4.5;
constexpr double kHeaderGap = 4.0;     // below the header rule
constexpr double kFooter = 6.0;
constexpr double kCaptionGap = 1.2;    // between a thumbnail and its caption
constexpr double kHeadingHeight = 7.0; // a group's section heading
constexpr double kPtToMm = 25.4 / 72.0;
constexpr double kLineSpacing = 1.3;   // line height as a multiple of font size
constexpr int kResolution = 300;

constexpr double kTitlePt = 14.0;
constexpr double kSubtitlePt = 9.0;
constexpr double kHeaderMetaPt = 8.0;
constexpr double kFooterPt = 7.0;
constexpr double kHeadingPt = 10.0;
constexpr double kCreditsPt = 7.0;

// Caption fitting: a caption too wide for its cell first shrinks, down to
// kShrinkFloor of its size (never below kMinSinglePt); past that it wraps
// onto two lines instead, which may shrink down to kMinWrappedPt. Only text
// that still doesn't fit two lines at that size is elided.
constexpr double kShrinkFloor = 0.7;
constexpr double kMinSinglePt = 4.0;
constexpr double kMinWrappedPt = 3.5;
constexpr double kFitSlack = 0.97;     // headroom against rounding in the PDF

double lineMm(double pt) { return pt * kPtToMm * kLineSpacing; }
double creditPt(double captionPt) { return std::max(4.5, captionPt - 2.0); }
double groupLabelPt(double captionPt) { return std::clamp(captionPt, 5.5, 8.0); }

QString rankFromJson(const QString &rank)
{
    static const QStringList known = {QStringLiteral("family"), QStringLiteral("order"),
                                      QStringLiteral("genus")};
    return known.contains(rank) ? rank : QString();
}

// One thing placed on a page, in page millimetres.
struct Item
{
    enum Kind { Heading, Cell };
    Kind kind = Cell;
    QRectF rect;
    int entry = -1;        // Cell: index into the sorted entries
    QString text;          // Heading: the group name
    int creditNo = 0;      // Cell: number on the credits page, 0 = none
};

struct Page
{
    QList<Item> items;
    QList<int> creditLines;   // credits pages: indices into the credits list
    bool isCredits = false;
};

QFont makeFont(double pt, bool bold = false, bool italic = false)
{
    QFont f;
    f.setPointSizeF(pt);
    f.setBold(bold);
    f.setItalic(italic);
    return f;
}

// A caption sized to fit its cell: one line, or two, at `pt`.
struct Fitted
{
    double pt = 0;
    QStringList lines;
};

// Fits `text` into `width` device units, starting from `basePt`. Text width
// scales linearly with point size, so one measurement at `basePt` suffices.
Fitted fitText(const QString &text, QFont font, double basePt, double width,
               QPaintDevice *device, bool allowWrap)
{
    Fitted fit{basePt, {text}};
    if (text.isEmpty())
        return fit;
    font.setPointSizeF(basePt);
    const QFontMetricsF fm(font, device);
    const double full = fm.horizontalAdvance(text);
    if (full <= width)
        return fit;

    const double shrunk = basePt * width / full * kFitSlack;
    const double floorPt = std::max(kMinSinglePt, basePt * kShrinkFloor);
    if (shrunk >= floorPt || !allowWrap || !text.contains(QLatin1Char(' '))) {
        fit.pt = std::max(shrunk, kMinSinglePt);   // elided when drawn if still too wide
        return fit;
    }

    // Split at the space that best balances the two lines.
    double best = full;
    int split = -1;
    for (int i = text.indexOf(QLatin1Char(' ')); i >= 0;
         i = text.indexOf(QLatin1Char(' '), i + 1)) {
        const double widest = std::max(fm.horizontalAdvance(text.left(i)),
                                       fm.horizontalAdvance(text.mid(i + 1)));
        if (widest < best) {
            best = widest;
            split = i;
        }
    }
    fit.lines = {text.left(split), text.mid(split + 1)};
    fit.pt = std::clamp(basePt * width / best * kFitSlack, kMinWrappedPt, basePt);
    return fit;
}

} // namespace

QJsonObject ContactSheetOptions::toJson() const
{
    return {
        {QStringLiteral("pageSize"), int(pageSize)},
        {QStringLiteral("landscape"), orientation == QPageLayout::Landscape},
        {QStringLiteral("columns"), columns},
        {QStringLiteral("cropSquare"), cropSquare},
        {QStringLiteral("showDate"), showDate},
        {QStringLiteral("showPageNumbers"), showPageNumbers},
        {QStringLiteral("showSpeciesCount"), showSpeciesCount},
        {QStringLiteral("captionScientific"), captionScientific},
        {QStringLiteral("captionCommon"), captionCommon},
        {QStringLiteral("includeMissing"), includeMissing},
        {QStringLiteral("groupRank"), groupRank},
        {QStringLiteral("groupNewRow"), groupNewRow},
        {QStringLiteral("credits"), int(credits)},
    };
}

ContactSheetOptions ContactSheetOptions::fromJson(const QJsonObject &json)
{
    ContactSheetOptions o;
    const int size = json.value(QStringLiteral("pageSize")).toInt(int(o.pageSize));
    if (size == QPageSize::A4 || size == QPageSize::A3 || size == QPageSize::Letter)
        o.pageSize = QPageSize::PageSizeId(size);
    o.orientation = json.value(QStringLiteral("landscape")).toBool(false)
                        ? QPageLayout::Landscape
                        : QPageLayout::Portrait;
    o.columns = std::clamp(json.value(QStringLiteral("columns")).toInt(o.columns), kMinColumns,
                           kMaxColumns);
    o.cropSquare = json.value(QStringLiteral("cropSquare")).toBool(o.cropSquare);
    o.showDate = json.value(QStringLiteral("showDate")).toBool(o.showDate);
    o.showPageNumbers = json.value(QStringLiteral("showPageNumbers")).toBool(o.showPageNumbers);
    o.showSpeciesCount = json.value(QStringLiteral("showSpeciesCount")).toBool(o.showSpeciesCount);
    o.captionScientific =
        json.value(QStringLiteral("captionScientific")).toBool(o.captionScientific);
    o.captionCommon = json.value(QStringLiteral("captionCommon")).toBool(o.captionCommon);
    o.includeMissing = json.value(QStringLiteral("includeMissing")).toBool(o.includeMissing);
    o.groupRank = rankFromJson(json.value(QStringLiteral("groupRank")).toString());
    o.groupNewRow = json.value(QStringLiteral("groupNewRow")).toBool(o.groupNewRow);
    const int credits = json.value(QStringLiteral("credits")).toInt(int(o.credits));
    if (credits >= NoCredits && credits <= CreditsPage)
        o.credits = Credits(credits);
    return o;
}

ContactSheetLayout computeContactSheetLayout(const ContactSheetOptions &options,
                                             const ContactSheetCaptionLines &lines)
{
    ContactSheetLayout l;
    QSizeF page = QPageSize(options.pageSize).size(QPageSize::Millimeter);
    if (options.orientation == QPageLayout::Landscape)
        page.transpose();
    l.page = page;
    l.margin = kMargin;
    l.gutter = kGutter;
    l.columns = std::clamp(options.columns, ContactSheetOptions::kMinColumns,
                           ContactSheetOptions::kMaxColumns);

    const bool firstLine = !options.title.isEmpty() || options.showDate || options.showSpeciesCount;
    const bool secondLine = !options.subtitle.isEmpty();
    if (firstLine || secondLine)
        l.headerHeight = (firstLine ? kTitleLine : 0) + (secondLine ? kSubtitleLine : 0) + kHeaderGap;
    l.footerHeight = options.showPageNumbers ? kFooter : 0;

    const double contentW = page.width() - 2 * kMargin;
    const double contentH = page.height() - 2 * kMargin - l.headerHeight - l.footerHeight;
    l.thumb = (contentW - (l.columns - 1) * kGutter) / l.columns;

    // Caption text scales with the cell so narrow cells keep room for the
    // names and wide ones don't look sparse; fitText() shrinks or wraps any
    // name that is still too long.
    l.captionPt = std::clamp(l.thumb * 0.26, 5.0, 9.0);
    double caption = 0;
    if (options.captionScientific)
        caption += std::max(1, lines.scientific) * lineMm(l.captionPt);
    if (options.captionCommon)
        caption += std::max(1, lines.common) * lineMm(l.captionPt);
    if (options.credits == ContactSheetOptions::CreditsUnderPhoto)
        caption += std::max(1, lines.credit) * lineMm(creditPt(l.captionPt));
    l.captionHeight = caption > 0 ? caption + kCaptionGap : 0;

    if (!options.groupRank.isEmpty() && !options.groupNewRow)
        l.groupStrip = lineMm(groupLabelPt(l.captionPt)) + 0.6;

    l.rowsPerPage =
        std::max(1, int(std::floor((contentH + kGutter) / (l.cellHeight() + kGutter))));
    return l;
}

bool writeContactSheet(const QString &path, QList<ContactSheetEntry> entries,
                       const ContactSheetOptions &options, QString *error, int *pageCount)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (!options.includeMissing) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [](const ContactSheetEntry &e) { return e.photo.isNull(); }),
                      entries.end());
    }
    if (entries.isEmpty())
        return fail(QObject::tr("There are no photos to put on the contact sheet."));

    const bool grouped = !options.groupRank.isEmpty();
    const bool groupRows = grouped && options.groupNewRow;
    const QString other = QObject::tr("Other");
    if (grouped) {
        for (ContactSheetEntry &e : entries) {
            if (e.group.isEmpty())
                e.group = other;
        }
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(entries.begin(), entries.end(),
                     [&](const ContactSheetEntry &a, const ContactSheetEntry &b) {
                         if (grouped && a.group != b.group) {
                             // "Other" (no known ancestor) goes last.
                             if ((a.group == other) != (b.group == other))
                                 return b.group == other;
                             return collator.compare(a.group, b.group) < 0;
                         }
                         return collator.compare(a.name, b.name) < 0;
                     });

    QPdfWriter writer(path);
    writer.setResolution(kResolution);
    writer.setPageLayout(QPageLayout(QPageSize(options.pageSize), options.orientation,
                                     QMarginsF(0, 0, 0, 0)));
    writer.setTitle(options.title);
    writer.setCreator(QStringLiteral("PhotoLife"));

    const double u = kResolution / 25.4;   // device units per millimetre
    const auto dev = [u](const QRectF &mm) {
        return QRectF(mm.x() * u, mm.y() * u, mm.width() * u, mm.height() * u);
    };

    // --- fit every caption to the cell width -------------------------------
    // The cell width doesn't depend on caption lines, so a first layout gives
    // it; the real one then reserves a second line where any name needs it.
    const ContactSheetLayout base = computeContactSheetLayout(options);
    const double captionPt = base.captionPt;
    const double cellWidthDev = base.thumb * u;
    const QFont sciTemplate = makeFont(captionPt, false, true);
    const QFont commonTemplate = makeFont(captionPt);
    const QFont creditTemplate = makeFont(creditPt(captionPt));

    struct EntryFits
    {
        Fitted scientific, common, credit;
    };
    QList<EntryFits> fits;
    fits.reserve(entries.size());
    ContactSheetCaptionLines lines;
    for (const ContactSheetEntry &e : entries) {
        EntryFits f;
        const bool hasCommon = !e.commonName.isEmpty() && e.commonName != e.name;
        if (options.captionScientific)
            f.scientific = fitText(e.name, sciTemplate, captionPt, cellWidthDev, &writer, true);
        if (options.captionCommon) {
            // No common name: fall back to the scientific one if it isn't
            // already shown, so every photo stays identifiable.
            if (hasCommon)
                f.common = fitText(e.commonName, commonTemplate, captionPt, cellWidthDev,
                                   &writer, true);
            else if (!options.captionScientific)
                f.common = fitText(e.name, sciTemplate, captionPt, cellWidthDev, &writer, true);
        }
        if (options.credits == ContactSheetOptions::CreditsUnderPhoto && !e.photo.isNull())
            f.credit = fitText(e.attribution, creditTemplate, creditPt(captionPt), cellWidthDev,
                               &writer, true);
        lines.scientific = std::max(lines.scientific, int(f.scientific.lines.size()));
        lines.common = std::max(lines.common, int(f.common.lines.size()));
        lines.credit = std::max(lines.credit, int(f.credit.lines.size()));
        fits.append(f);
    }
    const ContactSheetLayout l = computeContactSheetLayout(options, lines);

    // --- dry run: place every heading and cell, then the credits lines -----
    const double left = l.margin;
    const double contentW = l.page.width() - 2 * l.margin;
    const double top = l.margin + l.headerHeight;
    const double bottom = l.page.height() - l.margin - l.footerHeight;
    const double cellW = l.thumb;
    const double cellH = l.cellHeight();
    constexpr double eps = 0.01;

    QList<Page> pages(1);
    QList<int> credits;   // entry indices, in credit-number order
    double y = top;
    int col = 0;
    QString group;
    bool firstGroup = true;
    const auto newPage = [&] {
        pages.append(Page());
        y = top;
        col = 0;
    };
    for (int i = 0; i < entries.size(); ++i) {
        const ContactSheetEntry &e = entries.at(i);
        if (groupRows && (firstGroup || e.group != group)) {
            if (col != 0) {
                y += cellH + l.gutter;
                col = 0;
            }
            // Keep a heading with at least one row of its photos.
            if (y + kHeadingHeight + cellH > bottom + eps && !pages.last().items.isEmpty())
                newPage();
            pages.last().items.append({Item::Heading, QRectF(left, y, contentW, kHeadingHeight),
                                       -1, e.group, 0});
            y += kHeadingHeight;
            group = e.group;
            firstGroup = false;
        }
        if (col == 0 && y + cellH > bottom + eps && !pages.last().items.isEmpty()) {
            newPage();
            if (groupRows) {
                pages.last().items.append({Item::Heading,
                                           QRectF(left, y, contentW, kHeadingHeight), -1,
                                           QObject::tr("%1 (continued)").arg(group), 0});
                y += kHeadingHeight;
            }
        }
        Item cell;
        cell.kind = Item::Cell;
        cell.rect = QRectF(left + col * (cellW + l.gutter), y, cellW, cellH);
        cell.entry = i;
        if (options.credits == ContactSheetOptions::CreditsPage && !e.photo.isNull()
            && !e.attribution.isEmpty()) {
            credits.append(i);
            cell.creditNo = int(credits.size());
        }
        pages.last().items.append(cell);
        if (++col == l.columns) {
            col = 0;
            y += cellH + l.gutter;
        }
    }

    const QFont creditsFont = makeFont(kCreditsPt);
    const QFont creditsHeadingFont = makeFont(kHeadingPt, true);
    const auto creditText = [&](int n) {
        const ContactSheetEntry &e = entries.at(credits.at(n));
        return QStringLiteral("%1.  %2 — %3").arg(n + 1).arg(e.name, e.attribution);
    };
    QList<double> creditHeights;   // millimetres, wrapped to the content width
    if (!credits.isEmpty()) {
        const QFontMetricsF fm(creditsFont, &writer);
        for (int n = 0; n < credits.size(); ++n) {
            const QRectF r = fm.boundingRect(QRectF(0, 0, contentW * u, 1e6),
                                             Qt::TextWordWrap, creditText(n));
            creditHeights.append(r.height() / u + 0.8);
        }
        newPage();
        pages.last().isCredits = true;
        y = top + kHeadingHeight;
        for (int n = 0; n < credits.size(); ++n) {
            if (y + creditHeights.at(n) > bottom + eps && !pages.last().creditLines.isEmpty()) {
                newPage();
                pages.last().isCredits = true;
            }
            pages.last().creditLines.append(n);
            y += creditHeights.at(n);
        }
    }

    // --- render ------------------------------------------------------------
    QPainter p;
    if (!p.begin(&writer))
        return fail(QObject::tr("Could not write \"%1\".").arg(path));
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    const QColor ink(0x20, 0x20, 0x20);
    const QColor muted(0x6a, 0x6a, 0x6a);
    const QColor rule(0xbb, 0xbb, 0xbb);
    const QColor tile(0xee, 0xee, 0xee);

    const QString date = options.showDate
                             ? QLocale().toString(QDate::currentDate(), QLocale::LongFormat)
                             : QString();
    QString meta;
    if (options.showSpeciesCount)
        meta = QObject::tr("%n species", nullptr, int(entries.size()));
    if (!date.isEmpty())
        meta = meta.isEmpty() ? date : meta + QStringLiteral("  ·  ") + date;

    const QFont titleFont = makeFont(kTitlePt, true);
    const QFont subtitleFont = makeFont(kSubtitlePt);
    const QFont metaFont = makeFont(kHeaderMetaPt);
    const QFont footerFont = makeFont(kFooterPt);
    const QFont headingFont = makeFont(kHeadingPt, true);
    const QFont groupFont = makeFont(groupLabelPt(captionPt), true);
    const QFont placeholderFont = makeFont(captionPt);
    const QFont badgeFont = makeFont(std::max(4.5, captionPt - 2.5), true);

    const auto drawText = [&](const QRectF &mm, const QFont &font, const QColor &color,
                              int flags, const QString &text, bool elide = true) {
        p.setFont(font);
        p.setPen(color);
        const QRectF r = dev(mm);
        const QString shown =
            elide ? QFontMetricsF(font, &writer).elidedText(text, Qt::ElideRight, r.width())
                  : text;
        p.drawText(r, flags, shown);
    };

    // A fitted caption in its slot: `slotLines` lines of `lineHeight` mm,
    // whatever size the text itself was fitted to.
    const auto drawFitted = [&](double x, double y, const Fitted &fit, QFont font,
                                const QColor &color, double lineHeight) {
        font.setPointSizeF(fit.pt);
        for (const QString &line : fit.lines) {
            drawText(QRectF(x, y, l.thumb, lineHeight), font, color,
                     Qt::AlignLeft | Qt::AlignVCenter, line);
            y += lineHeight;
        }
    };

    const auto drawPhoto = [&](const QRectF &box, const QImage &photo) {
        const QRectF r = dev(box);
        if (photo.isNull()) {
            p.fillRect(r, tile);
            drawText(box, placeholderFont, muted, Qt::AlignCenter, QObject::tr("No photo"));
            return;
        }
        if (options.cropSquare) {
            const int side = std::min(photo.width(), photo.height());
            const QRect src((photo.width() - side) / 2, (photo.height() - side) / 2, side, side);
            p.drawImage(r, photo, src);
        } else {
            p.fillRect(r, QColor(0xf4, 0xf4, 0xf4));
            QSizeF s = QSizeF(photo.size()).scaled(r.size(), Qt::KeepAspectRatio);
            const QRectF target(r.center().x() - s.width() / 2, r.center().y() - s.height() / 2,
                                s.width(), s.height());
            p.drawImage(target, photo);
        }
    };

    // Continuous grouping: label each run of a group's cells within a row,
    // ink where the group starts and muted where it carries on.
    const auto drawGroupLabels = [&](const QList<Item> &items) {
        for (int a = 0; a < items.size();) {
            const Item &first = items.at(a);
            int b = a + 1;
            while (b < items.size() && items.at(b).rect.y() == first.rect.y()
                   && entries.at(items.at(b).entry).group == entries.at(first.entry).group)
                ++b;
            const QString &name = entries.at(first.entry).group;
            const bool starts =
                first.entry == 0 || entries.at(first.entry - 1).group != name;
            const double runW = items.at(b - 1).rect.right() - first.rect.x();
            const Fitted fit =
                fitText(name, groupFont, groupFont.pointSizeF(), runW * u, &writer, false);
            QFont font = groupFont;
            font.setPointSizeF(fit.pt);
            drawText(QRectF(first.rect.x(), first.rect.y(), runW, l.groupStrip - 0.6), font,
                     starts ? ink : muted, Qt::AlignLeft | Qt::AlignVCenter, name);
            p.setPen(QPen(starts ? ink : rule, 0.25 * u));
            const double ruleY = (first.rect.y() + l.groupStrip - 0.4) * u;
            p.drawLine(QPointF(first.rect.x() * u, ruleY),
                       QPointF((first.rect.x() + runW) * u, ruleY));
            a = b;
        }
    };

    const int total = int(pages.size());
    for (int pi = 0; pi < total; ++pi) {
        if (pi > 0)
            writer.newPage();
        const Page &page = pages.at(pi);

        // Header
        if (l.headerHeight > 0) {
            double hy = l.margin;
            const bool firstLine =
                !options.title.isEmpty() || options.showDate || options.showSpeciesCount;
            if (firstLine) {
                const QRectF line(left, hy, contentW, kTitleLine);
                drawText(line, metaFont, muted, Qt::AlignRight | Qt::AlignBottom, meta);
                const double metaW =
                    QFontMetricsF(metaFont, &writer).horizontalAdvance(meta) / u + 4;
                drawText(QRectF(left, hy, std::max(10.0, contentW - metaW), kTitleLine),
                         titleFont, ink, Qt::AlignLeft | Qt::AlignBottom, options.title);
                hy += kTitleLine;
            }
            if (!options.subtitle.isEmpty()) {
                drawText(QRectF(left, hy, contentW, kSubtitleLine), subtitleFont, muted,
                         Qt::AlignLeft | Qt::AlignVCenter, options.subtitle);
                hy += kSubtitleLine;
            }
            p.setPen(QPen(rule, 0.25 * u));
            p.drawLine(dev(QRectF(left, hy + 1, contentW, 0)).topLeft(),
                       dev(QRectF(left, hy + 1, contentW, 0)).topRight());
        }

        // Footer
        if (options.showPageNumbers) {
            drawText(QRectF(left, l.page.height() - l.margin - kFooter + 1.5, contentW,
                            kFooter - 1.5),
                     footerFont, muted, Qt::AlignHCenter | Qt::AlignBottom,
                     QObject::tr("Page %1 of %2").arg(pi + 1).arg(total));
        }

        if (page.isCredits) {
            double cy = top;
            if (pi == 0 || !pages.at(pi - 1).isCredits) {
                drawText(QRectF(left, cy, contentW, kHeadingHeight), creditsHeadingFont, ink,
                         Qt::AlignLeft | Qt::AlignVCenter, QObject::tr("Photo credits"));
            }
            cy += kHeadingHeight;
            for (int n : page.creditLines) {
                drawText(QRectF(left, cy, contentW, creditHeights.at(n)), creditsFont, ink,
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, creditText(n), false);
                cy += creditHeights.at(n);
            }
            continue;
        }

        if (l.groupStrip > 0)
            drawGroupLabels(page.items);

        for (const Item &item : page.items) {
            if (item.kind == Item::Heading) {
                drawText(QRectF(item.rect.x(), item.rect.y(), item.rect.width(),
                                item.rect.height() - 1.5),
                         headingFont, ink, Qt::AlignLeft | Qt::AlignBottom, item.text);
                continue;
            }

            const ContactSheetEntry &e = entries.at(item.entry);
            const EntryFits &f = fits.at(item.entry);
            const QRectF box(item.rect.x(), item.rect.y() + l.groupStrip, l.thumb, l.thumb);
            drawPhoto(box, e.photo);

            if (item.creditNo > 0) {
                p.setFont(badgeFont);
                const QString label = QString::number(item.creditNo);
                const QFontMetricsF fm(badgeFont, &writer);
                const double pad = 0.6 * u;
                const QRectF badge(dev(box).topLeft() + QPointF(pad, pad),
                                   QSizeF(fm.horizontalAdvance(label) + 2 * pad,
                                          fm.height() + 0.4 * pad));
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 255, 255, 210));
                p.drawRoundedRect(badge, pad, pad);
                p.setPen(ink);
                p.drawText(badge, Qt::AlignCenter, label);
            }

            // Lines stack tightly; a row's spare second line (reserved for
            // some other long name) stays at the bottom of the caption.
            const double captionLine = lineMm(captionPt);
            double ty = box.bottom() + kCaptionGap;
            if (options.captionScientific) {
                drawFitted(box.x(), ty, f.scientific, sciTemplate, ink, captionLine);
                ty += f.scientific.lines.size() * captionLine;
            }
            if (options.captionCommon) {
                const bool hasCommon = !e.commonName.isEmpty() && e.commonName != e.name;
                drawFitted(box.x(), ty, f.common, hasCommon ? commonTemplate : sciTemplate, ink,
                           captionLine);
                ty += f.common.lines.size() * captionLine;
            }
            if (options.credits == ContactSheetOptions::CreditsUnderPhoto)
                drawFitted(box.x(), ty, f.credit, creditTemplate, muted,
                           lineMm(creditPt(captionPt)));
        }
    }

    if (!p.end())
        return fail(QObject::tr("Could not finish writing \"%1\".").arg(path));
    if (pageCount)
        *pageCount = total;
    return true;
}

} // namespace pl::collection
