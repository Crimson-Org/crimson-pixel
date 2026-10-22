#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Build and start Crimson Pixel
set -eu
script_directory=$(cd "$(dirname "$0")" && pwd)
cd "$script_directory/.."

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
printf '==> %s\n' './${PROGRAM}'
./${PROGRAM}
