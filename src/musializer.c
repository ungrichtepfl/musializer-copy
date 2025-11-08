#include "musializer.h"
#include "fft.h"
#include <assert.h>
#include <errno.h>
#include <float.h>
#include <pthread.h>
#include <raylib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__) || defined(__wasm__) || defined(__wasm32__) ||     \
    defined(__wasm64__)
#define FOR_WASM 1
#else
#define FOR_WASM 0
#endif

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 450

#define FPS 60
#define FADE_OUT_TIME 2.0

// https://pages.mtu.edu/~suits/NoteFreqCalcs.html
#define EQUAL_TEMPERED_FACTOR 1.059463094359

#define ARRAY_LENGTH(x) (sizeof(x) / sizeof(x[0]))

static unsigned int CHANNELS = 2;

typedef struct Frames {
  // TODO: Check which is left and which is right
  float left;
  float right;
} Frames;

#define FRAME_BUFFER_CAPACITY (2 << 13)
static Frames FRAME_BUFFER[FRAME_BUFFER_CAPACITY] = {0};
static unsigned int FRAME_BUFFER_SIZE = 0;
#define FRAME_BUFFER_SIZE_BYTES (FRAME_BUFFER_SIZE * sizeof(Frames))
#define FRAME_BUFFER_UNINITIALIZED_ELEMS                                       \
  (((long)FRAME_BUFFER_CAPACITY - (long)FRAME_BUFFER_SIZE < 0)                 \
       ? 0                                                                     \
       : (FRAME_BUFFER_CAPACITY - FRAME_BUFFER_SIZE))
#define FRAME_BUFFER_UNINITIALIZED_BYTES                                       \
  (FRAME_BUFFER_UNINITIALIZED_ELEMS * sizeof(Frames))
#define FRAMES_TO_SAMPLES(frames) (frames * CHANNELS)
#define SAMPLES_TO_FRAMES(frames) (frames / CHANNELS)

static pthread_mutex_t BUFFER_LOCK;

#define SMOOTHED_AMPLITUDES_SIZE SCREEN_WIDTH
#define SHADOW_SIZE SCREEN_WIDTH

#define FFT_SIZE (2 * FRAME_BUFFER_CAPACITY)

#define DEFAULT_MAX_AMPLITUDE 0.01

#define max(a, b) (a > b ? a : b)

typedef struct MusicFiles {
  size_t count;
  size_t currentlyPlayed;
  char **paths;
} MusicFiles;

static Music MUSIC = {0};

typedef struct State {
  bool finished;
  bool reload;
  bool useWave;
  bool showHelpInfo;
  bool showHelp;
  bool audioInit;
  float timePlayedSeconds;
  float maxAmplitude;
  Vector2 windowPosition;
  MusicFiles musicFiles;
} State;

static State *STATE = NULL;

static void unloadMusicFiles(void) {
  if (STATE->musicFiles.count > 0) {
    for (size_t i = 0; i < STATE->musicFiles.count; ++i)
      free(STATE->musicFiles.paths[i]);

    free(STATE->musicFiles.paths);
    STATE->musicFiles.count = 0;
  }
}

static void loadMusicFiles() {
  FilePathList droppedFiles = LoadDroppedFiles();
  unloadMusicFiles();
  STATE->musicFiles.count = droppedFiles.count;
  STATE->musicFiles.currentlyPlayed = 0;

  printf("Music files in the queue:\n");
  STATE->musicFiles.paths = malloc(sizeof(char *) * droppedFiles.count);
  for (unsigned int i = 0; i < droppedFiles.count; ++i) {
    STATE->musicFiles.paths[i] =
        malloc(strlen(droppedFiles.paths[i]) + 1); // + 1 for null terminator
    strcpy(STATE->musicFiles.paths[i], droppedFiles.paths[i]);
    printf("%s\n", STATE->musicFiles.paths[i]);
  }
  UnloadDroppedFiles(droppedFiles);
}

typedef struct LinearColor_s {
  float r;
  float g;
  float b;
  float a;
} LinearColor;

static inline float color_u8_to_f32(const unsigned char x) { return x / 255.0; }

static inline unsigned char color_f32_to_u8(const float x) { return x * 255.0; }

static inline float to_linear(const unsigned char x) {
  const float f = color_u8_to_f32(x);
  if (f <= 0.04045)
    return f / 12.92;
  else
    return pow((f + 0.055) / 1.055, 2.4);
}

static inline LinearColor srgb_to_linear(const Color color) {
  return (LinearColor){
      .r = to_linear(color.r),
      .g = to_linear(color.g),
      .b = to_linear(color.b),
      .a = color_u8_to_f32(color.a),
  };
}

static inline unsigned char to_srgb(const float x) {
  const float f =
      (x <= 0.0031308) ? x * 12.92 : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
  return color_f32_to_u8(f);
}

static LinearColor lerpColor(const LinearColor color1, const LinearColor color2,
                             const float t) {
  const float vec1[] = {color1.r, color1.g, color1.b, color1.a};
  const float vec2[] = {color2.r, color2.g, color2.b, color2.a};
  float res[] = {0, 0, 0, 0};
  for (int i = 0; i < 4; i++)
    res[i] = vec1[i] + (vec2[i] - vec1[i]) * t;
  return (LinearColor){
      .r = res[0],
      .g = res[1],
      .b = res[2],
      .a = res[3],
  };
}

static inline Color linear_to_srgb(const LinearColor color) {
  return (Color){.r = to_srgb(color.r),
                 .g = to_srgb(color.g),
                 .b = to_srgb(color.b),
                 .a = color_f32_to_u8(color.a)};
}

static Color lerpColorGammaCorrected(const Color color1, const Color color2,
                                     const float t) {
  const LinearColor c1 = srgb_to_linear(color1);
  const LinearColor c2 = srgb_to_linear(color2);
  const LinearColor c = lerpColor(c1, c2, t);
  return linear_to_srgb(c);
}

#define RAINBOW_RED (Color){255, 0, 0, 255}
#define RAINBOW_ORANGE (Color){255, 127, 0, 255}
#define RAINBOW_YELLOW (Color){255, 255, 0, 255}
#define RAINBOW_GREEN (Color){0, 255, 0, 255}
#define RAINBOW_BLUE (Color){0, 0, 255, 255}
#define RAINBOW_PURPLE (Color){255, 0, 255, 255}

static Color nextRainbowColor(int i, int n, bool reversed) {

  if (reversed)
    i = n - 1 - i;

  const int buckets = n / 5;
  int start;
  int stop;
  Color startColor;
  Color stopColor;
  if (i < buckets) {
    start = 0;
    stop = buckets;
    startColor = RAINBOW_RED;
    stopColor = RAINBOW_ORANGE;
  } else if (i < 2 * buckets) {
    start = buckets;
    stop = 2 * buckets;
    startColor = RAINBOW_ORANGE;
    stopColor = RAINBOW_YELLOW;
  } else if (i < 3 * buckets) {
    start = 2 * buckets;
    stop = 3 * buckets;
    startColor = RAINBOW_YELLOW;
    stopColor = RAINBOW_GREEN;
  } else if (i < 4 * buckets) {
    start = 3 * buckets;
    stop = 4 * buckets;
    startColor = RAINBOW_GREEN;
    stopColor = RAINBOW_BLUE;
  } else {
    start = 4 * buckets;
    stop = n;
    startColor = RAINBOW_BLUE;
    stopColor = RAINBOW_PURPLE;
  }

  const float t = (float)(i - start) / (stop - start);
  return lerpColorGammaCorrected(startColor, stopColor, t);
}

static bool lockBuffer(void) {
  int err;
  // Locking mutex
  if ((err = pthread_mutex_lock(&BUFFER_LOCK)) != 0) {
    printf("WARNING: Could not lock mutex: %s.", strerror(err));
    return false;
  }
  return true;
}

static bool tryLockBuffer(void) {
  int err;
  if ((err = pthread_mutex_trylock(&BUFFER_LOCK)) != 0) {
    if (err != EBUSY) {
      fprintf(stderr, "WARNING: Failed trying to lock mutex: %s.",
              strerror(err));
    }
    return false;
  }
  return true;
}

static void unlockBuffer(void) {
  int err;
  // Unlock mutex
  if ((err = pthread_mutex_unlock(&BUFFER_LOCK)) != 0) {
    fprintf(stderr, "FATAL: Could not unlock mutex. %s. Quitting program.",
            strerror(err));
    exit(EXIT_FAILURE); // panic
  }
}

static inline float fmaxvf(const float x[], const size_t n) {
  assert(n > 0 && "Empty vector");
  float m = x[0];
  for (size_t i = 0; i < n; ++i) {
    m = max(m, x[i]);
  }
  return m;
}

static inline float cmaxvf(const float complex x[], const size_t n) {
  assert(n > 0 && "Empty vector");
  float m = cabsf(x[0]);
  for (size_t i = 0; i < n; ++i) {
    m = max(m, cabsf(x[i]));
  }
  return m;
}

static inline float hannWindow(float sample, unsigned int n, unsigned int N) {
  // Hann window: https://en.wikipedia.org/wiki/Window_function
  return 0.5 * (1 - cosf(2.0f * M_PI * n / N)) * sample;
}

static inline int nextFrequencyIndex(int k) {
  return (int)ceilf((float)k * EQUAL_TEMPERED_FACTOR);
}

static float SMOOTHED_AMPLITUDES[SMOOTHED_AMPLITUDES_SIZE] = {0};
static float SHADOWS[SHADOW_SIZE] = {0};

static void resetFilter(void) {
  for (int i = 0; i < max(SMOOTHED_AMPLITUDES_SIZE, SHADOW_SIZE); ++i) {
    if (i < SMOOTHED_AMPLITUDES_SIZE)
      SMOOTHED_AMPLITUDES[i] = 0.0f;
    if (i < SHADOW_SIZE)
      SHADOWS[i] = 0.0f;
  }
  STATE->maxAmplitude = DEFAULT_MAX_AMPLITUDE;
}

static void drawFrequency(void) {

  if (FRAME_BUFFER_SIZE == 0)
    return; // Nothing to draw

  if (!lockBuffer())
    return;

  float samples[FFT_SIZE] = {0};
  float complex frequencies[FFT_SIZE];
  assert(FFT_SIZE >= FRAME_BUFFER_SIZE && "You need to increase the FFT_SIZE");
  for (unsigned int i = 0; i < FRAME_BUFFER_SIZE; ++i) {
    samples[i] = hannWindow(FRAME_BUFFER[i].left, i,
                            FRAME_BUFFER_SIZE); // only take left channel
  }

  unlockBuffer();

  // Compute FFT
  fft(samples, frequencies, FFT_SIZE);

  int numFrequencyBuckets = 0;
  const int startIndex = 20;
  for (int k = startIndex, i = 0;
       k < FFT_SIZE / 2 && i < SMOOTHED_AMPLITUDES_SIZE;
       k = nextFrequencyIndex(k), ++i) {
    ++numFrequencyBuckets;
    float f = 0;
    int n = 0;
    for (int j = k; j < nextFrequencyIndex(k); ++j) {
      f += cabsf(frequencies[j]);
      ++n;
    }
    if (f > 0.0f && n != 0)
      f = logf(f / n);

    const float smoothFactor = 10.0f;
    const float shadowFactor = 0.7f;
    SMOOTHED_AMPLITUDES[i] +=
        (f - SMOOTHED_AMPLITUDES[i]) * smoothFactor * GetFrameTime();
    SHADOWS[i] += (f - SHADOWS[i]) * shadowFactor * GetFrameTime();

    if (SMOOTHED_AMPLITUDES[i] < 0.0f)
      SMOOTHED_AMPLITUDES[i] = 0.0f;

    if (SHADOWS[i] - SMOOTHED_AMPLITUDES[i] < 0.0f)
      SHADOWS[i] = SMOOTHED_AMPLITUDES[i];

    STATE->maxAmplitude = max(STATE->maxAmplitude, SMOOTHED_AMPLITUDES[i]);
  }

  const int w =
      numFrequencyBuckets > 0 ? SCREEN_WIDTH / numFrequencyBuckets : 1;

  assert(numFrequencyBuckets <= SMOOTHED_AMPLITUDES_SIZE &&
         numFrequencyBuckets <= SHADOW_SIZE &&
         "You need to increase the SMOOTHED_AMPLITUDES_SIZE and SHADOW_SIZE");

  for (int i = 0; i < numFrequencyBuckets; ++i) {
    const float f = SMOOTHED_AMPLITUDES[i];

    const bool reversed_rainbow = true;
    Color color = nextRainbowColor(i, numFrequencyBuckets, reversed_rainbow);

    const float t = f / STATE->maxAmplitude;
    const int h = (float)SCREEN_HEIGHT * t;
    const int x = w / 2 + i * w;

    const float tLine = t * t;

    const float minLineWidth = 3.0f;
    const float maxLineWidth = 5.5f;
    const float lineWidth =
        (maxLineWidth - minLineWidth) * tLine + minLineWidth;
    const float maxRadius = maxLineWidth;
    const float minRadius = lineWidth;
    const float radius =
        t < 0.0001 ? 0.0f : (maxRadius - minRadius) * tLine + minRadius;

    const float minAlpha = 0.4f;
    const float maxAlpha = 1.0f;
    const float tColor = sinf(t * M_PI / 2.0f);
    color = Fade(color, (maxAlpha - minAlpha) * tColor + minAlpha);

    const float shrinkFactor = 0.9f;

    DrawLineEx((Vector2){x, SCREEN_HEIGHT},
               (Vector2){x, SCREEN_HEIGHT - shrinkFactor * h + radius - 1},
               lineWidth, color);
    DrawCircle(x, SCREEN_HEIGHT - shrinkFactor * h, radius, color);

    const float fShadow = SHADOWS[i];

    const float tShadow = fShadow / STATE->maxAmplitude;
    const int hShadow = (float)SCREEN_HEIGHT * tShadow;

    const float maxShadowAlpha = 0.65f;
    Color colorShadow =
        Fade(color, (maxShadowAlpha - minAlpha) * tShadow + minAlpha);

    DrawLineEx((Vector2){x, SCREEN_HEIGHT},
               (Vector2){x, SCREEN_HEIGHT - shrinkFactor * hShadow}, lineWidth,
               colorShadow);
  }
}

static void drawWave(void) {
  if (FRAME_BUFFER_SIZE == 0)
    return; // Nothing to draw

  if (!lockBuffer())
    return;

  const long dx = 2;
  int previous_h = -1;
  for (long i = FRAME_BUFFER_SIZE - 1, j = 0, x = SCREEN_WIDTH - dx;
       i >= 0 && j < SMOOTHED_AMPLITUDES_SIZE && x >= 0;
       i -= dx, ++j, x -= dx) {

    float sleft = FRAME_BUFFER[i].left;
    const float smoothFactor = 1.2f;
    SMOOTHED_AMPLITUDES[j] +=
        (sleft - SMOOTHED_AMPLITUDES[j]) * smoothFactor * GetFrameTime();

    sleft = SMOOTHED_AMPLITUDES[j];

    STATE->maxAmplitude = max(STATE->maxAmplitude, sleft);
    sleft /= STATE->maxAmplitude;

    const bool reversed_rainbow = false;
    const Color color = nextRainbowColor(x, SCREEN_WIDTH, reversed_rainbow);
    const float lineWidth = 2.7f;
    const float shrinkFactor = 0.5f;
    const int diff = sleft * SCREEN_HEIGHT / 2;
    const int h = (float)SCREEN_HEIGHT / 2 + shrinkFactor * diff;
    if (j == 0) {
      previous_h = h;
      continue;
    }
    DrawLineEx((Vector2){x + dx, previous_h}, (Vector2){x, h}, lineWidth,
               color);
    previous_h = h;
  }

  unlockBuffer();
}

static void drawMusic(void) {
  if (FRAME_BUFFER_SIZE == 0) {
    return; // Nothing to draw
  }

  if (STATE->useWave) {
    drawWave();
  } else {
    drawFrequency();
  }
}

// NOTE: From raudio.c:1269 (LoadMusicStream) of raylib:
//  We are loading samples are 32bit float normalized data, so,
//  we configure the output audio stream to also use float 32bit
static void fillSampleBuffer(void *buffer, unsigned int frames) {
  if (frames == 0)
    return; // Nothing to do! TODO: Check if this even can happen.
  assert(CHANNELS == 2 && "Does only support music with 2 channels.");
  const float *samples = (float *)buffer;

  const unsigned int available_frames_to_fill =
      FRAME_BUFFER_CAPACITY - FRAME_BUFFER_SIZE;

  if (!tryLockBuffer())
    return;

  if (frames <= available_frames_to_fill) {
    memcpy(FRAME_BUFFER + FRAME_BUFFER_SIZE, samples,
           frames *
               sizeof(Frames)); // HACK: This only works for two channel audio
                                //  because Frames contains 2 floats.
    FRAME_BUFFER_SIZE += frames;
  } else if (frames >= FRAME_BUFFER_CAPACITY) {
    memcpy(FRAME_BUFFER, &samples[frames - FRAME_BUFFER_CAPACITY],
           FRAME_BUFFER_CAPACITY *
               sizeof(Frames)); // HACK: This only works for two channel audio
                                //  because Frames contains 2 floats.
    FRAME_BUFFER_SIZE = FRAME_BUFFER_CAPACITY;
  } else {
    assert(FRAME_BUFFER_CAPACITY < frames + FRAME_BUFFER_SIZE);
    const unsigned int diff =
        frames + FRAME_BUFFER_SIZE - FRAME_BUFFER_CAPACITY;
    memmove(FRAME_BUFFER, &FRAME_BUFFER[diff],
            (FRAME_BUFFER_SIZE - diff) *
                sizeof(Frames)); // HACK: This only works for two channel audio
                                 //  because Frames contains 2 floats.)
    memcpy(&FRAME_BUFFER[FRAME_BUFFER_SIZE - diff], samples,
           frames *
               sizeof(Frames)); // HACK: This only works for two channel audio
                                //  because Frames contains 2 floats.
                                //  FRAME_BUFFER_SIZE = FRAME_BUFFER_CAPACITY;
    FRAME_BUFFER_SIZE = FRAME_BUFFER_CAPACITY;
  }
  unlockBuffer();
}

static bool initInternal(void) {

  if (pthread_mutex_init(&BUFFER_LOCK, NULL) != 0) {
    printf("\n mutex init failed\n");
    return false;
  }
  SetConfigFlags(FLAG_MSAA_4X_HINT); // Enable anti-aliasing
  InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "musializer");
  // NOTE: Do not init audio here, only after the first music piece has been
  // loaded This is needed such that it will work in the browser as well
  // (annoying audio policies, you are not allowed to start audio before user
  // interaction on the page).
  STATE->audioInit = false;

  SetTargetFPS(FPS); // Set our game to run at 30 frames-per-second
  return true;
}

bool init(void) {

  STATE = malloc(sizeof(State));

  if (!initInternal()) {
    return false;
  }

  // No need to initialize MUSIC otherwise it segfaults
  STATE->finished = false;
  STATE->reload = false;
  STATE->timePlayedSeconds = 0.0f;
  STATE->maxAmplitude = DEFAULT_MAX_AMPLITUDE;

#if FOR_WASM
  STATE->windowPosition = (Vector2){0, 0};
#else
  STATE->windowPosition = GetWindowPosition();
#endif
  STATE->useWave = false;
  STATE->musicFiles = (MusicFiles){0, 0, NULL};
  STATE->showHelp = false;
  STATE->showHelpInfo = false;

  return true;
}

static void startMusic(void) {
  STATE->maxAmplitude =
      DEFAULT_MAX_AMPLITUDE; // Start as if they are normalized

  if (!lockBuffer()) {
    exit(EXIT_FAILURE); // TODO: pass error to state
  }
  FRAME_BUFFER_SIZE = 0;
  unlockBuffer();

  if (STATE->musicFiles.count > 0) {
    if (!STATE->audioInit) {
      // NOTE: Only initialize audio after a user interaction. Like this it will
      // also work in in the browser (annoying audio policies, you are not
      // allowed to start audio before user interaction on the page).
      InitAudioDevice();
      STATE->audioInit = true;
    }
    MUSIC = LoadMusicStream(
        STATE->musicFiles.paths[STATE->musicFiles.currentlyPlayed]);
    CHANNELS = MUSIC.stream.channels;
    printf("Frame count: %u\n", MUSIC.frameCount);
    printf("Sample rate: %u\n", MUSIC.stream.sampleRate);
    printf("Frame size: %u\n", MUSIC.frameCount);
    PlayMusicStream(MUSIC);
    SeekMusicStream(MUSIC, STATE->timePlayedSeconds);
    AttachAudioStreamProcessor(MUSIC.stream, fillSampleBuffer);
    resetFilter();
  }
}

bool resume(State *state) {
  STATE = state;

  if (!initInternal()) {
    return false;
  }

  STATE->reload = false;
  startMusic();

#if !FOR_WASM
  SetWindowPosition(STATE->windowPosition.x, STATE->windowPosition.y);
#endif
  return true;
}

State *getState(void) { return STATE; }

bool finished(void) { return STATE->finished; }

static void stopMusic(void) {
  if (IsMusicValid(MUSIC)) {
    DetachAudioStreamProcessor(MUSIC.stream, fillSampleBuffer);
    StopMusicStream(MUSIC);
    UnloadMusicStream(MUSIC);
    MUSIC = (Music){0};
  }
}

static void terminateInternal(void) {
  stopMusic();
  CloseAudioDevice();
  CloseWindow();
  pthread_mutex_destroy(&BUFFER_LOCK);
}

void terminate(void) {
  terminateInternal();

  unloadMusicFiles();
  free(STATE);
}

void pausePlugin(void) { terminateInternal(); }

bool reload() { return STATE->reload; }

static double TIC = -DBL_MAX;

#if FOR_WASM
#include <stdatomic.h>

atomic_int_fast8_t wasmStopGame = 0;

void sendStopGame(void) { wasmStopGame = 1; }

#endif // FOR_WASM

void update(void) {

  // Main game loop
  if (WindowShouldClose()) // Detect window close button or ESC key
  {
    STATE->finished = true;
    return;
  }

#if FOR_WASM
  if (wasmStopGame) {
    // Quit
    STATE->finished = true;
    return;
  }

#endif // FOR_WASM

#if !FOR_WASM
  if (IsKeyPressed(KEY_Q)) {
    // Quit
    STATE->finished = true;
    return;
  }
#endif // FOR_WASM

  // Update
  //----------------------------------------------------------------------------------
#if FOR_WASM
  STATE->windowPosition = (Vector2){0, 0};
#else
  STATE->windowPosition = GetWindowPosition();
#endif

  if (IsKeyPressed(KEY_R)) {
    // Reload plugins
    STATE->reload = true;
  }

  if (IsKeyPressed(KEY_W)) {
    STATE->useWave = !STATE->useWave;
    resetFilter();
  }

  if (IsFileDropped()) {
    stopMusic();
    loadMusicFiles();
    STATE->timePlayedSeconds = 0.0f;
    startMusic();
  }

  if (IsMusicValid(MUSIC)) {
    UpdateMusicStream(MUSIC); // Update music buffer with new stream data

    // Restart music playing (stop and play)
    if (IsKeyPressed(KEY_SPACE)) {
      StopMusicStream(MUSIC);
      resetFilter();
      PlayMusicStream(MUSIC);
    }

    // Pause/Resume music playing
    if (IsKeyPressed(KEY_P)) {

      if (IsMusicStreamPlaying(MUSIC)) {
        PauseMusicStream(MUSIC);
      } else {
        resetFilter();
        ResumeMusicStream(MUSIC);
      }
    }

    if (IsKeyPressed(KEY_LEFT)) {
      SeekMusicStream(MUSIC, GetMusicTimePlayed(MUSIC) - 5.0f);
      resetFilter();
    }

    if (IsKeyPressed(KEY_RIGHT)) {
      SeekMusicStream(MUSIC, GetMusicTimePlayed(MUSIC) + 5.0f);
      resetFilter();
    }

    STATE->timePlayedSeconds = GetMusicTimePlayed(MUSIC);
    if (STATE->timePlayedSeconds >=
        GetMusicTimeLength(MUSIC) - 0.1f) { // Otherwise it may repeat
      STATE->timePlayedSeconds = 0;
      if (++STATE->musicFiles.currentlyPlayed >= STATE->musicFiles.count)
        STATE->musicFiles.currentlyPlayed = 0;
      stopMusic();
      startMusic();
    }

    if (IsKeyPressed(KEY_S)) {
      stopMusic();
      unloadMusicFiles();
    }
  }

  //----------------------------------------------------------------------------------

  // Draw
  //----------------------------------------------------------------------------------
  BeginDrawing();

  ClearBackground(BLACK);

  if (IsMusicValid(MUSIC)) {

    drawMusic();

    if (STATE->showHelpInfo) {
      TIC = GetTime();
      STATE->showHelpInfo = false;
    }
    const double toc = GetTime();
    Color color = WHITE;
    if (toc - TIC < FADE_OUT_TIME) {
      color = Fade(color, 1.0f - (toc - TIC) / FADE_OUT_TIME);
      DrawText("TOGGLE 'H' FOR HELP", 290, 30, 20, color);
    }

    if (IsKeyPressed(KEY_H)) {
      STATE->showHelp = !STATE->showHelp;
    }

    if (STATE->showHelp) {
      DrawText("HIDE HELP:        'H'", 689, 20, 10, WHITE);
      DrawText("TOGGLE BETWEEN WAVE AND FREQUENCY:        'W'", 523, 40, 10,
               WHITE);
      DrawText("STOP PLAYING:        'S'", 669, 60, 10, WHITE);
      DrawText("PAUSE/RESUME PLAYING:        'P'", 612, 80, 10, WHITE);
      DrawText("RESTART PLAYING: 'SPACE'", 647, 100, 10, WHITE);
      DrawText("SEEK BACKWARDS:       '<-' ", 652, 120, 10, WHITE);
      DrawText("SEEK FORWARDS:       '->'", 659, 140, 10, WHITE);
#if !FOR_WASM
      DrawText("QUIT:        'Q'", 719, 160, 10, WHITE);
#endif
    }

  } else {
#if FOR_WASM
    DrawText("DRAG AND DROP MUSIC FILES", 230, 200, 20, WHITE);
    DrawText("(FOR EXAMPLE AN MP3 FILE)", 235, 225, 20, WHITE);
#else
    DrawText("DRAG AND DROP MUSIC FILES", 230, 175, 20, WHITE);
    DrawText("(FOR EXAMPLE AN MP3 FILE)", 235, 200, 20, WHITE);
    DrawText("OR PRESS 'Q' TO QUIT", 275, 225, 20, WHITE);
#endif
    STATE->showHelpInfo = true;
    STATE->showHelp = false;
  }

  EndDrawing();
  //----------------------------------------------------------------------------------
}

const exports_t exports = {init,        update, getState, reload,
                           pausePlugin, resume, finished, terminate};
