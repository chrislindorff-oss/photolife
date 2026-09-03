#!/usr/bin/env bash
# Builds PhotoLife-<version>-x86_64.AppImage from a fresh Release build.
#
# Requires: a working CMake/Qt6 toolchain, curl, and FUSE (or run the
# resulting tools with --appimage-extract-and-run in a container).
#
#   packaging/linux/build-appimage.sh [build-dir]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build-appimage}"
APPDIR="$BUILD_DIR/AppDir"
TOOLS_DIR="$BUILD_DIR/tools"

VERSION="$(git -C "$REPO_ROOT" describe --tags --always --match 'v*' 2>/dev/null | sed 's/^v//')"
VERSION="${VERSION:-0.1.0}"

mkdir -p "$TOOLS_DIR"
fetch() {  # fetch <url> <dest>
    [ -x "$2" ] && return 0
    echo ">> downloading $(basename "$2")"
    curl -fL --retry 3 -o "$2" "$1"
    chmod +x "$2"
}
BASE="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
QTBASE="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous"
fetch "$BASE/linuxdeploy-x86_64.AppImage"            "$TOOLS_DIR/linuxdeploy"
fetch "$QTBASE/linuxdeploy-plugin-qt-x86_64.AppImage" "$TOOLS_DIR/linuxdeploy-plugin-qt"

echo ">> configuring"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPHOTOLIFE_BUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr

echo ">> building"
cmake --build "$BUILD_DIR"

echo ">> installing into AppDir"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR"

echo ">> bundling Qt and packing (version $VERSION)"
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
export VERSION
export OUTPUT="PhotoLife-${VERSION}-x86_64.AppImage"
export EXTRA_QT_PLUGINS="sqldrivers;imageformats;tls"

cd "$BUILD_DIR"
"$TOOLS_DIR/linuxdeploy" \
    --appdir "$APPDIR" \
    --plugin qt \
    --desktop-file "$APPDIR/usr/share/applications/photolife.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/photolife.svg" \
    --output appimage

echo ">> done: $BUILD_DIR/$OUTPUT"
