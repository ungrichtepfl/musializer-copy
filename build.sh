#!/bin/sh

set -xe

mkdir -p ./build

CFLAGS="-Wall -Werror -Wextra -Wpedantic -Ofast -ggdb"

if [ "$1" = "--dynamic" ] || [ "$1" = "-d" ]; then
    ./hot-reload.sh
    LFLAGS="-ldl"
    CPPFLAGS="-DDYLIB"
    RAYLIB="" # No raylib needed, will be handled by hot-reload.sh
    SRC="./src/main.c"
else
    LFLAGS="-lm -lpthread"
    CPPFLAGS=""
    if [ "$(uname -m)" = "x86_64" ]; then
        RAYLIB="-I ./raylib-5.5_linux_amd64/include/ -L./raylib-5.5_linux_amd64/lib -l:libraylib.a -ldl"
    else
        echo "You are not on a x86_64 machine, please install raylib 5.5 and make sure that pkg-config can find it."
        RAYLIB="$(pkg-config --libs --cflags "raylib")"
    fi
    SRC="./src/main.c ./src/musializer.c ./src/fft.c"
fi


# shellcheck disable=SC2086
cc -o ./build/musializer $SRC $CPPFLAGS $CFLAGS $RAYLIB $LFLAGS

# Tests
CFLAGS_TEST="-Wall -Wextra -Wpedantic -Ofast"
LFLAGS_TEST="-lm"

# shellcheck disable=SC2086
cc ./src/fft.c ./src/fft_test.c -o ./build/fft_test $CFLAGS_TEST $LFLAGS_TEST
