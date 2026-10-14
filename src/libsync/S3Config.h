#pragma once

#include <QString>

namespace pl::libsync {

// Where an S3-compatible bucket is and how to sign in to it.
struct S3Config
{
    QString endpoint;                     // e.g. https://<account>.r2.cloudflarestorage.com
    QString region = QStringLiteral("auto");
    QString bucket;
    QString prefix;                       // optional folder within the bucket
    QString accessKey;
    QString secretKey;

    bool isComplete() const
    {
        return !endpoint.trimmed().isEmpty() && !bucket.trimmed().isEmpty()
               && !accessKey.trimmed().isEmpty() && !secretKey.isEmpty();
    }
};

} // namespace pl::libsync
