#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Build Crimson Pixel into the project folder
set -eu
script_directory=$(cd "$(dirname "$0")" && pwd)
cd "$script_directory/.."

crimson_copy() {
	if [ -d "$1" ]; then
		mkdir -p "$2" && cp -R "$1/." "$2"
	else
		mkdir -p "$(dirname "$2")" && cp "$1" "$2"
	fi
}

crimson_move() {
	mkdir -p "$(dirname "$2")" && mv "$1" "$2"
}

crimson_mkdir() {
	mkdir -p "$1"
}

crimson_needs() {
	command -v "$1" >/dev/null 2>&1 || { printf '%s\n' "$1 is needed, install it with your package manager" >&2; return 1; }
}

crimson_pkg() {
	pkg-config --exists "$1" || { printf '%s\n' "The development files for $1 are missing, install them with your package manager" >&2; return 1; }
}

crimson_each() {
	crimson_rules=$(mktemp) || return 1
	crimson_make=$(mktemp) || return 1
	crimson_extra=
	for crimson_glob in $4; do
		for crimson_file in $crimson_glob; do
			[ -e "$crimson_file" ] && crimson_extra="$crimson_extra $crimson_file"
		done
	done
	crimson_all=
	crimson_sep=$(printf '\037')
	for crimson_from in $1; do
		[ -e "$crimson_from" ] || continue
		crimson_id=$(printf '%s' "${crimson_from%.*}" | tr '/.' '__')
		crimson_name=$(basename "${crimson_from%.*}")
		crimson_to=$(printf '%s' "$2" | sed -e "s${crimson_sep}{id}${crimson_sep}$crimson_id${crimson_sep}g" -e "s${crimson_sep}{name}${crimson_sep}$crimson_name${crimson_sep}g")
		crimson_command=$(printf '%s' "$3" | sed -e 's/\$/$$/g' -e "s${crimson_sep}{from}${crimson_sep}$crimson_from${crimson_sep}g" -e "s${crimson_sep}{to}${crimson_sep}$crimson_to${crimson_sep}g")
		crimson_all="$crimson_all $crimson_to"
		printf '%s: %s%s\n\t@mkdir -p "$(dir $@)"\n\t@echo "==> %s"\n\t%s\n\n' "$crimson_to" "$crimson_from" "$crimson_extra" "$crimson_from" "$crimson_command" >> "$crimson_rules"
	done
	{ printf 'all:%s\n\n' "$crimson_all"; cat "$crimson_rules"; } > "$crimson_make"
	crimson_status=0
	make -s -f "$crimson_make" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" all || crimson_status=$?
	rm -f "$crimson_make" "$crimson_rules"
	return "$crimson_status"
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
printf '==> %s\n' 'needs cc'
crimson_needs "cc"
printf '==> %s\n' 'needs make'
crimson_needs "make"
printf '==> %s\n' 'needs pkg-config'
crimson_needs "pkg-config"
printf '==> %s\n' 'needs glib-compile-resources'
crimson_needs "glib-compile-resources"
printf '==> %s\n' 'pkg gtk4 >= 4.14'
crimson_pkg "gtk4 >= 4.14"
printf '==> %s\n' 'pkg libadwaita-1 >= 1.6'
crimson_pkg "libadwaita-1 >= 1.6"
printf '==> %s\n' 'pkg libpanel-1'
crimson_pkg "libpanel-1"
printf '==> %s\n' 'pkg zlib'
crimson_pkg "zlib"
printf '==> %s\n' 'mkdir build/obj'
crimson_mkdir "build/obj"
printf '==> %s\n' 'glib-compile-resources --sourcedir data --target build/resources.c --generate-source data/crimson-pixel.gresource.xml'
glib-compile-resources --sourcedir data --target build/resources.c --generate-source data/crimson-pixel.gresource.xml
printf '==> %s\n' 'each src/*.c'
crimson_each "src/*.c" "build/obj/{id}.o" "cc -std=${STD} \$WARN \$OPT \$DEFINES \$INCLUDES \$(pkg-config --cflags \$PKGS | sed \"s/-I/-isystem /g\") -c {from} -o {to}" "src/*.h lang/*.h subprojects/crimson-core/src/common/*.h subprojects/crimson-build/src/build/*.h"
printf '==> %s\n' 'each subprojects/crimson-core/src/common/*.c'
crimson_each "subprojects/crimson-core/src/common/*.c" "build/obj/{id}.o" "cc -std=${STD} \$WARN \$OPT \$DEFINES \$INCLUDES \$(pkg-config --cflags \$PKGS | sed \"s/-I/-isystem /g\") -c {from} -o {to}" "src/*.h lang/*.h subprojects/crimson-core/src/common/*.h subprojects/crimson-build/src/build/*.h"
printf '==> %s\n' 'cc -std=gnu11 -O2 -g $(pkg-config --cflags gio-2.0) -c build/resources.c -o build/obj/resources.o'
cc -std=gnu11 -O2 -g $(pkg-config --cflags gio-2.0) -c build/resources.c -o build/obj/resources.o
printf '==> %s\n' 'cc -o build/${PROGRAM} build/obj/*.o $(pkg-config --libs $PKGS) $LIBS'
cc -o build/${PROGRAM} build/obj/*.o $(pkg-config --libs $PKGS) $LIBS
printf '==> %s\n' 'copy build/${PROGRAM} to .${PROGRAM}.new'
crimson_copy "build/${PROGRAM}" ".${PROGRAM}.new"
printf '==> %s\n' 'move .${PROGRAM}.new to ${PROGRAM}'
crimson_move ".${PROGRAM}.new" "${PROGRAM}"
printf '==> %s\n' 'mkdir logs'
crimson_mkdir "logs"
