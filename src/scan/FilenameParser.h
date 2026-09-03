#pragma once

#include <QDate>
#include <QString>
#include <QStringList>

namespace pl::scan {

// The structured pieces PhotoLife reads out of a photo's file name. The library
// follows a consistent grammar:
//
//   <name>[ (organ/colour)][_organ] - <locality> <d-m-yyyy>[ (n)][_suffix].<ext>
//
// e.g. "Diuris pardina (bud) - 401 Fulbrooks Road, Dadswells Bridge 28-9-2020 (1)"
//   -> name "Diuris pardina", organs {"bud"},
//      locality "401 Fulbrooks Road, Dadswells Bridge", date 2020-09-28, seq 1
//
// Resolving `name` to an actual taxon is the matching engine's job; the parser
// only separates the fields and normalises punctuation.
struct ParsedFilename
{
    QString name;            // name portion, trimmed; may be empty
    QStringList organTags;   // lower-cased organ/phenology/colour tags, in order
    QString locality;        // free text between the separator and the date
    QDate capturedOn;        // invalid QDate when no d-m-yyyy was present
    int sequence = 0;        // the trailing "(n)" duplicate counter, 0 if absent

    bool operator==(const ParsedFilename &) const = default;
};

// Parses a file name *stem* (no extension, no directory). Never fails: whatever
// cannot be recognised is left in `name`.
ParsedFilename parseStem(const QString &stem);

// Convenience: strips any directory and the last extension, then parseStem().
ParsedFilename parseFileName(const QString &fileName);

// The grouping key for RAW + JPEG renditions of one shot: the file name with its
// final extension removed, otherwise untouched. Two files with the same stem in
// the same folder are two renditions of one capture.
QString captureBaseName(const QString &fileName);

// True for words PhotoLife treats as organ/phenology/colour tags rather than
// part of a name (used to disambiguate "Genus species_leaf" from the
// common-name pair "Jacky Winter_White-winged Triller").
bool isOrganTag(const QString &word);

} // namespace pl::scan
