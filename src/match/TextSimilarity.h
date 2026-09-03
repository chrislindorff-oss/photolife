#pragma once

#include <QString>

namespace pl::match {

// Levenshtein edit distance between two strings.
int editDistance(const QString &a, const QString &b);

// 1 - editDistance / maxLen, in [0, 1]. Good for single-character typos in
// otherwise-identical strings ("asaparagus" vs "asparagus").
double editSimilarity(const QString &a, const QString &b);

// Jaccard overlap of the two strings' character trigrams (space-padded), in
// [0, 1]. Tolerant of word reordering and small edits.
double trigramSimilarity(const QString &a, const QString &b);

// max(editSimilarity, trigramSimilarity) — the score the resolver ranks on.
double nameSimilarity(const QString &a, const QString &b);

} // namespace pl::match
