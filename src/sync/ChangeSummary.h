#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace pl::sync {

// One kind of change in the local catalogue since the last sync -- "Photo
// identifications", "Reference trees", ... -- with its counts and a readable
// line per changed item ("IMG_1234 → Diuris pardina (was: unidentified)").
struct ChangeKind
{
    QString key;            // stable id, e.g. "identifications"
    QString title;          // "Photo identifications"
    bool automatic = false; // derived / background data (cover photos, localities ...)
    int added = 0;
    int changed = 0;
    int removed = 0;
    QStringList items;      // capped; see moreItems
    int moreItems = 0;      // items beyond the cap

    int total() const { return added + changed + removed; }
};

struct ChangeSummary
{
    QList<ChangeKind> kinds;   // only kinds with changes, in display order
    int totalRows = 0;         // what a push would send

    bool isEmpty() const { return totalRows == 0; }
    int automaticRows() const;
    // "Photo identifications (12) · Reference trees (1) · + 40 automatic updates"
    QString headline() const;
};

// Compares the local catalogue (the main schema of `localConnection`) with
// the snapshot at `basePath` -- what was last pulled or pushed -- and
// describes every difference. `basePath` is attached for the duration.
std::optional<ChangeSummary> summarizeChanges(const QString &localConnection,
                                              const QString &basePath, QString *error = nullptr,
                                              int itemCap = 500);

} // namespace pl::sync
