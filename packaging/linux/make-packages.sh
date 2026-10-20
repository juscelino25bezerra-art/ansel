#!/usr/bin/env bash
#   This file is part of Lumivell, a fork of Ansel.
#   Copyright (C) 2026 Juscelino Be.
#
#   Lumivell is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
# Build a .deb and an AppImage of Lumivell from an already configured & compiled tree.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_INSTALL_PREFIX=/opt/lumivell -DBINARY_PACKAGE_BUILD=ON
#   ninja -C build
#   packaging/linux/make-packages.sh build out
#
# Layout of the .deb: the program lives in /opt/lumivell (so it never collides with an
# Ansel or darktable installed from the distribution), and only what the desktop needs
# to see is exposed in /usr: the `lumivell` command, the menu entry, the icons and the
# AppStream metadata. Every runtime path is relative to the executable, so the same
# tree is relocated as-is into the AppImage.
#
# Needs: dpkg-dev (dpkg-shlibdeps), and for the AppImage linuxdeploy,
# linuxdeploy-plugin-gtk.sh and appimagetool in $TOOLS_DIR (default ./tools/appimage).

set -euo pipefail

SRC_DIR=$(cd "$(dirname "$0")/../.." && pwd -P)
BUILD_DIR=$(realpath "${1:-$SRC_DIR/build}")
OUT_DIR=$(realpath -m "${2:-$SRC_DIR/dist}")
TOOLS_DIR=$(realpath -m "${TOOLS_DIR:-$SRC_DIR/tools/appimage}")

PKG_NAME="${PKG_NAME:-lumivell}"
APP_ID="photos.lumivell.Lumivell"
ARCH="amd64"
VERSION="${VERSION:-0.1.0+git$(date +%Y%m%d).$(git -C "$SRC_DIR" rev-parse --short HEAD 2>/dev/null || echo 0)}"

PREFIX=$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$BUILD_DIR/CMakeCache.txt")
if [ "$PREFIX" != "/opt/lumivell" ]; then
  echo "error: configure the build with -DCMAKE_INSTALL_PREFIX=/opt/lumivell (found '$PREFIX')" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
ROOT="$WORK/deb"
OPT="$ROOT/opt/lumivell"

echo "==> staging install"
DESTDIR="$ROOT" cmake --install "$BUILD_DIR" > "$WORK/install.log"

# release packages ship without debug info
find "$OPT" -type f \( -perm -u+x -o -name '*.so*' \) -exec sh -c \
  'file -b "$1" | grep -q ELF && strip --strip-unneeded "$1"' _ {} \;

# expose the desktop integration in /usr
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/share/applications" "$ROOT/usr/share/metainfo" "$ROOT/usr/share/icons"
ln -s /opt/lumivell/bin/lumivell "$ROOT/usr/bin/lumivell"
sed -e 's|^Icon=.*|Icon=lumivell|' \
    "$OPT/share/applications/$APP_ID.desktop" > "$ROOT/usr/share/applications/$APP_ID.desktop"
rm -rf "$OPT/share/applications"
mv "$OPT/share/metainfo/$APP_ID.appdata.xml" "$ROOT/usr/share/metainfo/"
rmdir "$OPT/share/metainfo"
# the icons go where the icon theme looks for them: window, dialogs and splash use the name
mv "$OPT/share/icons/hicolor" "$ROOT/usr/share/icons/"
rmdir "$OPT/share/icons"
# the splash screen looks for its logo next to the data directory first
mkdir -p "$OPT/share/icons"
ln -s /usr/share/icons/hicolor "$OPT/share/icons/hicolor"

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
BINS=$(find "$OPT/bin" "$OPT/lib" -type f \( -perm -u+x -o -name '*.so*' \) \
         -exec sh -c 'file -b "$1" | grep -q ELF' _ {} \; -print)
LIBARGS=()
while IFS= read -r d; do LIBARGS+=("-l$d"); done < <(find "$OPT/lib" -type d)
DEPS=$(cd "$WORK/shlib" && dpkg-shlibdeps -O --ignore-missing-info "${LIBARGS[@]}" \
         $BINS 2>/dev/null | sed -n 's/^shlibs:Depends=//p')

INSTALLED_KB=$(du -sk "$ROOT" | cut -f1)
mkdir -p "$ROOT/DEBIAN"
cat > "$ROOT/DEBIAN/control" <<EOF
Package: $PKG_NAME
Version: $VERSION
Section: graphics
Priority: optional
Architecture: $ARCH
Installed-Size: $INSTALLED_KB
Depends: $DEPS, iso-codes, liblensfun-data-v1 | liblensfun-data
Maintainer: Juscelino Be <noreply@github.com>
Homepage: https://github.com/juscelino25bezerra-art/ansel
Description: photo editor and catalogue for the desktop
 Lumivell organizes your photos in a library and develops RAW files
 non-destructively, with dark flat panels and a catalogue-first workflow.
 It is the desktop companion of the Lumivell Android app.
 .
 Lumivell is based on Ansel by Aurélien Pierre, itself derived from
 darktable, and is distributed under the GNU GPL v3 or later.
EOF
cat > "$ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
  command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
  command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/share/applications || true
fi
EOF
chmod 0755 "$ROOT/DEBIAN/postinst"
cp "$ROOT/DEBIAN/postinst" "$ROOT/DEBIAN/postrm"
sed -i 's/"\$1" = "configure"/"$1" = "remove" ] || [ "$1" = "purge"/' "$ROOT/DEBIAN/postrm"

DEB="$OUT_DIR/${PKG_NAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$DEB" > /dev/null
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
mkdir -p "$APPDIR/usr/share"
# same tree, relocated from /opt/lumivell to the AppDir's usr/
cp -a "$OPT/bin" "$OPT/lib" "$APPDIR/usr/"
cp -a "$OPT/share/." "$APPDIR/usr/share/"
rm -f "$APPDIR/usr/share/icons/hicolor"
cp -a "$ROOT/usr/share/icons/hicolor" "$APPDIR/usr/share/icons/"
mkdir -p "$APPDIR/usr/share/applications" "$APPDIR/usr/share/metainfo"
sed -e 's|^Exec=.*|Exec=lumivell %U|' -e 's|^TryExec=.*|TryExec=lumivell|' -e 's|^Icon=.*|Icon=lumivell|' \
    "$ROOT/usr/share/applications/$APP_ID.desktop" > "$APPDIR/usr/share/applications/$APP_ID.desktop"
cp "$ROOT/usr/share/metainfo/$APP_ID.appdata.xml" "$APPDIR/usr/share/metainfo/"

# lensfun database, if the host has one
if [ -d /usr/share/lensfun ]; then
  cp -a /usr/share/lensfun "$APPDIR/usr/share/"
fi

PLUGINS=(--plugin gtk)
cp "$TOOLS_DIR/linuxdeploy-plugin-gtk.sh" "$WORK/linuxdeploy-plugin-gtk.sh"
export PATH="$WORK:$TOOLS_DIR:$PATH"
export DEPLOY_GTK_VERSION=3
export APPIMAGE_EXTRACT_AND_RUN=1
export NO_STRIP=1

LIBS=()
while IFS= read -r lib; do LIBS+=(--library "$lib"); done < <(
  find "$APPDIR/usr/lib" -maxdepth 2 -name 'libansel*.so*' -type f)

EXECS=()
for e in "$APPDIR"/usr/bin/*; do
  [ "$(basename "$e")" = lumivell ] && continue
  if file -b "$e" | grep -q ELF; then EXECS+=(--executable "$e"); fi
done

# IOP plugins are dlopen()ed, let linuxdeploy collect their deps too
DEPLOY_LIBS=()
while IFS= read -r so; do DEPLOY_LIBS+=(--deploy-deps-only "$so"); done < <(
  find "$APPDIR/usr/lib" -path '*ansel/plugins*' -name '*.so' 2>/dev/null)

(
  cd "$WORK"
  "$TOOLS_DIR/linuxdeploy" --appdir "$APPDIR" \
      --executable "$APPDIR/usr/bin/lumivell" "${EXECS[@]}" "${LIBS[@]}" \
      "${DEPLOY_LIBS[@]}" \
      --desktop-file "$APPDIR/usr/share/applications/$APP_ID.desktop" \
      --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/lumivell.svg" \
      "${PLUGINS[@]}" > "$WORK/linuxdeploy.log" 2>&1 || { tail -40 "$WORK/linuxdeploy.log"; exit 1; }
)

APPIMG="$OUT_DIR/Lumivell-${VERSION}-x86_64.AppImage"
ARCH=x86_64 "$TOOLS_DIR/appimagetool" --no-appstream "$APPDIR" "$APPIMG" > "$WORK/appimagetool.log" 2>&1 \
  || { tail -30 "$WORK/appimagetool.log"; exit 1; }
echo "==> $APPIMG"
