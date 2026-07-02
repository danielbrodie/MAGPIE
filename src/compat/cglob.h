#ifndef COMPAT_CGLOB_H
#define COMPAT_CGLOB_H

#ifndef _WIN32
#include <glob.h>
#else
// Minimal glob() replacement for native-Windows builds, covering the one
// pattern shape data_filepaths.c builds: "<dir>/<prefix>*<suffix>" (exactly
// one '*', always in the basename). Anything else returns no matches.
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t gl_pathc;
  char **gl_pathv;
} glob_t;

// Only the return-value contract data_filepaths.c relies on: 0 when at least
// one path matched, nonzero otherwise.
static inline int glob(const char *pattern, int flags, void *errfunc,
                       glob_t *pglob) {
  (void)flags;
  (void)errfunc;
  pglob->gl_pathc = 0;
  pglob->gl_pathv = NULL;

  const char *base = strrchr(pattern, '/');
  const char *base_backslash = strrchr(pattern, '\\');
  if (base_backslash > base) {
    base = base_backslash;
  }
  const size_t dir_len = base ? (size_t)(base - pattern) : 0;
  base = base ? base + 1 : pattern;

  const char *star = strchr(base, '*');
  if (!star || strchr(star + 1, '*')) {
    return 3;
  }
  const size_t prefix_len = (size_t)(star - base);
  const char *suffix = star + 1;
  const size_t suffix_len = strlen(suffix);

  char dir[4096];
  if (dir_len == 0) {
    dir[0] = '.';
    dir[1] = '\0';
  } else {
    if (dir_len >= sizeof(dir)) {
      return 3;
    }
    memcpy(dir, pattern, dir_len);
    dir[dir_len] = '\0';
  }

  DIR *dp = opendir(dir);
  if (!dp) {
    return 3;
  }
  size_t cap = 8;
  pglob->gl_pathv = (char **)malloc(cap * sizeof(char *));
  if (!pglob->gl_pathv) {
    closedir(dp);
    return 3;
  }
  const struct dirent *ent;
  while ((ent = readdir(dp)) != NULL) {
    const char *name = ent->d_name;
    const size_t name_len = strlen(name);
    if (name_len < prefix_len + suffix_len) {
      continue;
    }
    if (strncmp(name, base, prefix_len) != 0) {
      continue;
    }
    if (strcmp(name + name_len - suffix_len, suffix) != 0) {
      continue;
    }
    if (pglob->gl_pathc + 1 > cap) {
      cap *= 2;
      char **grown = (char **)realloc(pglob->gl_pathv, cap * sizeof(char *));
      if (!grown) {
        break;
      }
      pglob->gl_pathv = grown;
    }
    const size_t full_len = dir_len ? dir_len + 1 + name_len : name_len;
    char *full = (char *)malloc(full_len + 1);
    if (!full) {
      break;
    }
    if (dir_len) {
      memcpy(full, pattern, dir_len);
      full[dir_len] = '/';
      memcpy(full + dir_len + 1, name, name_len + 1);
    } else {
      memcpy(full, name, name_len + 1);
    }
    pglob->gl_pathv[pglob->gl_pathc++] = full;
  }
  closedir(dp);
  if (pglob->gl_pathc == 0) {
    free(pglob->gl_pathv);
    pglob->gl_pathv = NULL;
    return 3;
  }
  return 0;
}

static inline void globfree(glob_t *pglob) {
  for (size_t i = 0; i < pglob->gl_pathc; i++) {
    free(pglob->gl_pathv[i]);
  }
  free(pglob->gl_pathv);
  pglob->gl_pathc = 0;
  pglob->gl_pathv = NULL;
}
#endif // _WIN32
#endif // COMPAT_CGLOB_H
