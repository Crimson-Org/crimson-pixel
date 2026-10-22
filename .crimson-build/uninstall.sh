#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Remove Crimson Pixel from the current user
set -eu
script_directory=$(cd "$(dirname "$0")" && pwd)
cd "$script_directory/.."

crimson_remove() {
	rm -rf "$1"
}

export PROGRAM="crimson-pixel"
export APPLICATION_ID="io.github.CrimsonOrg.CrimsonPixel"
export VERSION="0.1.0"
export PREFIX="${HOME}/.local"
export STD="c11"
export WARN="-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter"
export OPT="-O2 -g"
export PKGS="gtk4 libadwaita-1 libpanel-1 zlib"
export LIBS="-lm"
export DEFINES="-DAPPLICATION_VERSION=\"${VERSION}\""
export INCLUDES="-Isrc -Isubprojects/crimson-core/src"
printf '==> %s\n' 'remove ${PREFIX}/bin/${PROGRAM}'
crimson_remove "${PREFIX}/bin/${PROGRAM}"
printf '==> %s\n' 'remove ${PREFIX}/share/applications/${APPLICATION_ID}.desktop'
crimson_remove "${PREFIX}/share/applications/${APPLICATION_ID}.desktop"
printf '==> %s\n' 'remove ${PREFIX}/share/icons/hicolor/scalable/apps/${APPLICATION_ID}.svg'
crimson_remove "${PREFIX}/share/icons/hicolor/scalable/apps/${APPLICATION_ID}.svg"
printf '==> %s\n' 'remove ${PREFIX}/share/metainfo/${APPLICATION_ID}.metainfo.xml'
crimson_remove "${PREFIX}/share/metainfo/${APPLICATION_ID}.metainfo.xml"
