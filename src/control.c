#include "leash/control.h"
#include "leash/builder.h"
#include "leash/key.h"
#include "leash/util.h"

#include <crprintf.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <util.h>

#define SSH_WAIT_LOG_LINES 7
#define SSH_WAIT_LINE_CAP 512

static char *vm_base_dir(void) {
  return vm_instances_dir();
}

static int cmd_ip(const char *name);

static char *vm_candidate_dir(const char *name) {
  if (!name || !*name) die("missing instance name");
  char *base = vm_base_dir();
  char *dir = strchr(name, '/') ? xstrdup(name) : path_join(base, name);
  free(base);
  return dir;
}

static char *expand_vm_dir(const char *name) {
  char *dir = vm_candidate_dir(name);
  char *resolved = absolute_existing_dir(dir);
  free(dir);
  return resolved;
}

static char *ensure_vm_available(const char *name) {
  char *candidate = vm_candidate_dir(name);
  if (!dir_exists(candidate) && !strchr(name, '/')) {
    char *argv[] = {(char *)name};
    int rc = builder_build_main(1, argv);
    if (rc != 0) exit(rc);
  }
  free(candidate);
  return expand_vm_dir(name);
}

static bool is_vm_dir(const char *dir) {
  char *conf = path_join(dir, "leash.conf");
  bool ok = file_exists(conf);
  free(conf);
  return ok;
}

static void generate_mac_file(const char *path) {
  unsigned char b[6];
  if (getentropy(b, sizeof(b)) != 0) die("getentropy: %s", strerror(errno));
  b[0] = (unsigned char)((b[0] & 0xfe) | 0x02);
  char text[13];
  snprintf(text, sizeof(text), "%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
  write_text_file(path, text);
}

static char *get_mac(const char *path) {
  if (!file_exists(path)) generate_mac_file(path);
  char *line = read_first_line(path);
  if (!line) die("cannot read %s", path);
  return line;
}

static char *format_mac_colons(const char *hex) {
  if (strlen(hex) != 12) die("bad MAC in macaddr file: %s", hex);
  return xasprintf("%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", hex, hex + 2, hex + 4, hex + 6, hex + 8, hex + 10);
}

static void args_push(str_list *args, const char *s) {
  str_list_push(args, s);
}

static void compile_vm_conf_args(const char *dir, str_list *args) {
  char *conf = path_join(dir, "leash.conf");
  FILE *f = fopen(conf, "rb");
  if (!f) die("cannot open %s: %s", conf, strerror(errno));
  free(conf);

  char *line = NULL;
  size_t cap = 0;
  unsigned netid = 0;
  while (getline(&line, &cap, f) >= 0) {
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = '\0';
    if (!line[0] || line[0] == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    char *key = line;
    char *value = eq + 1;
    if (!strcmp(key, "network") && !strchr(value, '@')) {
      char *macfile = xasprintf("%s/%u.macaddr", dir, netid);
      char *mac = get_mac(macfile);
      char *colon = format_mac_colons(mac);
      char *arg = xasprintf("--network=%s@%s", colon, value);
      args_push(args, arg);
      free(arg);
      free(colon);
      free(mac);
      free(macfile);
      netid++;
    } else {
      char *arg = xasprintf("--%s=%s", key, value);
      args_push(args, arg);
      free(arg);
      if (!strcmp(key, "network")) netid++;
    }
  }
  free(line);
  fclose(f);
}

static char *vm_pid_path(const char *dir) {
  return path_join(dir, "leash.pid");
}

static char *vm_sock_path(const char *dir) {
  return path_join(dir, "leash.sock");
}

static char *vm_child_pid_path(const char *dir) {
  return path_join(dir, "leash.child.pid");
}

static const char *path_basename_const(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static char *vm_log_name_from_dir(const char *dir) {
  const char *base = path_basename_const(dir);
  return xstrdup(base && *base ? base : "leash");
}

static char *vm_log_dir(const char *log_name) {
  char *home = vm_home_dir();
  char *logs = path_join(home, "logs");
  char *dir = path_join(logs, log_name);
  ensure_dir_tree(dir);
  free(logs);
  free(home);
  return dir;
}

static char *vm_current_log_path(const char *log_name) {
  char *dir = vm_log_dir(log_name);
  char *path = path_join(dir, "current.log");
  free(dir);
  return path;
}

static char *vm_timestamp_log_path(const char *log_name) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  char stamp[64];
  strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H-%M-%S", &tm);
  char *dir = vm_log_dir(log_name);
  char *path = xasprintf("%s/%s.log", dir, stamp);
  for (unsigned i = 1; file_exists(path); i++) {
    free(path);
    path = xasprintf("%s/%s-%u.log", dir, stamp, i);
  }
  free(dir);
  return path;
}

static void archive_current_log(const char *log_name) {
  char *current = vm_current_log_path(log_name);
  struct stat st;
  if (stat(current, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
    free(current);
    return;
  }
  char *archive = vm_timestamp_log_path(log_name);
  if (rename(current, archive) != 0) warn_msg("archive log %s: %s", current, strerror(errno));
  free(archive);
  free(current);
}

static pid_t read_pid_file(const char *path) {
  char *line = read_first_line(path);
  if (!line) return -1;
  char *end = NULL;
  long value = strtol(line, &end, 10);
  free(line);
  if (value <= 0 || (end && *end)) return -1;
  return (pid_t)value;
}

static bool pid_running(pid_t pid) {
  return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static bool vm_running(const char *dir) {
  char *pid_path = vm_pid_path(dir);
  pid_t pid = read_pid_file(pid_path);
  bool running = pid_running(pid);
  if (!running) unlink(pid_path);
  free(pid_path);
  return running;
}

static int connect_vm_socket(const char *sock_path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) die("socket: %s", strerror(errno));
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void clients_remove(int *clients, size_t *count, size_t i) {
  close(clients[i]);
  clients[i] = clients[*count - 1];
  *count -= 1;
}

static void write_all_ignore_errors(int fd, const char *data, size_t length) {
  while (length > 0) {
    ssize_t n = write(fd, data, length);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;
    data += n;
    length -= (size_t)n;
  }
}

static void daemonize_stdio(void) {
  int fd = open("/dev/null", O_RDWR);
  if (fd < 0) return;
  dup2(fd, STDIN_FILENO);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);
  if (fd > STDERR_FILENO) close(fd);
}

static struct termios console_saved_termios;
static bool console_have_saved_termios = false;

static void restore_console_terminal(void) {
  if (console_have_saved_termios) tcsetattr(STDIN_FILENO, TCSANOW, &console_saved_termios);
}

static void console_signal_handler(int sig) {
  restore_console_terminal();
  _exit(sig == SIGINT ? 130 : 128 + sig);
}

static void install_console_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = console_signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

static void supervisor_loop(const char *dir, const char *log_name, const char *pid_path, const char *sock_path,
                            char **argv) {
  if (chdir(dir) != 0) die("chdir %s: %s", dir, strerror(errno));
  archive_current_log(log_name);
  char *current_log_path = vm_current_log_path(log_name);
  int log_fd = open(current_log_path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (log_fd < 0) die("open %s: %s", current_log_path, strerror(errno));
  free(current_log_path);

  int master = -1, slave = -1;
  if (openpty(&master, &slave, NULL, NULL, NULL) != 0) die("openpty: %s", strerror(errno));

  int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0) die("socket: %s", strerror(errno));
  unlink(sock_path);
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) die("bind %s: %s", sock_path, strerror(errno));
  if (listen(listen_fd, 8) != 0) die("listen: %s", strerror(errno));

  pid_t child = fork();
  if (child < 0) die("fork: %s", strerror(errno));
  if (child == 0) {
    close(listen_fd);
    close(master);
    setsid();
    ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO) close(slave);
    execv(argv[0], argv);
    _exit(127);
  }
  close(slave);

  char *pid_text = xasprintf("%ld\n", (long)getpid());
  write_text_file(pid_path, pid_text);
  free(pid_text);
  char *child_pid_path = vm_child_pid_path(dir);
  char *child_pid_text = xasprintf("%ld\n", (long)child);
  write_text_file(child_pid_path, child_pid_text);
  free(child_pid_text);
  daemonize_stdio();

  int clients[32];
  size_t client_count = 0;
  char buf[8192];
  bool child_exited = false;
  while (!child_exited) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(master, &rfds);
    FD_SET(listen_fd, &rfds);
    int maxfd = master > listen_fd ? master : listen_fd;
    for (size_t i = 0; i < client_count; i++) {
      FD_SET(clients[i], &rfds);
      if (clients[i] > maxfd) maxfd = clients[i];
    }

    int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
    if (rc < 0 && errno == EINTR) continue;
    if (rc < 0) break;

    if (FD_ISSET(listen_fd, &rfds)) {
      int client = accept(listen_fd, NULL, NULL);
      if (client >= 0) {
        if (client_count < sizeof(clients) / sizeof(clients[0])) {
          clients[client_count++] = client;
        } else {
          close(client);
        }
      }
    }

    if (FD_ISSET(master, &rfds)) {
      ssize_t n = read(master, buf, sizeof(buf));
      if (n <= 0) break;
      write_all_ignore_errors(log_fd, buf, (size_t)n);
      for (size_t i = 0; i < client_count;) {
        if (write(clients[i], buf, (size_t)n) < 0) clients_remove(clients, &client_count, i);
        else i++;
      }
    }

    for (size_t i = 0; i < client_count;) {
      if (!FD_ISSET(clients[i], &rfds)) {
        i++;
        continue;
      }
      ssize_t n = read(clients[i], buf, sizeof(buf));
      if (n <= 0) {
        clients_remove(clients, &client_count, i);
        continue;
      }
      if (write(master, buf, (size_t)n) < 0) {
        clients_remove(clients, &client_count, i);
        continue;
      }
      i++;
    }

    int status = 0;
    pid_t done = waitpid(child, &status, WNOHANG);
    if (done == child) child_exited = true;
  }

  kill(child, SIGTERM);
  waitpid(child, NULL, 0);
  for (size_t i = 0; i < client_count; i++)
    close(clients[i]);
  close(master);
  close(listen_fd);
  close(log_fd);
  archive_current_log(log_name);
  unlink(sock_path);
  unlink(pid_path);
  unlink(child_pid_path);
  free(child_pid_path);
  _exit(0);
}

typedef void (*start_wait_callback)(void *ctx, const char *detail);

static int vm_start_with_wait(const char *name, start_wait_callback callback, void *ctx, bool error_if_running) {
  char *dir = ensure_vm_available(name);
  if (!is_vm_dir(dir)) die("%s is not a leash instance", dir);
  if (vm_running(dir)) {
    free(dir);
    if (error_if_running) die("instance '%s' is already running", name);
    return 0;
  }

  str_list args;
  str_list_init(&args);
  char *self = current_executable_path();
  args_push(&args, self);
  args_push(&args, "run");
  compile_vm_conf_args(dir, &args);
  char **argv = calloc(args.count + 1, sizeof(char *));
  if (!argv) die("out of memory");
  for (size_t i = 0; i < args.count; i++)
    argv[i] = args.items[i];

  char *pid_path = vm_pid_path(dir);
  char *sock_path = vm_sock_path(dir);
  char *log_name = vm_log_name_from_dir(dir);
  pid_t pid = fork();
  if (pid < 0) die("fork: %s", strerror(errno));
  if (pid == 0) {
    setsid();
    supervisor_loop(dir, log_name, pid_path, sock_path, argv);
  }

  for (int i = 0; i < 50 && !file_exists(pid_path); i++) {
    if (callback) callback(ctx, "starting instance");
    usleep(100000);
  }
  for (int i = 0; i < 50 && !file_exists(sock_path); i++) {
    if (callback) callback(ctx, "starting instance");
    usleep(100000);
  }

  free(sock_path);
  free(pid_path);
  free(log_name);
  free(argv);
  free(self);
  str_list_free(&args);
  free(dir);
  return 0;
}

static int cmd_start_internal(const char *name) {
  return vm_start_with_wait(name, NULL, NULL, false);
}

static double monotonic_seconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static bool first_vm_ip(const char *name, char *ip, size_t ip_size) {
  char *dir = expand_vm_dir(name);
  char *cached_ip_path = path_join(dir, "0.ipaddr");
  char *cached_ip = read_first_line(cached_ip_path);
  free(cached_ip_path);
  free(dir);
  if (cached_ip) {
    snprintf(ip, ip_size, "%s", cached_ip);
    free(cached_ip);
    return true;
  }

  int pipefd[2];
  if (pipe(pipefd) != 0) die("pipe: %s", strerror(errno));
  pid_t pid = fork();
  if (pid < 0) die("fork: %s", strerror(errno));
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    exit(cmd_ip(name));
  }
  close(pipefd[1]);
  FILE *f = fdopen(pipefd[0], "r");
  ip[0] = '\0';
  if (f && fgets(ip, (int)ip_size, f)) ip[strcspn(ip, "\r\n")] = '\0';
  if (f) fclose(f);
  waitpid(pid, NULL, 0);
  return ip[0] != '\0';
}

static bool tcp_port_open(const char *ip, uint16_t port, int timeout_ms) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    close(fd);
    return false;
  }

  int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc == 0) {
    close(fd);
    return true;
  }
  if (errno != EINPROGRESS) {
    close(fd);
    return false;
  }

  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(fd, &wfds);
  struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
  rc = select(fd + 1, NULL, &wfds, NULL, &tv);
  if (rc <= 0) {
    close(fd);
    return false;
  }
  int error = 0;
  socklen_t len = sizeof(error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0) error = errno;
  close(fd);
  return error == 0;
}

static int proxy_connect_tcp(const char *ip, uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) die("socket: %s", strerror(errno));
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) die("bad instance IP: %s", ip);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) die("connect %s:%u: %s", ip, port, strerror(errno));
  return fd;
}

static int proxy_stream(int sock) {
  char buf[8192];
  bool stdin_open = true;
  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    int maxfd = sock;
    if (stdin_open) {
      FD_SET(STDIN_FILENO, &rfds);
      if (STDIN_FILENO > maxfd) maxfd = STDIN_FILENO;
    }

    int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
    if (rc < 0 && errno == EINTR) continue;
    if (rc < 0) return 1;

    if (FD_ISSET(sock, &rfds)) {
      ssize_t n = read(sock, buf, sizeof(buf));
      if (n <= 0) break;
      write_all_ignore_errors(STDOUT_FILENO, buf, (size_t)n);
    }
    if (stdin_open && FD_ISSET(STDIN_FILENO, &rfds)) {
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n <= 0) {
        shutdown(sock, SHUT_WR);
        stdin_open = false;
      } else {
        write_all_ignore_errors(sock, buf, (size_t)n);
      }
    }
  }
  return 0;
}

static int cmd_proxy(const char *name) {
  int started = cmd_start_internal(name);
  if (started != 0) return started;
  vm_ssh_config_ensure();

  char ip[128] = {0};
  double deadline = monotonic_seconds() + 180.0;
  while (monotonic_seconds() < deadline) {
    if (first_vm_ip(name, ip, sizeof(ip)) && tcp_port_open(ip, 22, 250)) {
      int sock = proxy_connect_tcp(ip, 22);
      int rc = proxy_stream(sock);
      close(sock);
      return rc;
    }
    usleep(500000);
  }
  die("timed out waiting for %s ssh", name);
}

typedef struct {
  int fd;
  bool tty;
  size_t rendered_lines;
  char *lines[SSH_WAIT_LOG_LINES];
  size_t line_count;
  char current[SSH_WAIT_LINE_CAP];
  size_t current_len;
  int escape_state;
  bool pending_cr;
  double started_at;
  const char *name;
  const char *verb;
  unsigned frame;
} ssh_wait_preview;

static void ssh_wait_preview_init(ssh_wait_preview *preview) {
  memset(preview, 0, sizeof(*preview));
  preview->fd = -1;
  preview->tty = isatty(STDERR_FILENO);
}

static void ssh_wait_preview_close(ssh_wait_preview *preview) {
  if (preview->fd >= 0) {
    close(preview->fd);
    preview->fd = -1;
  }
}

static void ssh_wait_preview_free(ssh_wait_preview *preview) {
  ssh_wait_preview_close(preview);
  for (size_t i = 0; i < preview->line_count; i++)
    free(preview->lines[i]);
}

static void ssh_wait_preview_open(ssh_wait_preview *preview, const char *name) {
  if (preview->fd >= 0) return;
  char *dir = expand_vm_dir(name);
  char *log_name = vm_log_name_from_dir(dir);
  char *log_path = vm_current_log_path(log_name);
  preview->fd = open(log_path, O_RDONLY);
  if (preview->fd >= 0) {
    int flags = fcntl(preview->fd, F_GETFL, 0);
    if (flags >= 0) fcntl(preview->fd, F_SETFL, flags | O_NONBLOCK);
  }
  free(log_path);
  free(log_name);
  free(dir);
}

static bool ssh_wait_preview_hide_line(const char *line) {
  if (strstr(line, "-----BEGIN SSH HOST KEY")) return true;
  if (strstr(line, "-----END SSH HOST KEY")) return true;
  if ((strncmp(line, "ssh-", 4) == 0 || strncmp(line, "ecdsa-", 6) == 0) && strstr(line, " root@")) return true;
  return false;
}

static void ssh_wait_preview_add_line(ssh_wait_preview *preview) {
  while (preview->current_len > 0 && isspace((unsigned char)preview->current[preview->current_len - 1]))
    preview->current_len--;
  preview->current[preview->current_len] = '\0';
  if (ssh_wait_preview_hide_line(preview->current)) {
    preview->current_len = 0;
    return;
  }
  char *line = preview->current_len > 0 ? xstrdup(preview->current) : xstrdup("");
  if (preview->line_count == SSH_WAIT_LOG_LINES) {
    free(preview->lines[0]);
    memmove(preview->lines, preview->lines + 1, (SSH_WAIT_LOG_LINES - 1) * sizeof(preview->lines[0]));
    preview->lines[SSH_WAIT_LOG_LINES - 1] = line;
  } else {
    preview->lines[preview->line_count++] = line;
  }
  preview->current_len = 0;
}

static void ssh_wait_preview_byte(ssh_wait_preview *preview, unsigned char ch) {
  if (preview->pending_cr && ch != '\n') {
    preview->current_len = 0;
    preview->pending_cr = false;
  }
  if (preview->escape_state == 1) {
    preview->escape_state = ch == '[' ? 2 : 0;
    return;
  }
  if (preview->escape_state == 2) {
    if (ch >= 0x40 && ch <= 0x7e) preview->escape_state = 0;
    return;
  }
  if (ch == '\033') {
    preview->escape_state = 1;
    return;
  }
  if (ch == '\r') {
    preview->pending_cr = true;
    return;
  }
  if (ch == '\n') {
    if (preview->current_len > 0) ssh_wait_preview_add_line(preview);
    preview->pending_cr = false;
    return;
  }
  if (ch == '\b') {
    if (preview->current_len > 0) preview->current_len--;
    return;
  }
  if (ch == '\t') ch = ' ';
  if (!isprint(ch)) return;
  if (preview->current_len + 1 < sizeof(preview->current)) preview->current[preview->current_len++] = (char)ch;
}

static void ssh_wait_preview_poll(ssh_wait_preview *preview) {
  if (preview->fd < 0) return;
  char buf[4096];
  for (;;) {
    ssize_t n = read(preview->fd, buf, sizeof(buf));
    if (n > 0) {
      for (ssize_t i = 0; i < n; i++)
        ssh_wait_preview_byte(preview, (unsigned char)buf[i]);
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) ssh_wait_preview_close(preview);
    break;
  }
}

static void ssh_wait_preview_clear(ssh_wait_preview *preview) {
  if (!preview->tty || preview->rendered_lines == 0) return;
  fprintf(stderr, "\r");
  if (preview->rendered_lines > 1) fprintf(stderr, "\033[%zuA", preview->rendered_lines - 1);
  for (size_t i = 0; i < preview->rendered_lines; i++) {
    fprintf(stderr, "\033[2K");
    if (i + 1 < preview->rendered_lines) fprintf(stderr, "\033[1B\r");
  }
  if (preview->rendered_lines > 1) fprintf(stderr, "\033[%zuA", preview->rendered_lines - 1);
  fprintf(stderr, "\r");
  fflush(stderr);
  preview->rendered_lines = 0;
}

static void ssh_wait_status(ssh_wait_preview *preview, const char *name, const char *detail, double started_at,
                            unsigned frame) {
  static const char frames[] = {'-', '\\', '|', '/'};
  const char *verb = preview->verb ?: "connecting";
  if (preview->tty) {
    struct winsize ws;
    size_t columns = 80;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) columns = ws.ws_col;
    size_t log_columns = columns > 1 ? columns - 1 : columns;
    if (preview->rendered_lines > 0) {
      fprintf(stderr, "\r");
      if (preview->rendered_lines > 1) fprintf(stderr, "\033[%zuA", preview->rendered_lines - 1);
    }
    crfprintf(stderr, "\033[2K<cyan>%c</cyan> %s '<bold>%s</bold>' - %s <dim>(%.1fs)</dim>",
              frames[frame % (sizeof(frames) / sizeof(frames[0]))], verb, name, detail,
              monotonic_seconds() - started_at);
    size_t visible_lines = preview->line_count > SSH_WAIT_LOG_LINES ? SSH_WAIT_LOG_LINES : preview->line_count;
    size_t first = preview->line_count > visible_lines ? preview->line_count - visible_lines : 0;
    if (visible_lines > 0) fprintf(stderr, "\033[1B\r");
    for (size_t i = 0; i < visible_lines; i++) {
      fprintf(stderr, "\033[2K");
      size_t idx = first + i;
      if (idx < preview->line_count) {
        size_t len = strlen(preview->lines[idx]);
        if (len > log_columns) len = log_columns;
        fprintf(stderr, "\033[2m%.*s\033[0m", (int)len, preview->lines[idx]);
      }
      if (i + 1 < visible_lines) fprintf(stderr, "\033[1B\r");
    }
    preview->rendered_lines = visible_lines + 1;
    fflush(stderr);
  } else if (frame == 0) {
    crfprintf(stderr, "<dim>%s '%s' - %s</dim>\n", verb, name, detail);
  }
}

static void ssh_start_wait_callback(void *ctx, const char *detail) {
  ssh_wait_preview *preview = ctx;
  ssh_wait_preview_open(preview, preview->name);
  ssh_wait_preview_poll(preview);
  ssh_wait_status(preview, preview->name, detail, preview->started_at, preview->frame++);
}

static int cmd_connect_ssh(const char *name) {
  double started_at = monotonic_seconds();
  ssh_wait_preview preview;
  ssh_wait_preview_init(&preview);
  preview.started_at = started_at;
  preview.name = name;
  preview.verb = "connecting";
  ssh_wait_status(&preview, name, "starting instance", started_at, preview.frame++);
  int started = vm_start_with_wait(name, ssh_start_wait_callback, &preview, false);
  if (started != 0) return started;
  ssh_wait_preview_open(&preview, name);

  char ip[128] = {0};
  double last_ip_check = 0.0;
  double deadline = monotonic_seconds() + 180.0;
  while (monotonic_seconds() < deadline) {
    ssh_wait_preview_poll(&preview);
    ssh_wait_preview_open(&preview, name);
    const char *detail = ip[0] ? "waiting for ssh" : "waiting for IP";
    ssh_wait_status(&preview, name, detail, started_at, preview.frame++);

    double now = monotonic_seconds();
    if (now - last_ip_check >= 1.0) {
      last_ip_check = now;
      if (!ip[0]) (void)first_vm_ip(name, ip, sizeof(ip));
      if (ip[0] && tcp_port_open(ip, 22, 100)) {
        ssh_wait_preview_clear(&preview);
        ssh_wait_preview_free(&preview);
        vm_ssh_config_ensure();
        char *target = xasprintf("%s@leash.local", name);
        execlp("ssh", "ssh", target, NULL);
        die("exec ssh: %s", strerror(errno));
      }
    }
    usleep(100000);
  }
  ssh_wait_preview_clear(&preview);
  ssh_wait_preview_free(&preview);
  die("timed out waiting for instance IP");
}

static int cmd_attach(const char *name) {
  return cmd_connect_ssh(name);
}

static int cmd_start(const char *name) {
  double started_at = monotonic_seconds();
  ssh_wait_preview preview;
  ssh_wait_preview_init(&preview);
  preview.started_at = started_at;
  preview.name = name;
  preview.verb = "starting";
  int rc = vm_start_with_wait(name, ssh_start_wait_callback, &preview, true);
  ssh_wait_preview_poll(&preview);
  ssh_wait_preview_clear(&preview);
  ssh_wait_preview_free(&preview);
  if (rc == 0) crprintf("<green>started leash</green> '<bold>%s</bold>'\n", name);
  return rc;
}

static int cmd_console(const char *name) {
  int started = cmd_start_internal(name);
  if (started != 0) return started;
  char *dir = expand_vm_dir(name);
  if (!is_vm_dir(dir)) die("%s is not a leash instance", dir);
  char *sock_path = vm_sock_path(dir);
  int sock = connect_vm_socket(sock_path);
  if (sock < 0) die("instance is not running");

  struct termios oldt, raw;
  bool have_term = tcgetattr(STDIN_FILENO, &oldt) == 0;
  if (have_term) {
    console_saved_termios = oldt;
    console_have_saved_termios = true;
    install_console_signal_handlers();
    raw = oldt;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  }

  char buf[8192];
  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    FD_SET(sock, &rfds);
    int maxfd = sock > STDIN_FILENO ? sock : STDIN_FILENO;
    int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
    if (rc < 0 && errno == EINTR) continue;
    if (rc < 0) break;
    if (FD_ISSET(sock, &rfds)) {
      ssize_t n = read(sock, buf, sizeof(buf));
      if (n <= 0) break;
      if (write(STDOUT_FILENO, buf, (size_t)n) < 0) break;
    }
    if (FD_ISSET(STDIN_FILENO, &rfds)) {
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n <= 0) break;
      if (write(sock, buf, (size_t)n) < 0) break;
    }
  }

  if (have_term) {
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    console_have_saved_termios = false;
  }
  close(sock);
  free(sock_path);
  free(dir);
  return 0;
}

static int cmd_logs(const char *name) {
  int started = cmd_start_internal(name);
  if (started != 0) return started;
  char *dir = expand_vm_dir(name);
  if (!is_vm_dir(dir)) die("%s is not a leash instance", dir);
  char *log_name = vm_log_name_from_dir(dir);
  char *log_path = vm_current_log_path(log_name);
  int fd = -1;
  for (int i = 0; i < 50 && fd < 0; i++) {
    fd = open(log_path, O_RDONLY);
    if (fd < 0) usleep(100000);
  }
  if (fd < 0) die("cannot open %s: %s", log_path, strerror(errno));

  char buf[8192];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      write_all_ignore_errors(STDOUT_FILENO, buf, (size_t)n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) break;

    if (!vm_running(dir)) break;
    usleep(100000);
  }
  close(fd);
  free(log_path);
  free(log_name);
  free(dir);
  return 0;
}

static int cmd_stop(const char *name) {
  char *dir = expand_vm_dir(name);
  if (!is_vm_dir(dir)) die("%s is not a leash instance", dir);
  if (!vm_running(dir)) die("instance '%s' is not running", name);
  char *sock_path = vm_sock_path(dir);
  char *pid_path = vm_pid_path(dir);
  char *log_name = vm_log_name_from_dir(dir);
  int sock = connect_vm_socket(sock_path);
  if (sock >= 0) {
    char escape[] = {'\033', 'q'};
    (void)write(sock, escape, sizeof(escape));
    close(sock);
  }
  pid_t pid = read_pid_file(pid_path);
  for (int i = 0; i < 100 && pid_running(pid); i++)
    usleep(100000);
  if (pid_running(pid)) kill(pid, SIGTERM);
  archive_current_log(log_name);
  unlink(sock_path);
  unlink(pid_path);
  free(log_name);
  free(pid_path);
  free(sock_path);
  free(dir);
  return 0;
}

static int cmd_kill(const char *name) {
  char *dir = expand_vm_dir(name);
  if (!is_vm_dir(dir)) die("%s is not a leash instance", dir);
  char *sock_path = vm_sock_path(dir);
  char *pid_path = vm_pid_path(dir);
  char *child_pid_path = vm_child_pid_path(dir);
  char *log_name = vm_log_name_from_dir(dir);
  pid_t child = read_pid_file(child_pid_path);
  pid_t supervisor = read_pid_file(pid_path);
  if (child > 0) {
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
  }
  if (supervisor > 0) {
    kill(-supervisor, SIGKILL);
    kill(supervisor, SIGKILL);
  }
  for (int i = 0; i < 20 && (pid_running(child) || pid_running(supervisor)); i++)
    usleep(50000);
  archive_current_log(log_name);
  unlink(sock_path);
  unlink(pid_path);
  unlink(child_pid_path);
  free(log_name);
  free(child_pid_path);
  free(pid_path);
  free(sock_path);
  free(dir);
  return 0;
}

typedef struct {
  char ip[64];
  char mac[13];
} arp_assoc;

static size_t read_arp(arp_assoc **out) {
  int pipefd[2];
  if (pipe(pipefd) != 0) die("pipe: %s", strerror(errno));
  pid_t pid = fork();
  if (pid < 0) die("fork: %s", strerror(errno));
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    execlp("arp", "arp", "-a", NULL);
    _exit(127);
  }
  close(pipefd[1]);
  FILE *f = fdopen(pipefd[0], "r");
  if (!f) die("fdopen: %s", strerror(errno));
  arp_assoc *items = NULL;
  size_t count = 0, cap = 0;
  char *line = NULL;
  size_t linecap = 0;
  while (getline(&line, &linecap, f) >= 0) {
    char ip[64], mac_raw[64];
    if (sscanf(line, "%*s (%63[^)]) at %63s", ip, mac_raw) != 2) continue;
    if (!strchr(mac_raw, ':')) continue;
    char mac[13] = {0};
    size_t pos = 0;
    for (const char *p = mac_raw; *p && pos < 12; p++) {
      if (*p == ':') continue;
      mac[pos++] = (char)tolower((unsigned char)*p);
    }
    if (pos != 12) continue;
    if (count == cap) {
      cap = cap ? cap * 2 : 16;
      items = realloc(items, cap * sizeof(*items));
      if (!items) die("out of memory");
    }
    snprintf(items[count].ip, sizeof(items[count].ip), "%s", ip);
    snprintf(items[count].mac, sizeof(items[count].mac), "%s", mac);
    count++;
  }
  free(line);
  fclose(f);
  int status;
  waitpid(pid, &status, 0);
  *out = items;
  return count;
}

static int cmd_ip(const char *name) {
  char *dir = expand_vm_dir(name);
  if (!is_vm_dir(dir)) die("%s is not a leash instance", dir);
  char *cached_ip_path = path_join(dir, "0.ipaddr");
  char *cached_ip = read_first_line(cached_ip_path);
  free(cached_ip_path);
  if (cached_ip) {
    printf("%s\n", cached_ip);
    free(cached_ip);
    free(dir);
    return 0;
  }
  arp_assoc *assoc = NULL;
  size_t assoc_count = read_arp(&assoc);
  DIR *d = opendir(dir);
  if (!d) die("opendir %s: %s", dir, strerror(errno));
  struct dirent *ent;
  while ((ent = readdir(d))) {
    size_t len = strlen(ent->d_name);
    if (len <= 8 || strcmp(ent->d_name + len - 8, ".macaddr")) continue;
    char *mac_path = path_join(dir, ent->d_name);
    char *mac = read_first_line(mac_path);
    char *prefix = xstrdup(mac_path);
    prefix[strlen(prefix) - 8] = '\0';
    char *ip_path = xasprintf("%s.ipaddr", prefix);
    if (mac) {
      for (size_t i = 0; i < assoc_count; i++) {
        if (!strcmp(mac, assoc[i].mac)) {
          write_text_file(ip_path, assoc[i].ip);
          break;
        }
      }
    }
    char *ip = read_first_line(ip_path);
    if (ip) {
      printf("%s\n", ip);
      free(ip);
    }
    free(ip_path);
    free(prefix);
    free(mac);
    free(mac_path);
  }
  closedir(d);
  free(assoc);
  free(dir);
  return 0;
}

static int cmd_ssh(const char *name) {
  return cmd_connect_ssh(name);
}

static int cmd_list(void) {
  char *base = vm_base_dir();
  DIR *d = opendir(base);
  if (!d) die("opendir %s: %s", base, strerror(errno));
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    char *dir = path_join(base, ent->d_name);
    if (!dir_exists(dir)) {
      free(dir);
      continue;
    }
    if (!is_vm_dir(dir)) {
      free(dir);
      continue;
    }
    bool running = vm_running(dir);
    if (running) crprintf("<green>● running</green>\t%s\n", ent->d_name);
    else crprintf("<red>● stopped</red>\t%s\n", ent->d_name);
    free(dir);
  }
  closedir(d);
  free(base);
  return 0;
}

int leash_control_main(int argc, char **argv) {
  crprintf_set_color(isatty(STDOUT_FILENO));
  if (argc < 1) {
    fprintf(stderr, "usage: leash {start|stop|kill|attach|logs|console|ip|ssh} INSTANCE\n       leash list\n");
    return 1;
  }
  if (!strcmp(argv[0], "start"))
    return argc >= 2 ? cmd_start(argv[1]) : (fprintf(stderr, "missing instance name\n"), 1);
  if (!strcmp(argv[0], "stop")) return argc >= 2 ? cmd_stop(argv[1]) : (fprintf(stderr, "missing instance name\n"), 1);
  if (!strcmp(argv[0], "kill")) return argc >= 2 ? cmd_kill(argv[1]) : (fprintf(stderr, "missing instance name\n"), 1);
  if (!strcmp(argv[0], "attach"))
    return argc >= 2 ? cmd_attach(argv[1]) : (fprintf(stderr, "missing instance name\n"), 1);
  if (!strcmp(argv[0], "logs")) return argc >= 2 ? cmd_logs(argv[1]) : (fprintf(stderr, "missing instance name\n"), 1);
  if (!strcmp(argv[0], "console"))
    return argc >= 2 ? cmd_console(argv[1]) : (fprintf(stderr, "missing instance name\n"), 1);
  if (!strcmp(argv[0], "ip")) return argc >= 2 ? cmd_ip(argv[1]) : (fprintf(stderr, "missing instance name\n"), 1);
  if (!strcmp(argv[0], "ssh")) return argc >= 2 ? cmd_ssh(argv[1]) : (fprintf(stderr, "missing instance name\n"), 1);
  if (!strcmp(argv[0], "proxy"))
    return argc >= 2 ? cmd_proxy(argv[1]) : (fprintf(stderr, "missing instance name\n"), 1);
  if (!strcmp(argv[0], "list") || !strcmp(argv[0], "ls")) return cmd_list();
  fprintf(stderr, "usage: leash {start|stop|kill|attach|logs|console|ip|ssh} INSTANCE\n       leash list\n");
  return 1;
}
