#!/bin/sh

set -xe

mkdir -p ./build

RAYLIB_VERSION="4.2.0" # Corresponds to raylib 4.5

if [ "$1" = "--dynamic" ] || [ "$1" = "-d" ]; then
    CFLAGS="-Wall -Wextra -Wpedantic -Ofast"
    LFLAGS="-ldl"
    CPPFLAGS="-DDYLIB"
    RAYLIB=""
    SRC="./src/main.c"
else
    CFLAGS="-Wall -Wextra -Wpedantic -Ofast"
    LFLAGS="-lm -lpthread"
    CPPFLAGS=""
    RAYLIB="$(pkg-config --libs --cflags "raylib = $RAYLIB_VERSION")"
    SRC="./src/main.c ./src/plug.c ./src/fft.c"
fi

# shellcheck disable=SC2086
cc -o ./build/musializer $SRC $CPPFLAGS $CFLAGS $RAYLIB $LFLAGS

# Tests
CFLAGS_TEST="-Wall -Wextra -Wpedantic -Ofast"
LFLAGS_TEST="-lm"

# shellcheck disable=SC2086
cc ./src/fft.c ./src/fft_test.c -o ./build/fft_test $CFLAGS_TEST $LFLAGS_TEST
