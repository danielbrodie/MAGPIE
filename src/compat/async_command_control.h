#ifndef ASYNC_COMMAND_CONTROL_H
#define ASYNC_COMMAND_CONTROL_H

#include "../util/io_util.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/poll.h>
#include <unistd.h>
#endif

#ifdef _WIN32
// Native-Windows variant. The POSIX implementation polls {stdin, signal
// pipe} so the waiter wakes on either input or command completion without
// consuming stdin it wasn't granted. Windows has no poll(2) on anonymous
// pipes, and a blocking-read stub would RACE THE MAIN LOOP for stdin lines
// (observed: the startup async-mode 'set' command's waiter stealing the
// next piped command nondeterministically). So on Windows the waiter never
// reads stdin at all: it waits on the finished flag only. Degradation: no
// 'stop' input while a command is running -- irrelevant for the piped batch
// usage (leavegen/autoplay) this port exists for.
#include <stdatomic.h>
#include <synchapi.h>

typedef struct AsyncCommandControl {
  atomic_int finished;
} AsyncCommandControl;

static inline AsyncCommandControl *async_command_control_create(void) {
  AsyncCommandControl *acc =
      (AsyncCommandControl *)malloc_or_die(sizeof(AsyncCommandControl));
  atomic_init(&acc->finished, 0);
  return acc;
}

static inline void async_command_control_destroy(AsyncCommandControl *acc) {
  free(acc);
}

static inline void
async_command_control_send_finished_signal(AsyncCommandControl *acc) {
  atomic_store(&acc->finished, 1);
}

static inline char *async_command_control_wait_for_input_or_finished_signal(
    AsyncCommandControl *acc) {
  while (!atomic_load(&acc->finished)) {
    Sleep(10);
  }
  return NULL;
}
#else

typedef struct AsyncCommandControl {
  int pipefds[2];
  struct pollfd fds[2];
} AsyncCommandControl;

static inline AsyncCommandControl *async_command_control_create(void) {
  AsyncCommandControl *acc =
      (AsyncCommandControl *)malloc_or_die(sizeof(AsyncCommandControl));
  if (pipe(acc->pipefds) == -1) {
    perror("pipe");
    log_fatal("failed to create pipe for async command control");
  }
  acc->fds[0].fd = fileno(get_stream_in());
  acc->fds[0].events = POLLIN;
  acc->fds[1].fd = acc->pipefds[0];
  acc->fds[1].events = POLLIN;
  return acc;
}

static inline void async_command_control_destroy(AsyncCommandControl *acc) {
  close(acc->pipefds[0]);
  close(acc->pipefds[1]);
  free(acc);
}

static inline void
async_command_control_send_finished_signal(AsyncCommandControl *acc) {
  if (write(acc->pipefds[1], "x", 1) == -1) {
    log_fatal("failed to write to finished signal");
  }
}

// Returns an alloc'ed char* if there was input detected or NULL if the finished
// signal was detected
static inline char *async_command_control_wait_for_input_or_finished_signal(
    AsyncCommandControl *acc) {
  int ret = poll(acc->fds, 2, -1); // wait indefinitely
  if (ret == -1) {
    perror("poll");
    log_fatal("unexpected error while polling for input or finished signal");
  }
  char *input = NULL;
  if (acc->fds[0].revents) {
    input = read_line_from_stream_in();
  }
  return input;
}

#endif // _WIN32

#endif
