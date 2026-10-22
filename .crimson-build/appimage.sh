#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Pack Crimson Pixel into an AppImage in dist
set -eu
script_directory=$(cd "$(dirname "$0")" && pwd)
cd "$script_directory/.."

crimson_needs() {
	command -v "$1" >/dev/null 2>&1 || { printf '%s\n' "$1 is needed, install it with your package manager" >&2; return 1; }
}

sh "$script_directory/build.sh"
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
printf '==> %s\n' 'needs quick-sharun'
crimson_needs "quick-sharun"
printf '==> %s\n' 'sh subprojects/crimson-core/scripts/appimage.sh'
sh subprojects/crimson-core/scripts/appimage.sh
