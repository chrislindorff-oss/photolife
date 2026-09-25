#include <QtTest>

#include "util/PathSanitize.h"

using namespace pl::util;

class TestPathSanitize : public QObject
{
    Q_OBJECT

private slots:
    void leavesAnOrdinaryNameUntouched();
    void replacesIllegalCharactersWithSpaces();
    void collapsesWhitespaceLeftBehind();
};

void TestPathSanitize::leavesAnOrdinaryNameUntouched()
{
    QCOMPARE(sanitizeFilenameComponent(QStringLiteral("Caladenia carnea")),
             QStringLiteral("Caladenia carnea"));
}

void TestPathSanitize::replacesIllegalCharactersWithSpaces()
{
    QCOMPARE(sanitizeFilenameComponent(QStringLiteral("sp. \"aff.\" carnea: 1/2 <test>|*?")),
             QStringLiteral("sp. aff. carnea 1 2 test"));
}

void TestPathSanitize::collapsesWhitespaceLeftBehind()
{
    QCOMPARE(sanitizeFilenameComponent(QStringLiteral("a///b")), QStringLiteral("a b"));
    QCOMPARE(sanitizeFilenameComponent(QStringLiteral("  padded  ")), QStringLiteral("padded"));
}

QTEST_MAIN(TestPathSanitize)
#include "tst_pathsanitize.moc"
