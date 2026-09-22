#pragma once

#include "lightroom/LightroomCatalogReader.h"

#include <QMetaType>
#include <QString>

#include <functional>

namespace pl::lightroom {

// Matches each Lightroom-catalogued photo to an already-scanned capture by
// its file path, resolves its keywords against the taxonomy cache (the same
// match::parseName + match::TaxonResolver pipeline MatchEngine and
// CandidateFinder already use for filenames/folder names), and records the
// results in capture_keyword_hint for MatchEngine to fold in later.
//
// Synchronous core -- see LightroomImporter for the QThread wrapper. Give it
// an already-open connection name.
class LightroomImportEngine
{
public:
    explicit LightroomImportEngine(QString connectionName);

    struct Stats
    {
        int photosMatched = 0;     // photo path found a capture
        int photosUnmatched = 0;   // photo path had no matching rendition
        int keywordsResolved = 0;  // distinct keywords that resolved to a taxon
        int hintsWritten = 0;      // capture_keyword_hint rows inserted/updated
        bool cancelled = false;
        QString error;

        bool ok() const { return error.isEmpty(); }
    };

    using CancelFn = std::function<bool()>;
    using ProgressFn = std::function<void(int done, int total)>;

    Stats import(const QList<LightroomPhotoKeywords> &photos, const CancelFn &cancel = {},
                const ProgressFn &progress = {});

private:
    QString m_connectionName;
};

} // namespace pl::lightroom

Q_DECLARE_METATYPE(pl::lightroom::LightroomImportEngine::Stats)
