#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Remove Crimson Pixel from the current user
set -eu
script_directory=$(cd "$(dirname "$0")" && pwd)
cd "$script_directory/.."

export PROGRAM="crimson-pixel"
export APPLICATION_ID="io.github.CrimsonOrg.CrimsonPixel"
export VERSION="0.1.0"
export PREFIX="${HOME}/.local"
export STD="c11"
export WARN="-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter"
export OPT="-O2 -g"
export GTK_MINOR="16"
export ADWAITA_MINOR="7"
export PKGS="gtk4 >= 4.${GTK_MINOR} libadwaita-1 >= 1.${ADWAITA_MINOR} libpanel-1 zlib"
export LIBS="-lm"
export DEFINES="-DAPPLICATION_VERSION=\"${VERSION}\""
export VERSION_CHECKS="-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_${GTK_MINOR} -DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_${GTK_MINOR} -DADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_${ADWAITA_MINOR} -DADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_${ADWAITA_MINOR}"
export INCLUDES="-Isrc -Isubprojects/crimson-core/src"
export HEADERS="src/*/*.h subprojects/crimson-core/src/*/*.h subprojects/crimson-core/src/*/*/*.h"
printf '==> %s\n' 'sh subprojects/crimson-core/scripts/uninstall.sh'
sh subprojects/crimson-core/scripts/uninstall.sh
