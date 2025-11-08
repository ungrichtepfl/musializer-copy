#!/bin/bash

set -ex

rm -f build/{*.wasm,*.js,*.html}

mkdir -p build/

emcc -o build/musializer.js \
    ./src/main.c ./src/musializer.c ./src/fft.c \
    -Os -Wall \
    -lm -lpthread -ldl \
    -I ./raylib-5.5_wasm/include/ -L./raylib-5.5_wasm/lib -l:libraylib.a \
    -sUSE_GLFW=3 -sASYNCIFY -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createMusializer \
    -sEXPORTED_FUNCTIONS=_run_game,_send_stop_game \
    -sTOTAL_STACK=512mb -DPLATFORM_WEB \

    cp build/musializer.js build/musializer.wasm .

python3 -m http.server 3000
