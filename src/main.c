
#include "musializer.h"

#ifdef DYLIB

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define LIB_MUSIALIZER_FILE_NAME "build/libmusializer.so"
#define LIB_MUSIALIZER_OBJECT_FILE_NAME "build/musializer.o"
MUSIALIZER *musializer = NULL;
void *lib_musializer = NULL;

bool unloadMusializer(void) {
  if (dlclose(lib_musializer) != 0) {
    fprintf(stderr, "Could not unload musializer: %s\n", dlerror());
    return false;
  }
  return true;
}

bool loadMusializer(void) {

  lib_musializer = dlopen(LIB_MUSIALIZER_FILE_NAME, RTLD_NOW);

  if (lib_musializer == NULL) {
    fprintf(stderr, "Could not load %s: %s\n", LIB_MUSIALIZER_FILE_NAME,
            dlerror());
    return false;
  }
  musializer = dlsym(lib_musializer, MUSIALIZER_SYM);

  if (musializer == NULL) {
    fprintf(stderr, "Could not load %s from %s: %s\n", MUSIALIZER_SYM,
            LIB_MUSIALIZER_FILE_NAME, dlerror());
    return false;
  }

  return true;
}

bool hotReload(void) {
  State *state = musializer->getState();
  musializer->pause();
  if (!unloadMusializer())
    return false;
  if (!loadMusializer())
    return false;
  if (!musializer->resume(state)) {
    fprintf(stderr, "Could not resume musializer\n");
    return false;
  }
  return true;
}

bool init_watches(int *fd_, int *wd_, struct pollfd *pfd_) {
  const int fd = inotify_init1(IN_NONBLOCK);
  if (fd == -1) {
    fprintf(stderr, "Could not initialize inotify: %s\n", strerror(errno));
    return false;
  }
  const int wd = inotify_add_watch(fd, LIB_MUSIALIZER_OBJECT_FILE_NAME,
                                   IN_MODIFY | IN_IGNORED);
  if (wd == -1) {
    fprintf(stderr, "Cannot watch '%s': %s\n", LIB_MUSIALIZER_OBJECT_FILE_NAME,
            strerror(errno));
    return false;
  }
  pfd_->fd = fd;
  pfd_->events = POLLIN;
  *fd_ = fd;
  *wd_ = wd;
  return true;
}

bool dynlibModified(bool *modified, int *fd, int *wd, struct pollfd *pfd) {

  const int poll_num = poll(pfd, 1, 1); // block for 1ms
  *modified = false;

  if (poll_num == -1) {
    if (errno != EINTR) {
      fprintf(stderr, "poll\n");
      return false;
    }
  }

  if (poll_num > 0) {
    printf("Poll number > 0\n");
    if (pfd->revents & POLLIN) {
      printf("Pollin event\n");
      /* Some systems cannot read integer variables if they are not
        properly aligned. On other systems, incorrect alignment may
        decrease performance. Hence, the buffer used for reading from
        the inotify file descriptor should have the same alignment as
        struct inotify_event. */
      char buf[4096]
          __attribute__((aligned(__alignof__(struct inotify_event))));
      ssize_t len;
      len = read(*fd, buf, sizeof(buf));
      if (len == -1 && errno != EAGAIN) {
        fprintf(stderr, "Could not read from file descriptor");
        return false;
      }

      if (len > 0) {
        printf("Data length: %zd\n", len);
        // Data available
        const struct inotify_event *event;
        for (char *ptr = buf; ptr < buf + len;
             ptr += sizeof(struct inotify_event) + event->len) {
          event = (const struct inotify_event *)ptr;

          printf("Iterating event mask %u\n", event->mask);

          if (event->mask & IN_IGNORED) {
            printf("File has been deleted. Reinit watches.\n");
            if (close(*fd) == -1) {
              fprintf(stderr, "close\n");
              return false;
            }
            usleep(200000); // Wait until file has been written
            if (!init_watches(fd, wd, pfd)) {
              return false;
            }
            *modified = true;
          } else if (event->mask & IN_MODIFY) {
            if (*wd == event->wd) {
              printf("File modified\n");
              *modified = true;
            }
          }
        }
      }
    }
  }
  return true;
}

#else

const MUSIALIZER *musializer = &exports;

#endif // DYLIB

int runGame(void) {

#ifdef DYLIB
  int fd = -1;
  int wd = -1;
  struct pollfd pfd = {0};
  if (!init_watches(&fd, &wd, &pfd)) {
    return 1;
  }

  if (!loadMusializer()) {
    return 1;
  }
#endif // DYLIB

  musializer->init();

  while (!musializer->finished()) {

#ifdef DYLIB
    bool dynlib_modified = false;

    if (!dynlibModified(&dynlib_modified, &fd, &wd, &pfd)) {
      return 1;
    }
    const bool musializer_modified = musializer->reload() | dynlib_modified;

    if (musializer_modified) {
      printf("Musializer file was modified!\n");
      if (!hotReload())
        return 1;
    }
#endif // DYLIB

    musializer->update();
  }
  musializer->terminate();

#ifdef DYLIB
  close(fd);
#endif // DYLIB

  return 0;
}

int main(void) { return runGame(); }
