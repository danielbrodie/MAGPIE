// Native-Windows replacement for linenoise: no terminal editing, plain stdio
// line reads. The Windows build only ever drives magpie over piped stdin
// (leavegen, autoplay), where line editing and history are irrelevant.
#ifdef _WIN32

#include "linenoise.h"

#include <stdio.h>
#include <stdlib.h>

char *linenoise(const char *prompt) {
  if (prompt && prompt[0] != '\0') {
    fputs(prompt, stdout);
    fflush(stdout);
  }
  size_t cap = 256;
  size_t len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf) {
    return NULL;
  }
  int ch;
  while ((ch = fgetc(stdin)) != EOF && ch != '\n') {
    if (len + 2 > cap) {
      cap *= 2;
      char *grown = (char *)realloc(buf, cap);
      if (!grown) {
        free(buf);
        return NULL;
      }
      buf = grown;
    }
    buf[len++] = (char)ch;
  }
  if (ch == EOF && len == 0) {
    free(buf);
    return NULL;
  }
  while (len > 0 && buf[len - 1] == '\r') {
    len--;
  }
  buf[len] = '\0';
  return buf;
}

void linenoiseFree(void *ptr) { free(ptr); }

int linenoiseHistoryAdd(const char *line) {
  (void)line;
  return 0;
}

int linenoiseHistorySetMaxLen(int len) {
  (void)len;
  return 0;
}

#endif // _WIN32
