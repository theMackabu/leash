#include "leash/util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mach-o/dyld.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

void str_list_init(str_list *list) {
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

void str_list_push(str_list *list, const char *value) {
  if (list->count == list->capacity) {
    size_t next = list->capacity == 0 ? 8 : list->capacity * 2;
    char **items = realloc(list->items, next * sizeof(char *));
    if (!items) die("out of memory");
    list->items = items;
    list->capacity = next;
  }
  list->items[list->count++] = xstrdup(value);
}

void str_list_free(str_list *list) {
  for (size_t i = 0; i < list->count; i++)
    free(list->items[i]);
  free(list->items);
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

char *xstrdup(const char *s) {
  char *copy = strdup(s ? s : "");
  if (!copy) die("out of memory");
  return copy;
}

char *xasprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *out = NULL;
  if (vasprintf(&out, fmt, ap) < 0 || !out) die("out of memory");
  va_end(ap);
  return out;
}

void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

void warn_msg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

char *path_join(const char *a, const char *b) {
  if (!a || !*a) return xstrdup(b);
  if (!b || !*b) return xstrdup(a);
  return xasprintf("%s%s%s", a, a[strlen(a) - 1] == '/' ? "" : "/", b);
}

char *read_text_file(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f) die("cannot open %s: %s", path, strerror(errno));
  if (fseek(f, 0, SEEK_END) != 0) die("cannot seek %s", path);
  long len = ftell(f);
  if (len < 0) die("cannot tell %s", path);
  rewind(f);
  char *buf = malloc((size_t)len + 1);
  if (!buf) die("out of memory");
  size_t got = fread(buf, 1, (size_t)len, f);
  if (got != (size_t)len) die("cannot read %s", path);
  fclose(f);
  buf[len] = '\0';
  if (len_out) *len_out = (size_t)len;
  return buf;
}

char *read_first_line(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  char *line = NULL;
  size_t cap = 0;
  ssize_t len = getline(&line, &cap, f);
  fclose(f);
  if (len < 0) {
    free(line);
    return NULL;
  }
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
    line[--len] = '\0';
  }
  return line;
}

void write_text_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) die("cannot write %s: %s", path, strerror(errno));
  if (fputs(text, f) == EOF) die("cannot write %s", path);
  fclose(f);
}

bool file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void ensure_dir(const char *path) {
  if (mkdir(path, 0777) != 0 && errno != EEXIST) die("mkdir %s: %s", path, strerror(errno));
}

void ensure_dir_tree(const char *path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t len = strlen(tmp);
  while (len > 1 && tmp[len - 1] == '/')
    tmp[--len] = '\0';

  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/') continue;
    *p = '\0';
    ensure_dir(tmp);
    *p = '/';
  }
  ensure_dir(tmp);
}

char *vm_home_dir(void) {
  const char *home = getenv("HOME");
  if (!home || !*home) die("HOME is not set");
  char *dir = path_join(home, ".leash");
  ensure_dir_tree(dir);
  return dir;
}

char *vm_instances_dir(void) {
  const char *env = getenv("LEASHDIR");
  if (env && *env) {
    ensure_dir_tree(env);
    return absolute_existing_dir(env);
  }
  char *home = vm_home_dir();
  char *dir = path_join(home, "instances");
  ensure_dir_tree(dir);
  free(home);
  return dir;
}

char *vm_builders_dir(void) {
  char *home = vm_home_dir();
  char *dir = path_join(home, "builders");
  ensure_dir_tree(dir);
  free(home);
  return dir;
}

char *vm_cache_dir(void) {
  char *home = vm_home_dir();
  char *cache = path_join(home, "cache");
  ensure_dir_tree(cache);
  free(home);
  return cache;
}

char *current_executable_path(void) {
  uint32_t size = 0;
  _NSGetExecutablePath(NULL, &size);
  char *tmp = malloc(size + 1);
  if (!tmp) die("out of memory");
  if (_NSGetExecutablePath(tmp, &size) != 0) die("cannot resolve executable path");
  char resolved[PATH_MAX];
  if (!realpath(tmp, resolved)) die("realpath executable: %s", strerror(errno));
  free(tmp);
  return xstrdup(resolved);
}

char *absolute_existing_dir(const char *path) {
  char resolved[PATH_MAX];
  if (!realpath(path, resolved)) die("cannot resolve %s: %s", path, strerror(errno));
  if (!dir_exists(resolved)) die("%s is not a directory", resolved);
  return xstrdup(resolved);
}

int run_process(char *const argv[], const char *cwd, char *const envp_extra[], bool quiet) {
  pid_t pid = fork();
  if (pid < 0) die("fork: %s", strerror(errno));
  if (pid == 0) {
    if (cwd && chdir(cwd) != 0) {
      fprintf(stderr, "chdir %s: %s\n", cwd, strerror(errno));
      _exit(127);
    }
    if (envp_extra) {
      for (size_t i = 0; envp_extra[i]; i++) {
        putenv(envp_extra[i]);
      }
    }
    if (quiet) {
      int fd = open("/dev/null", O_RDWR);
      if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
      }
    }
    execvp(argv[0], argv);
    fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) die("waitpid: %s", strerror(errno));
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}
