#!/bin/sh

set -xe

mkdir -p ./build

if [ "$(uname -m)" = "x86_64" ]; then
    RAYLIB_CFLAGS="-I ./raylib-5.5_linux_amd64/include/"
    RAYLIB_LFLAGS="-L./raylib-5.5_linux_amd64/lib -l:libraylib.so -Wl,-rpath=./raylib-5.5_linux_amd64/lib"
else
    echo "You are not on a x86_64 machine, please install raylib 5.5 and make sure that pkg-config can find it."
    RAYLIB_LFLAGS="$(pkg-config --libs "raylib")"
    RAYLIB_CFLAGS="$(pkg-config --flags "raylib")"
fi

CFLAGS="-Wall -Werror -Wextra -Wpedantic -Ofast -ggdb $RAYLIB_CFLAGS"
LFLAGS="-lpthread -lm $RAYLIB_LFLAGS"

# shellcheck disable=SC2086
cc -c -o ./build/musializer.o ./src/musializer.c $CFLAGS -fPIC

# shellcheck disable=SC2086
cc -o ./build/libmusializer.so ./build/musializer.o ./src/fft.c $CFLAGS $LFLAGS -shared
