#include "leash/key.h"
#include "leash/util.h"

#include <errno.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *vm_ssh_key_private_path(const char *dir) {
  return path_join(dir, "ssh_key");
}

char *vm_ssh_key_public_path(const char *dir) {
  return path_join(dir, "ssh_key.pub");
}

static char *vm_name_from_dir(const char *dir) {
  char *copy = xstrdup(dir);
  char *base = basename(copy);
  char *name = xstrdup(base && *base ? base : "leash");
  free(copy);
  return name;
}

void vm_ssh_key_ensure(const char *dir) {
  char *private_path = vm_ssh_key_private_path(dir);
  char *public_path = vm_ssh_key_public_path(dir);
  if (file_exists(private_path) && file_exists(public_path)) {
    chmod(private_path, 0600);
    free(public_path);
    free(private_path);
    return;
  }

  unlink(private_path);
  unlink(public_path);
  char *name = vm_name_from_dir(dir);
  char *comment = xasprintf("leash:%s", name);
  char *argv[] = {"ssh-keygen", "-q", "-t", "rsa",   "-b", "3072",       "-m", "PEM",
                  "-N",         "",   "-C", comment, "-f", private_path, NULL};
  int rc = run_process(argv, NULL, NULL, false);
  if (rc != 0) die("ssh-keygen failed creating %s", private_path);
  chmod(private_path, 0600);
  free(comment);
  free(name);
  free(public_path);
  free(private_path);
}

char *vm_ssh_key_public_line(const char *dir) {
  vm_ssh_key_ensure(dir);
  char *public_path = vm_ssh_key_public_path(dir);
  char *line = read_first_line(public_path);
  if (!line) die("cannot read %s", public_path);
  free(public_path);
  return line;
}

static char *vm_ssh_config_dir(void) {
  char *home = vm_home_dir();
  char *dir = path_join(home, "ssh");
  ensure_dir_tree(dir);
  free(home);
  return dir;
}

static char *vm_ssh_config_path(void) {
  char *dir = vm_ssh_config_dir();
  char *path = path_join(dir, "config");
  free(dir);
  return path;
}

static char *user_ssh_config_path(void) {
  const char *home = getenv("HOME");
  if (!home || !*home) die("HOME is not set");
  char *ssh_dir = path_join(home, ".ssh");
  ensure_dir_tree(ssh_dir);
  char *path = path_join(ssh_dir, "config");
  free(ssh_dir);
  return path;
}

static bool text_contains(const char *text, const char *needle) {
  return text && needle && strstr(text, needle);
}

static void ensure_user_ssh_include(void) {
  char *config = user_ssh_config_path();
  char *vm_config = vm_ssh_config_path();
  const char *include_line = "Include ~/.leash/ssh/config\n";
  char *absolute_include = xasprintf("Include %s\n", vm_config);
  char *text = file_exists(config) ? read_text_file(config, NULL) : xstrdup("");

  if (text_contains(text, absolute_include)) {
    FILE *f = fopen(config, "wb");
    if (!f) die("open %s: %s", config, strerror(errno));
    char *line = text;
    while (*line) {
      char *next = strchr(line, '\n');
      size_t len = next ? (size_t)(next - line + 1) : strlen(line);
      bool skip = len == strlen(absolute_include) && strncmp(line, absolute_include, len) == 0;
      if (!skip) fwrite(line, 1, len, f);
      line += len;
    }
    fclose(f);
    free(text);
    text = read_text_file(config, NULL);
  }

  if (!text_contains(text, include_line)) {
    FILE *f = fopen(config, file_exists(config) ? "r+b" : "wb");
    if (!f) die("open %s: %s", config, strerror(errno));
    char *old = text;
    fputs(include_line, f);
    fputs(old, f);
    fclose(f);
    chmod(config, 0600);
  }
  free(text);
  free(absolute_include);
  free(vm_config);
  free(config);
}

static char *remove_managed_block(const char *text) {
  const char *cursor = text;
  char *out = xstrdup("");
  while (*cursor) {
    const char *start = strstr(cursor, "# leash managed:");
    if (!start) {
      char *next = xasprintf("%s%s", out, cursor);
      free(out);
      return next;
    }
    const char *end = strstr(start, "# end leash managed:");
    if (!end) break;
    const char *after = strchr(end, '\n');
    after = after ? after + 1 : end + strlen(end);
    char *prefix = strndup(cursor, (size_t)(start - cursor));
    if (!prefix) die("out of memory");
    char *next = xasprintf("%s%s", out, prefix);
    free(prefix);
    free(out);
    out = next;
    cursor = after;
  }
  char *next = xasprintf("%s%s", out, cursor);
  free(out);
  return next;
}

static char *trim_blank_prefix(const char *text) {
  while (*text == '\n' || *text == '\r')
    text++;
  return xstrdup(text);
}

static char *collapse_blank_lines(const char *text) {
  char *out = malloc(strlen(text) + 1);
  if (!out) die("out of memory");
  char *dst = out;
  bool previous_blank = false;
  const char *line = text;
  while (*line) {
    const char *next = strchr(line, '\n');
    size_t len = next ? (size_t)(next - line + 1) : strlen(line);
    bool blank = len == 1 && line[0] == '\n';
    if (!blank || !previous_blank) {
      memcpy(dst, line, len);
      dst += len;
    }
    previous_blank = blank;
    line += len;
  }
  *dst = '\0';
  return out;
}

void vm_ssh_config_ensure(void) {
  char *config = vm_ssh_config_path();
  char *self = current_executable_path();
  char *home = vm_home_dir();
  char *identity = xasprintf("%s/instances/%%r/ssh_key", home);
  char *old = file_exists(config) ? read_text_file(config, NULL) : xstrdup("");
  char *without_managed = remove_managed_block(old);
  char *trimmed = trim_blank_prefix(without_managed);
  char *base = collapse_blank_lines(trimmed);
  char *block = xasprintf("# leash managed: leash.local\n"
                          "Host leash.local\n"
                          "  HostName leash.local\n"
                          "  ProxyCommand %s proxy %%r\n"
                          "  IdentityFile %s\n"
                          "  IdentitiesOnly yes\n"
                          "  StrictHostKeyChecking no\n"
                          "  UserKnownHostsFile /dev/null\n"
                          "  LogLevel ERROR\n"
                          "# end leash managed: leash.local\n",
                          self, identity);

  FILE *f = fopen(config, "wb");
  if (!f) die("open %s: %s", config, strerror(errno));
  fputs(base, f);
  if (base[0] && base[strlen(base) - 1] != '\n') fputc('\n', f);
  fputs(block, f);
  fclose(f);
  chmod(config, 0600);
  ensure_user_ssh_include();

  free(block);
  free(base);
  free(trimmed);
  free(without_managed);
  free(old);
  free(identity);
  free(home);
  free(self);
  free(config);
}
