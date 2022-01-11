#!/bin/sh -e
CFLAGS="\
-pedantic -Wall -Wextra \
-Wno-implicit-fallthrough \
-Wno-missing-field-initializers \
-Wno-unused-parameter \
-Wfatal-errors -std=c99 \
-lavutil -lavformat -lavcodec -lavutil \
-lswscale -lswresample -lz -lm -lpthread -lasound \
-D_POSIX_C_SOURCE=200809L $CFLAGS"

OS="$(uname)"
: ${CC:=$(command -v cc)}
: ${PREFIX:=/usr/local}
case "$OS" in *BSD*) CFLAGS="$CFLAGS -D_BSD_SOURCE" ;; esac

run() {
	printf '%s\n' "$*"
	"$@"
}

install() {
	[ -x fvp ] || build
	run mkdir -p "$DESTDIR$PREFIX/bin/"
	run cp -f fvp "$DESTDIR$PREFIX/bin/fvp"
}

build() {
	run "$CC" "fvp.c" $CFLAGS -o fvp
}

if [ "$#" -gt 0 ]; then
	"$@"
else
	build
fi
