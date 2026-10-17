#pragma once

#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QPageLayout>
#include <QPageSize>
#include <QSizeF>
#include <QString>

namespace pl::collection {

// The choices behind a reference-photo contact sheet: a printable PDF grid of
// thumbnails, each captioned with its taxon's names. Set in
// ContactSheetDialog; everything but the title and subtitle (which follow the
// reference tree) is remembered in Settings via toJson()/fromJson().
struct ContactSheetOptions
{
    enum Credits { NoCredits, CreditsUnderPhoto, CreditsPage };

    QPageSize::PageSizeId pageSize = QPageSize::A4;
    QPageLayout::Orientation orientation = QPageLayout::Portrait;
    int columns = 5;              // thumbnails per row, kMinColumns..kMaxColumns
    bool cropSquare = true;       // crop each photo to a square, else fit it whole

    QString title;
    QString subtitle;
    bool showDate = true;
    bool showPageNumbers = true;
    bool showSpeciesCount = true;

    bool captionScientific = true;
    bool captionCommon = true;
    bool includeMissing = false;  // a placeholder box for taxa with no photo

    // Empty = one alphabetical grid; otherwise a rank ("family", "order",
    // "genus") whose ancestor names group the photos under section headings.
    QString groupRank;
    // With groups: true starts each group on a new row under a full-width
    // heading; false runs the photos on as one continuous, full grid, each
    // group marked by a small label above its photos.
    bool groupNewRow = true;

    // iNaturalist photos are mostly CC-licensed, which requires credit.
    Credits credits = CreditsPage;

    static constexpr int kMinColumns = 2;
    static constexpr int kMaxColumns = 10;

    QJsonObject toJson() const;   // leaves out title and subtitle
    static ContactSheetOptions fromJson(const QJsonObject &json);
};

// One photo on the sheet. A null `photo` is a taxon with no photo (drawn as a
// placeholder, or left out unless includeMissing). `group` is the taxon's
// ancestor name at the options' groupRank, empty if unknown.
struct ContactSheetEntry
{
    QString name;
    QString commonName;
    QString attribution;
    QString group;
    QImage photo;
};

// The grid an options set produces, in millimetres. Pure arithmetic on the
// page size and which header/caption lines are on -- so the dialog can show
// it live, before any photo is gathered.
struct ContactSheetLayout
{
    QSizeF page;           // whole page, oriented
    double margin = 0;
    double headerHeight = 0;
    double footerHeight = 0;
    double gutter = 0;
    double groupStrip = 0; // continuous grouping: a label strip above each thumbnail
    double thumb = 0;      // thumbnails are square boxes this wide
    double captionHeight = 0;
    double captionPt = 0;  // caption font size, scaled to the cell width
    int columns = 0;
    int rowsPerPage = 0;

    int perPage() const { return columns * rowsPerPage; }
    double cellHeight() const { return groupStrip + thumb + captionHeight; }
};

// How many lines each caption needs: a name too long for its cell even when
// shrunk wraps onto a second line. The writer measures the real names; the
// dialog's estimate assumes one line each.
struct ContactSheetCaptionLines
{
    int scientific = 1;
    int common = 1;
    int credit = 1;
};

ContactSheetLayout computeContactSheetLayout(const ContactSheetOptions &options,
                                             const ContactSheetCaptionLines &lines = {});

// Writes `entries` (any order -- they are sorted by group, then name) as a
// PDF to `path`. On success returns true and sets `*pageCount` when given;
// on failure returns false with `*error` set. GUI thread only (uses QPainter
// with fonts).
bool writeContactSheet(const QString &path, QList<ContactSheetEntry> entries,
                       const ContactSheetOptions &options, QString *error = nullptr,
                       int *pageCount = nullptr);

} // namespace pl::collection
