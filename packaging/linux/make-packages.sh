#!/usr/bin/env bash
#   This file is part of Ansel.
#   Copyright (C) 2026 Juscelino Be.
#
#   Ansel is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
# Build a .deb and an AppImage from an already configured & compiled tree.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_INSTALL_PREFIX=/usr -DBINARY_PACKAGE_BUILD=ON
#   ninja -C build
#   packaging/linux/make-packages.sh build out
#
# Needs: dpkg-dev (dpkg-shlibdeps), and for the AppImage linuxdeploy,
# linuxdeploy-plugin-gtk.sh and appimagetool in $TOOLS_DIR (default ./tools).

set -euo pipefail

SRC_DIR=$(cd "$(dirname "$0")/../.." && pwd -P)
BUILD_DIR=$(realpath "${1:-$SRC_DIR/build}")
OUT_DIR=$(realpath -m "${2:-$SRC_DIR/dist}")
TOOLS_DIR=$(realpath -m "${TOOLS_DIR:-$SRC_DIR/tools/appimage}")

PKG_NAME="${PKG_NAME:-ansel-revelar}"
ARCH="amd64"
GIT_DESC=$(git -C "$SRC_DIR" describe --tags --always 2>/dev/null || echo 0)
VERSION="${VERSION:-0.1.0+git$(date +%Y%m%d).$(git -C "$SRC_DIR" rev-parse --short HEAD 2>/dev/null || echo 0)}"

mkdir -p "$OUT_DIR"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "==> staging install (${GIT_DESC})"
DESTDIR="$WORK/deb" cmake --install "$BUILD_DIR" > "$WORK/install.log"

# ---------------------------------------------------------------------------
# .deb
# ---------------------------------------------------------------------------
echo "==> computing shared library dependencies"
mkdir -p "$WORK/shlib/debian"
cat > "$WORK/shlib/debian/control" <<EOF
Source: $PKG_NAME

Package: $PKG_NAME
Architecture: $ARCH
EOF
BINS=$(find "$WORK/deb/usr/bin" "$WORK/deb/usr/lib" -type f \( -perm -u+x -o -name '*.so*' \) -exec sh -c 'file -b "$1" | grep -q ELF' _ {} \; -print)
DEPS=$(cd "$WORK/shlib" && dpkg-shlibdeps -O --ignore-missing-info \
         -l"$WORK/deb/usr/lib/x86_64-linux-gnu/ansel" -l"$WORK/deb/usr/lib/ansel" \
         $BINS 2>/dev/null | sed -n 's/^shlibs:Depends=//p')

INSTALLED_KB=$(du -sk "$WORK/deb" | cut -f1)
mkdir -p "$WORK/deb/DEBIAN"
cat > "$WORK/deb/DEBIAN/control" <<EOF
Package: $PKG_NAME
Version: $VERSION
Section: graphics
Priority: optional
Architecture: $ARCH
Installed-Size: $INSTALLED_KB
Depends: $DEPS, iso-codes, liblensfun-data-v1 | liblensfun-data
Conflicts: ansel
Provides: ansel
Maintainer: Juscelino Be <noreply@github.com>
Homepage: https://github.com/juscelino25bezerra-art/ansel
Description: photo editor and catalogue with a classic dark desktop look
 Fork of Ansel (itself derived from darktable) with the "revelar"
 interface: dark flat panels, full-width modules and a catalogue-first
 workflow. Non-destructive RAW development, library, export.
EOF

DEB="$OUT_DIR/${PKG_NAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group -Zxz --build "$WORK/deb" "$DEB" > /dev/null
echo "==> $DEB"

# ---------------------------------------------------------------------------
# AppImage
# ---------------------------------------------------------------------------
if [ ! -x "$TOOLS_DIR/linuxdeploy" ]; then
  echo "==> no linuxdeploy in $TOOLS_DIR, skipping AppImage"
  exit 0
fi

echo "==> building AppImage"
APPDIR="$WORK/AppDir"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" > /dev/null

# lensfun database, if the host has one
if [ -d /usr/share/lensfun ]; then
  mkdir -p "$APPDIR/usr/share"
  cp -a /usr/share/lensfun "$APPDIR/usr/share/"
fi

DESKTOP=$(find "$APPDIR/usr/share/applications" -name '*.desktop' | head -1)
ICON=$(find "$APPDIR/usr/share/icons" -path '*scalable*' -name 'ansel*.svg' | head -1)

PLUGINS=(--plugin gtk)
cp "$TOOLS_DIR/linuxdeploy-plugin-gtk.sh" "$WORK/linuxdeploy-plugin-gtk.sh"
export PATH="$WORK:$TOOLS_DIR:$PATH"
export DEPLOY_GTK_VERSION=3
export APPIMAGE_EXTRACT_AND_RUN=1
export NO_STRIP=1

LIBDIRS=()
for d in "$APPDIR/usr/lib/x86_64-linux-gnu/ansel" "$APPDIR/usr/lib/ansel"; do
  [ -d "$d" ] && LIBDIRS+=(--library "$(find "$d" -maxdepth 1 -name 'libansel*.so*' | head -1)")
done

EXECS=()
for e in "$APPDIR"/usr/bin/*; do
  [ "$(basename "$e")" = ansel ] && continue
  file -b "$e" | grep -q ELF && EXECS+=(--executable "$e")
done

# IOP plugins are dlopen()ed, let linuxdeploy collect their deps too
DEPLOY_LIBS=()
while IFS= read -r so; do DEPLOY_LIBS+=(--deploy-deps-only "$so"); done < <(
  find "$APPDIR/usr/lib" -path '*ansel/plugins*' -name '*.so' 2>/dev/null)

(
  cd "$WORK"
  "$TOOLS_DIR/linuxdeploy" --appdir "$APPDIR" \
      --executable "$APPDIR/usr/bin/ansel" "${EXECS[@]}" "${LIBDIRS[@]}" \
      "${DEPLOY_LIBS[@]}" \
      --desktop-file "$DESKTOP" ${ICON:+--icon-file "$ICON"} \
      "${PLUGINS[@]}" > "$WORK/linuxdeploy.log" 2>&1 || { tail -40 "$WORK/linuxdeploy.log"; exit 1; }
)

APPIMG="$OUT_DIR/${PKG_NAME}-${VERSION}-x86_64.AppImage"
ARCH=x86_64 "$TOOLS_DIR/appimagetool" --no-appstream "$APPDIR" "$APPIMG" > "$WORK/appimagetool.log" 2>&1 \
  || { tail -30 "$WORK/appimagetool.log"; exit 1; }
echo "==> $APPIMG"
