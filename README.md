# Musializer

> [!NOTE]
> This code (or maybe just hot reloading) does not yet work on Windows. Also, it
> has never been tested on a Mac. Feel free to add support.

Inspiration from [Tsoding the Legend](https://www.youtube.com/watch?v=Xdbk1Pr5WXU&list=PLpM-Dvs8t0Vak1rrE2NJn8XYEJ5M7-BqT).

## Raylib

Raylib version [5.5](https://github.com/raysan5/raylib/releases/tag/5.5) is needed.

See [raylib](https://www.raylib.com/) for installation.

For an `x86_64` linux machine, raylib is already included.

## Build & Run

Hot reloading **disabled** (for users):

```shell
./build.sh && ./build/musializer
```

Hot reloading **enabled** (for developers):

```shell
./build.sh -d && ./build/musializer
```

When hot reloading is enabled you can modify the `./src/musializer.c` file during
runtime and then load the modification by running:

```shell
./hot-reload.sh
```

You can also reload it manually by pressing `R`.

### Vim

You can add an autocommand in Vim to reload it when you save `musializer.c`:

```vimscript
autocmd BufWritePost musializer.c !./hot-reload.sh
```

### Build for WEB

To build for web run:

```shell
./build_web.sh
```

You need to have the [emscripten](https://emscripten.org/) toolchain installed.
The compilation was works using emcc version `4.0.2 (7591f1c5ea0adf6f4293cfba2995ee9700aa0d93)`.

> [!Warning]
> Somehow 4.0.19 does not work, maybe this is a bug and will be fixed in newer versions.

## Test files

Run `./build.sh` and `./build_inotify.sh` to compile the two test files `fft_test.c`
and `inotify_test.c`. The binaries can then be found in `./build/`
