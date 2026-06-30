#include "leash/builder.h"
#include "leash/download.h"
#include "leash/key.h"
#include "leash/nocloud.h"
#include "leash/util.h"

#include <crprintf.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <yaml.h>
#include <zlib.h>

#include "default_ubuntu_builder.inc"

#define NOCLOUD_SEED_PATH "cidata.iso"

void builder_ensure_defaults(void) {
  char *builders = vm_builders_dir();
  char *ubuntu = path_join(builders, "ubuntu.yml");
  if (!file_exists(ubuntu)) write_text_file(ubuntu, default_ubuntu_builder);
  free(ubuntu);
  free(builders);
}

static char *host_cloud_arch(void) {
  struct utsname u;
  if (uname(&u) != 0) die("uname: %s", strerror(errno));
  if (!strcmp(u.machine, "x86_64")) return xstrdup("amd64");
  return xstrdup(u.machine);
}

static char *default_output_dir(const char *path) {
  char *copy = xstrdup(path);
  char *base = basename(copy);
  char *out = xstrdup(base);
  free(copy);
  char *dot = strrchr(out, '.');
  if (dot && dot != out) *dot = '\0';
  return out;
}

static char *display_name_for_output(const char *output) {
  char *copy = xstrdup(output ?: "leash");
  char *base = basename(copy);
  char *name = xstrdup(base);
  free(copy);
  return name;
}

static char *expand_home_path(const char *path) {
  if (!path || strncmp(path, "~/", 2) != 0) return xstrdup(path ?: "");
  const char *home = getenv("HOME");
  if (!home || !*home) die("HOME is not set");
  return path_join(home, path + 2);
}

static char *downloads_cache_dir(void) {
  char *cache = vm_cache_dir();
  char *downloads = path_join(cache, "downloads");
  ensure_dir_tree(downloads);
  free(cache);
  return downloads;
}

static char *images_cache_dir(void) {
  char *cache = vm_cache_dir();
  char *images = path_join(cache, "images");
  ensure_dir_tree(images);
  free(cache);
  return images;
}

static char *url_cache_path(const char *cache_dir, const char *url, const char *fallback) {
  const char *name = strrchr(url, '/');
  name = name ? name + 1 : fallback;
  if (!name || !*name) name = fallback;
  char *copy = xstrdup(name);
  copy[strcspn(copy, "?#")] = '\0';
  if (!*copy) {
    free(copy);
    copy = xstrdup(fallback);
  }
  char *path = path_join(cache_dir, copy);
  free(copy);
  return path;
}

static char *cache_path_with_suffix(const char *cache_dir, const char *url, const char *fallback, const char *suffix) {
  char *path = url_cache_path(cache_dir, url, fallback);
  char *suffixed = xasprintf("%s%s", path, suffix);
  free(path);
  return suffixed;
}

static void copy_file(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (!in) die("open %s: %s", src, strerror(errno));
  FILE *out = fopen(dst, "wb");
  if (!out) die("open %s: %s", dst, strerror(errno));
  char buf[128 * 1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) die("write %s failed", dst);
  }
  if (ferror(in)) die("read %s failed", src);
  fclose(in);
  fclose(out);
}

static void clone_or_copy_file(const char *src, const char *dst) {
  unlink(dst);
  if (clonefile(src, dst, 0) == 0) return;
  copy_file(src, dst);
}

static char *replace_all(const char *s, const char *needle, const char *replacement) {
  size_t needle_len = strlen(needle);
  size_t replacement_len = strlen(replacement);
  size_t count = 0;
  for (const char *p = s; (p = strstr(p, needle)); p += needle_len)
    count++;
  size_t len = strlen(s);
  if (replacement_len >= needle_len) len += count * (replacement_len - needle_len);
  else len -= count * (needle_len - replacement_len);
  char *out = malloc(len + 1);
  if (!out) die("out of memory");

  char *dst = out;
  const char *src = s;
  const char *match = NULL;
  while ((match = strstr(src, needle))) {
    size_t chunk = (size_t)(match - src);
    memcpy(dst, src, chunk);
    dst += chunk;
    memcpy(dst, replacement, replacement_len);
    dst += replacement_len;
    src = match + needle_len;
  }
  strcpy(dst, src);
  return out;
}

static char *expand_template(const vm_build_config *config, const char *s, const char *arch) {
  char *arch_expanded = replace_all(s, "{arch}", arch);
  char *release_expanded = replace_all(arch_expanded, "{release}", config->release ?: "");
  char *version_expanded = replace_all(release_expanded, "{version}", config->version ?: "");
  char *user_expanded = replace_all(version_expanded, "{user}", getenv("USER") ?: "user");
  free(arch_expanded);
  free(release_expanded);
  free(version_expanded);
  return user_expanded;
}

static int parse_int_value(const char *key, const char *value) {
  char *end = NULL;
  long v = strtol(value, &end, 10);
  if (!*value || *end || v <= 0 || v > INT_MAX) die("invalid integer for %s: %s", key, value);
  return (int)v;
}

static bool parse_bool_value(const char *key, const char *value) {
  if (!strcmp(value, "true") || !strcmp(value, "yes") || !strcmp(value, "1")) return true;
  if (!strcmp(value, "false") || !strcmp(value, "no") || !strcmp(value, "0")) return false;
  die("invalid boolean for %s: %s", key, value);
}

static void replace_string(char **slot, const char *value) {
  free(*slot);
  *slot = xstrdup(value);
}

static bool key_is(const char *key, const char *a, const char *b, const char *c) {
  return !strcmp(key, a) || (b && !strcmp(key, b)) || (c && !strcmp(key, c));
}

static void apply_config_value(vm_build_config *config, const char *key, const char *value) {
  if (key_is(key, "type", "builder", NULL)) replace_string(&config->type, value);
  else if (key_is(key, "output", "output_dir", "directory")) replace_string(&config->output_dir, value);
  else if (key_is(key, "arch", NULL, NULL)) replace_string(&config->arch, value);
  else if (key_is(key, "release", NULL, NULL)) replace_string(&config->release, value);
  else if (key_is(key, "version", NULL, NULL)) replace_string(&config->version, value);
  else if (key_is(key, "disk_size_mb", "disk.size_mb", NULL)) config->disk_size_mb = parse_int_value(key, value);
  else if (key_is(key, "customize", NULL, NULL)) config->customize = parse_bool_value(key, value);
  else if (key_is(key, "leash.cpu_count", "cpu_count", NULL)) config->cpu_count = (size_t)parse_int_value(key, value);
  else if (key_is(key, "leash.memory_mb", "memory_mb", NULL)) config->memory_mb = parse_int_value(key, value);
  else if (key_is(key, "leash.network", "network", NULL)) replace_string(&config->network, value);
  else if (key_is(key, "leash.ip", "leash.network_ip", "network_ip")) replace_string(&config->network_ip, value);
  else if (key_is(key, "leash.nested_virtualization", "nested_virtualization", "leash.nested-virtualization"))
    config->nested_virtualization = parse_bool_value(key, value);
  else if (key_is(key, "leash.cmdline", "cmdline", NULL)) replace_string(&config->cmdline, value);
  else if (key_is(key, "leash.kernel", "kernel", "kernel_path")) replace_string(&config->kernel_path, value);
  else if (key_is(key, "leash.initrd", "initrd", "initrd_path")) replace_string(&config->initrd_path, value);
  else if (key_is(key, "leash.disk", "disk", "disk_path")) replace_string(&config->disk_path, value);
  else if (key_is(key, "cloud_init.enabled", "cloud_init_enabled", NULL))
    config->cloud_init_enabled = parse_bool_value(key, value);
  else if (key_is(key, "cloud_init.user", "user", NULL)) replace_string(&config->cloud_user, value);
  else if (key_is(key, "cloud_init.ssh_key", "ssh_key", NULL)) replace_string(&config->ssh_key, value);
  else if (key_is(key, "cloud_init.ssh_key_file", "ssh_key_file", "ssh_key_path"))
    replace_string(&config->ssh_key_file, value);
  else if (key_is(key, "cloud_init.user_data", "user_data", "user_data_file"))
    replace_string(&config->cloud_init_path, value);
  else if (key_is(key, "cloud_init.remove_irqbalance", "remove_irqbalance", NULL))
    config->remove_irqbalance = parse_bool_value(key, value);
  else if (key_is(key, "downloads.kernel_url", "kernel_url", NULL)) replace_string(&config->kernel_url, value);
  else if (key_is(key, "downloads.initrd_url", "initrd_url", NULL)) replace_string(&config->initrd_url, value);
  else if (key_is(key, "downloads.disk_url", "disk_url", NULL)) replace_string(&config->disk_url, value);
  else if (key_is(key, "downloads.disk_archive", "disk_archive", NULL)) replace_string(&config->disk_archive, value);
  else if (key_is(key, "downloads.disk_image", "disk_image", NULL)) replace_string(&config->disk_image, value);
  else if (key_is(key, "downloads.kernel_gzip", "kernel_gzip", NULL)) {
    if (!strcmp(value, "true") || !strcmp(value, "yes") || !strcmp(value, "1")) {
      config->kernel_compression = VM_BUILD_KERNEL_COMPRESSED;
    } else if (!strcmp(value, "false") || !strcmp(value, "no") || !strcmp(value, "0")) {
      config->kernel_compression = VM_BUILD_KERNEL_UNCOMPRESSED;
    } else if (!strcmp(value, "auto_non_amd64") || !strcmp(value, "auto-non-amd64")) {
      config->kernel_compression = VM_BUILD_KERNEL_AUTO_NON_AMD64;
    } else {
      die("invalid value for %s: %s", key, value);
    }
  } else {
    warn_msg("ignoring unknown build config key: %s", key);
  }
}

void vm_build_config_init(vm_build_config *config) {
  memset(config, 0, sizeof(*config));
  config->disk_size_mb = 4096;
  config->customize = true;
  config->cpu_count = 1;
  config->memory_mb = 1024;
  config->network = xstrdup("nat");
  config->network_ip = xstrdup("auto");
  config->cmdline = xstrdup("console=hvc0 irqfixup root=/dev/vda systemd.mask=systemd-networkd-wait-online.service");
  config->kernel_path = xstrdup("vmlinux");
  config->initrd_path = xstrdup("initrd");
  config->disk_path = xstrdup("disk.img");
  config->cloud_init_enabled = true;
  config->disk_archive = xstrdup("disk.tar.gz");
  config->remove_irqbalance = true;
  config->kernel_compression = VM_BUILD_KERNEL_UNCOMPRESSED;
}

void vm_build_config_free(vm_build_config *config) {
  free(config->type);
  free(config->output_dir);
  free(config->arch);
  free(config->release);
  free(config->version);
  free(config->network);
  free(config->network_ip);
  free(config->cmdline);
  free(config->kernel_path);
  free(config->initrd_path);
  free(config->disk_path);
  free(config->cloud_user);
  free(config->ssh_key);
  free(config->ssh_key_file);
  free(config->cloud_init_path);
  free(config->kernel_url);
  free(config->initrd_url);
  free(config->disk_url);
  free(config->disk_archive);
  free(config->disk_image);
  memset(config, 0, sizeof(*config));
}

static yaml_event_t yaml_next(yaml_parser_t *parser, const char *path) {
  yaml_event_t event;
  if (!yaml_parser_parse(parser, &event)) {
    die("%s:%zu:%zu: YAML parse error: %s", path, parser->problem_mark.line + 1, parser->problem_mark.column + 1,
        parser->problem ?: "unknown error");
  }
  return event;
}

static char *yaml_scalar_dup(const yaml_event_t *event, const char *path) {
  if (event->type != YAML_SCALAR_EVENT) die("%s: expected scalar", path);
  return xstrdup((const char *)event->data.scalar.value);
}

static void yaml_parse_mapping(yaml_parser_t *parser, const char *path, vm_build_config *config, const char *prefix);

static void yaml_parse_value(yaml_parser_t *parser, const char *path, vm_build_config *config, const char *key,
                             yaml_event_t *event) {
  if (event->type == YAML_SCALAR_EVENT) {
    char *value = yaml_scalar_dup(event, path);
    apply_config_value(config, key, value);
    free(value);
    yaml_event_delete(event);
  } else if (event->type == YAML_MAPPING_START_EVENT) {
    yaml_event_delete(event);
    yaml_parse_mapping(parser, path, config, key);
  } else {
    die("%s: unsupported YAML value for %s", path, key);
  }
}

static void yaml_parse_mapping(yaml_parser_t *parser, const char *path, vm_build_config *config, const char *prefix) {
  for (;;) {
    yaml_event_t key_event = yaml_next(parser, path);
    if (key_event.type == YAML_MAPPING_END_EVENT) {
      yaml_event_delete(&key_event);
      return;
    }
    char *name = yaml_scalar_dup(&key_event, path);
    yaml_event_delete(&key_event);
    char *full_key = prefix && *prefix ? xasprintf("%s.%s", prefix, name) : xstrdup(name);
    free(name);

    yaml_event_t value_event = yaml_next(parser, path);
    yaml_parse_value(parser, path, config, full_key, &value_event);
    free(full_key);
  }
}

static void vm_build_config_parse_file(vm_build_config *config, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) die("cannot open %s: %s", path, strerror(errno));

  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) die("cannot initialize YAML parser");
  yaml_parser_set_input_file(&parser, f);

  yaml_event_t event = yaml_next(&parser, path);
  if (event.type != YAML_STREAM_START_EVENT) die("%s: expected YAML stream", path);
  yaml_event_delete(&event);

  event = yaml_next(&parser, path);
  if (event.type != YAML_DOCUMENT_START_EVENT) die("%s: expected YAML document", path);
  yaml_event_delete(&event);

  event = yaml_next(&parser, path);
  if (event.type != YAML_MAPPING_START_EVENT) die("%s: expected top-level mapping", path);
  yaml_event_delete(&event);
  yaml_parse_mapping(&parser, path, config, "");

  event = yaml_next(&parser, path);
  if (event.type != YAML_DOCUMENT_END_EVENT) die("%s: expected end of YAML document", path);
  yaml_event_delete(&event);
  event = yaml_next(&parser, path);
  if (event.type != YAML_STREAM_END_EVENT) die("%s: expected end of YAML stream", path);
  yaml_event_delete(&event);

  yaml_parser_delete(&parser);
  fclose(f);
  if (!config->output_dir) config->output_dir = default_output_dir(path);
}

static void gunzip_file(const char *src, const char *dst) {
  gzFile in = gzopen(src, "rb");
  if (!in) die("gzopen %s failed", src);
  FILE *out = fopen(dst, "wb");
  if (!out) die("open %s: %s", dst, strerror(errno));
  char buf[128 * 1024];
  int n;
  while ((n = gzread(in, buf, sizeof(buf))) > 0) {
    if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) die("write %s failed", dst);
  }
  if (n < 0) die("gzread %s failed", src);
  gzclose(in);
  fclose(out);
}

static bool tar_name_safe(const char *name) {
  return name[0] && name[0] != '/' && !strstr(name, "../") && strcmp(name, "..");
}

static uint64_t tar_octal(const char *field, size_t len) {
  uint64_t value = 0;
  for (size_t i = 0; i < len && field[i]; i++) {
    if (field[i] == ' ') continue;
    if (field[i] < '0' || field[i] > '7') break;
    value = (value << 3) + (uint64_t)(field[i] - '0');
  }
  return value;
}

static void extract_targz_member(const char *path, const char *member, const char *dst) {
  gzFile gz = gzopen(path, "rb");
  if (!gz) die("gzopen %s failed", path);
  bool found = false;
  for (;;) {
    unsigned char header[512];
    int got = gzread(gz, header, sizeof(header));
    if (got == 0) break;
    if (got != 512) die("short tar header in %s", path);
    bool zero = true;
    for (size_t i = 0; i < sizeof(header); i++)
      zero &= header[i] == 0;
    if (zero) break;

    char name[101] = {0};
    memcpy(name, header, 100);
    char type = (char)header[156];
    uint64_t size = tar_octal((const char *)header + 124, 12);
    if (!tar_name_safe(name)) die("unsafe tar path: %s", name);

    FILE *out = NULL;
    if (!found && (!strcmp(name, member)) && (type == '\0' || type == '0')) {
      out = fopen(dst, "wb");
      if (!out) die("extract %s: %s", dst, strerror(errno));
      found = true;
    }

    char buf[64 * 1024];
    uint64_t remaining = size;
    while (remaining > 0) {
      unsigned want = remaining > sizeof(buf) ? sizeof(buf) : (unsigned)remaining;
      int n = gzread(gz, buf, want);
      if (n <= 0) die("short tar file data in %s", path);
      if (out && fwrite(buf, 1, (size_t)n, out) != (size_t)n) die("write %s failed", dst);
      remaining -= (uint64_t)n;
    }
    if (out) fclose(out);
    uint64_t padding = (512 - (size % 512)) % 512;
    while (padding > 0) {
      unsigned want = padding > sizeof(buf) ? sizeof(buf) : (unsigned)padding;
      int n = gzread(gz, buf, want);
      if (n <= 0) die("short tar padding in %s", path);
      padding -= (uint64_t)n;
    }
  }
  gzclose(gz);
  if (!found) die("%s not found in %s", member, path);
}

static void download_if_missing(const char *path, const char *url) {
  if (file_exists(path)) return;
  crprintf("<cyan>downloading</cyan> %s\n", url);
  if (vm_download_file(url, path) != 0) die("download failed: %s", url);
}

static void copy_from_cache_if_missing(const char *dst, const char *cache_path) {
  if (file_exists(dst)) return;
  copy_file(cache_path, dst);
}

static void write_vm_conf(const vm_build_config *config) {
  char *text =
    xasprintf("kernel=%s\n"
              "initrd=%s\n"
              "cmdline=%s\n"
              "cpu-count=%zu\n"
              "memory-size=%d\n"
              "disk=%s\n"
              "network=%s\n"
              "%s"
              "%s%s",
              config->kernel_path, config->initrd_path, config->cmdline, config->cpu_count, config->memory_mb,
              config->disk_path, config->network,
              config->nested_virtualization ? "nested-virtualization=true\n" : "",
              config->cloud_init_enabled ? "cdrom=" : "",
              config->cloud_init_enabled ? NOCLOUD_SEED_PATH "\n" : "");
  write_text_file("leash.conf", text);
  free(text);
}

static char *cloud_init_user(const vm_build_config *config) {
  char *auto_user = NULL;
  const char *user = config->cloud_user;
  if (!user || !strcmp(user, "auto")) {
    auto_user = display_name_for_output(config->output_dir);
    user = auto_user;
  }
  if (!*user || !isalpha((unsigned char)user[0])) die("invalid cloud-init user: %s", user);
  for (const char *p = user; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') die("invalid cloud-init user: %s", user);
  }
  char *copy = xstrdup(user);
  free(auto_user);
  return copy;
}

static char *make_user_data(const vm_build_config *config, const char *user, const char *instance_dir) {
  if (config->cloud_init_path) {
    char *cloud_init_path = expand_home_path(config->cloud_init_path);
    char *text = read_text_file(cloud_init_path, NULL);
    free(cloud_init_path);
    return text;
  }

  char *ssh_key = NULL;
  if (config->ssh_key) {
    ssh_key = xstrdup(config->ssh_key);
  } else if (config->ssh_key_file) {
    char *ssh_pub_path = expand_home_path(config->ssh_key_file);
    ssh_key = read_first_line(ssh_pub_path);
    if (!ssh_key) die("cannot find SSH public key at %s", ssh_pub_path);
    free(ssh_pub_path);
  } else {
    ssh_key = vm_ssh_key_public_line(instance_dir);
  }

  char *irqbalance_cmd = config->remove_irqbalance ? "  - [apt-get, remove, -y, irqbalance]\n" : "";
  char *runcmd = xasprintf("runcmd:\n"
                           "%s"
                           "  - [truncate, -s, '0', /etc/legal]\n"
                           "  - [truncate, -s, '0', /etc/motd]\n"
                           "  - [find, /etc/update-motd.d, -mindepth, '1', -delete]\n"
                           "  - [truncate, -s, '0', /run/motd.dynamic]\n"
                           "  - [install, -o, %s, -g, %s, -m, '0644', /dev/null, /home/%s/.hushlogin]\n",
                           irqbalance_cmd, user, user, user);
  char *text = xasprintf("#cloud-config\n"
                         "bootcmd:\n"
                         "  - [truncate, -s, '0', /etc/legal]\n"
                         "  - [truncate, -s, '0', /etc/motd]\n"
                         "  - [find, /etc/update-motd.d, -mindepth, '1', -delete]\n"
                         "  - [touch, /etc/skel/.hushlogin]\n"
                         "  - [sh, -c, \"sed -i '/^# sudo hint$/,/^fi$/d' /etc/bash.bashrc\"]\n"
                         "write_files:\n"
                         "  - path: /etc/legal\n"
                         "    content: ''\n"
                         "    permissions: '0644'\n"
                         "  - path: /etc/motd\n"
                         "    content: ''\n"
                         "    permissions: '0644'\n"
                         "  - path: /etc/skel/.hushlogin\n"
                         "    content: ''\n"
                         "    permissions: '0644'\n"
                         "users:\n"
                         "  - default\n"
                         "  - name: %s\n"
                         "    lock_passwd: false\n"
                         "    gecos: %s\n"
                         "    groups: [adm, audio, cdrom, dialout, dip, floppy, lxd, netdev, plugdev, sudo, video]\n"
                         "    sudo: [\"ALL=(ALL) NOPASSWD:ALL\"]\n"
                         "    shell: /bin/bash\n"
                         "    ssh_authorized_keys:\n"
                         "      - %s\n"
                         "%s",
                         user, user, ssh_key, runcmd);
  free(runcmd);
  free(ssh_key);
  return text;
}

static bool uses_nat_network(const vm_build_config *config) {
  return config->network && !strcmp(config->network, "nat");
}

static char *default_nat_ip(const char *name) {
  uint32_t hash = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    hash ^= *p;
    hash *= 16777619u;
  }
  return xasprintf("192.168.64.%u", 100u + hash % 100u);
}

static char *configured_network_ip(const vm_build_config *config, const char *hostname) {
  if (!uses_nat_network(config)) return NULL;
  if (!config->network_ip || !strcmp(config->network_ip, "auto")) return default_nat_ip(hostname);
  if (!strcmp(config->network_ip, "dhcp")) return NULL;
  return xstrdup(config->network_ip);
}

static char *make_network_config(const char *ip) {
  if (!ip) {
    return xstrdup("version: 2\n"
                   "ethernets:\n"
                   "  vmnic:\n"
                   "    match:\n"
                   "      name: \"enp*\"\n"
                   "    dhcp4: true\n"
                   "    dhcp6: false\n"
                   "    optional: true\n");
  }

  return xasprintf("version: 2\n"
                   "ethernets:\n"
                   "  vmnic:\n"
                   "    match:\n"
                   "      name: \"enp*\"\n"
                   "    dhcp4: false\n"
                   "    dhcp6: false\n"
                   "    addresses: [%s/24]\n"
                   "    routes:\n"
                   "      - to: default\n"
                   "        via: 192.168.64.1\n"
                   "    nameservers:\n"
                   "      addresses: [192.168.64.1, 1.1.1.1]\n"
                   "    optional: true\n",
                   ip);
}

static void write_nocloud_seed(const vm_build_config *config) {
  if (!config->cloud_init_enabled) {
    unlink(NOCLOUD_SEED_PATH);
    return;
  }
  char *user = config->cloud_init_path ? NULL : cloud_init_user(config);
  char *cwd = absolute_existing_dir(".");
  if (!config->cloud_init_path) vm_ssh_key_ensure(cwd);
  char *hostname = display_name_for_output(config->output_dir);
  char *network_ip = configured_network_ip(config, hostname);
  char *user_data = make_user_data(config, user, cwd);
  char *network_config = make_network_config(network_ip);
  nocloud_write_seed_iso(NOCLOUD_SEED_PATH, hostname, user_data, network_config);
  write_text_file("user-data", user_data);
  char *meta_data = xasprintf("instance-id: %s\nlocal-hostname: %s\n", hostname, hostname);
  write_text_file("meta-data", meta_data);
  write_text_file("network-config", network_config);
  if (network_ip) write_text_file("0.ipaddr", network_ip);
  else unlink("0.ipaddr");
  free(meta_data);
  free(hostname);
  free(network_ip);
  free(network_config);
  free(user_data);
  free(cwd);
  free(user);
}

static void validate_config(const vm_build_config *config, const char *path) {
  if (!config->type) die("%s: missing type", path);
  if (strcmp(config->type, "image")) die("%s: unknown build type: %s", path, config->type);
  if (!config->output_dir || !*config->output_dir) die("%s: missing output", path);
  if (!config->kernel_url) die("%s: missing downloads.kernel_url", path);
  if (!config->initrd_url) die("%s: missing downloads.initrd_url", path);
  if (!config->disk_url) die("%s: missing downloads.disk_url", path);
  if (!config->disk_image) die("%s: missing downloads.disk_image", path);
  if (config->disk_size_mb <= 0) die("%s: invalid disk_size_mb", path);
  if (config->memory_mb <= 0) die("%s: invalid leash.memory_mb", path);
  if (config->cpu_count == 0) die("%s: invalid leash.cpu_count", path);
}

static char *resolve_output_dir(const char *output) {
  if (strchr(output, '/')) return xstrdup(output);
  char *instances = vm_instances_dir();
  char *dir = path_join(instances, output);
  free(instances);
  return dir;
}

static void remove_generated_instance_files(const vm_build_config *config) {
  unlink(config->kernel_path);
  unlink(config->initrd_path);
  unlink(config->disk_path);
  unlink(NOCLOUD_SEED_PATH);
  unlink("user-data");
  unlink("meta-data");
  unlink("network-config");
  unlink("0.ipaddr");
  unlink("leash.conf");
  unlink("README");
}

static double elapsed_seconds(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

typedef struct {
  char *message;
  struct timespec start;
  pthread_t thread;
  atomic_bool running;
  bool spinner;
} build_step;

static void *build_step_spinner(void *arg) {
  build_step *step = arg;
  static const char frames[] = {'-', '\\', '|', '/'};
  size_t frame = 0;
  while (atomic_load(&step->running)) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) memset(&now, 0, sizeof(now));
    crfprintf(stderr, "\r\033[2K<dim>build:</dim> <cyan>%c</cyan> %s <dim>(%.1fs)</dim>",
              frames[frame++ % (sizeof(frames) / sizeof(frames[0]))], step->message, elapsed_seconds(step->start, now));
    fflush(stderr);
    usleep(100000);
  }
  return NULL;
}

static void build_step_begin(build_step *step, const char *fmt, ...) {
  memset(step, 0, sizeof(*step));
  va_list ap;
  va_start(ap, fmt);
  if (vasprintf(&step->message, fmt, ap) < 0 || !step->message) die("out of memory");
  va_end(ap);
  if (clock_gettime(CLOCK_MONOTONIC, &step->start) != 0) memset(&step->start, 0, sizeof(step->start));
  step->spinner = isatty(STDERR_FILENO);
  atomic_init(&step->running, true);
  if (step->spinner) {
    if (pthread_create(&step->thread, NULL, build_step_spinner, step) != 0) step->spinner = false;
  }
  if (!step->spinner) crfprintf(stderr, "<dim>build:</dim> %s...\n", step->message);
}

static void build_step_end(build_step *step) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) memset(&now, 0, sizeof(now));
  if (step->spinner) {
    atomic_store(&step->running, false);
    pthread_join(step->thread, NULL);
    fprintf(stderr, "\r\033[2K");
  }
  crfprintf(stderr, "<dim>build:</dim> %s <dim>(%.3fs)</dim>\n", step->message, elapsed_seconds(step->start, now));
  free(step->message);
}

static int builder_image_build(const char *config_path, const vm_build_config *config, bool force) {
  validate_config(config, config_path);

  char *arch = (!config->arch || !strcmp(config->arch, "auto")) ? host_cloud_arch() : xstrdup(config->arch);
  char *output_dir = resolve_output_dir(config->output_dir);
  ensure_dir_tree(output_dir);
  char *out = absolute_existing_dir(output_dir);
  if (chdir(out) != 0) die("chdir %s: %s", out, strerror(errno));
  free(output_dir);
  free(out);

  char *name = display_name_for_output(config->output_dir);
  build_step step;
  build_step_begin(&step, "creating '%s'", name);
  build_step_end(&step);
  if (force) {
    build_step_begin(&step, "removing generated files");
    remove_generated_instance_files(config);
    build_step_end(&step);
  }

  build_step_begin(&step, "writing leash.conf");
  write_vm_conf(config);
  build_step_end(&step);

  build_step_begin(&step, "resolving download cache");
  char *cache_dir = downloads_cache_dir();
  char *image_cache_dir = images_cache_dir();
  build_step_end(&step);
  crfprintf(stderr, "<dim>build:</dim> cache %s\n", cache_dir);
  crfprintf(stderr, "<dim>build:</dim> image cache %s\n", image_cache_dir);

  char *kernel_url = expand_template(config, config->kernel_url, arch);
  bool kernel_compressed = config->kernel_compression == VM_BUILD_KERNEL_COMPRESSED ||
                           (config->kernel_compression == VM_BUILD_KERNEL_AUTO_NON_AMD64 && strcmp(arch, "amd64"));
  if (kernel_compressed) {
    if (!file_exists(config->kernel_path)) {
      build_step_begin(&step, "installing kernel");
      char *cached_kernel = cache_path_with_suffix(cache_dir, kernel_url, config->kernel_path, ".gz");
      download_if_missing(cached_kernel, kernel_url);
      gunzip_file(cached_kernel, config->kernel_path);
      free(cached_kernel);
      build_step_end(&step);
    }
  } else {
    build_step_begin(&step, "installing kernel");
    char *cached_kernel = url_cache_path(cache_dir, kernel_url, config->kernel_path);
    download_if_missing(cached_kernel, kernel_url);
    copy_from_cache_if_missing(config->kernel_path, cached_kernel);
    free(cached_kernel);
    build_step_end(&step);
  }
  free(kernel_url);

  build_step_begin(&step, "installing initrd");
  char *initrd_url = expand_template(config, config->initrd_url, arch);
  char *cached_initrd = url_cache_path(cache_dir, initrd_url, config->initrd_path);
  download_if_missing(cached_initrd, initrd_url);
  copy_from_cache_if_missing(config->initrd_path, cached_initrd);
  free(cached_initrd);
  free(initrd_url);
  build_step_end(&step);

  char *disk_image = expand_template(config, config->disk_image, arch);
  if (!file_exists(config->disk_path)) {
    char *disk_url = expand_template(config, config->disk_url, arch);
    char *cached_disk = url_cache_path(cache_dir, disk_url, config->disk_archive);
    download_if_missing(cached_disk, disk_url);
    char *cached_disk_image = cache_path_with_suffix(image_cache_dir, disk_url, disk_image, ".img");
    free(disk_url);

    if (!file_exists(cached_disk_image)) {
      build_step_begin(&step, "extracting cached disk image");
      extract_targz_member(cached_disk, disk_image, cached_disk_image);
      build_step_end(&step);
    }
    build_step_begin(&step, "cloning disk image");
    clone_or_copy_file(cached_disk_image, config->disk_path);
    free(cached_disk);
    free(cached_disk_image);
    build_step_end(&step);
  }

  build_step_begin(&step, "writing NoCloud seed");
  write_nocloud_seed(config);
  vm_ssh_config_ensure();
  build_step_end(&step);

  off_t disk_size = (off_t)config->disk_size_mb * 1024 * 1024;
  build_step_begin(&step, "sizing disk to %d MB", config->disk_size_mb);
  if (truncate(config->disk_path, disk_size) != 0) die("truncate %s: %s", config->disk_path, strerror(errno));
  build_step_end(&step);

  free(disk_image);
  free(image_cache_dir);
  free(cache_dir);
  free(arch);
  crprintf("<green>created leash</green> '<bold>%s</bold>'\n", name);
  free(name);
  return 0;
}

static void build_usage(FILE *stream) {
  fprintf(stream, "usage: leash build [--check] [--force] <name|config.yml>\n");
}

static const char *kernel_compression_name(vm_build_kernel_compression compression) {
  switch (compression) {
  case VM_BUILD_KERNEL_AUTO_NON_AMD64:
    return "auto_non_amd64";
  case VM_BUILD_KERNEL_COMPRESSED:
    return "true";
  case VM_BUILD_KERNEL_UNCOMPRESSED:
    return "false";
  }
  return "unknown";
}

static void builder_info_usage(FILE *stream) {
  fprintf(stream, "usage: leash builder <name|config.yml>\n");
}

static char *resolve_builder_config(const char *arg) {
  if (strchr(arg, '/')) return xstrdup(arg);
  if (strstr(arg, ".yml") || strstr(arg, ".yaml")) return xstrdup(arg);
  char *builders = vm_builders_dir();
  char *file = xasprintf("%s/%s.yml", builders, arg);
  free(builders);
  return file;
}

int builder_info_main(int argc, char **argv) {
  crprintf_set_color(isatty(STDOUT_FILENO));
  if (argc != 1 || !strcmp(argv[0], "-h") || !strcmp(argv[0], "--help")) {
    builder_info_usage(argc == 1 ? stdout : stderr);
    return argc == 1 ? 0 : 1;
  }

  builder_ensure_defaults();
  char *config_path = resolve_builder_config(argv[0]);
  vm_build_config config;
  vm_build_config_init(&config);
  vm_build_config_parse_file(&config, config_path);
  validate_config(&config, config_path);

  char *name = display_name_for_output(config.output_dir);
  crprintf("<bold>%s</bold>\n", name);
  printf("  config: %s\n", config_path);
  printf("  output: %s\n", config.output_dir);
  printf("  type: %s\n", config.type ?: "unknown");
  printf("  version: %s (%s)\n", config.version ?: "unknown", config.release ?: "unknown");
  printf("  arch: %s\n", config.arch ?: "auto");
  printf("  cpu: %zu\n", config.cpu_count);
  printf("  memory: %d MB\n", config.memory_mb);
  printf("  disk: %d MB\n", config.disk_size_mb);
  printf("  network: %s", config.network ?: "none");
  if (config.network_ip) printf(" (%s)", config.network_ip);
  putchar('\n');
  printf("  nested virtualization: %s\n", config.nested_virtualization ? "on" : "off");
  printf("  cloud-init: %s", config.cloud_init_enabled ? "on" : "off");
  if (config.cloud_init_enabled) printf(" (user %s)", config.cloud_user ?: "auto");
  putchar('\n');
  printf("  kernel gzip: %s\n", kernel_compression_name(config.kernel_compression));
  printf("  cmdline: %s\n", config.cmdline ?: "");

  free(name);
  vm_build_config_free(&config);
  free(config_path);
  return 0;
}

int builder_build_main(int argc, char **argv) {
  crprintf_set_color(isatty(STDOUT_FILENO));
  bool check_only = false;
  bool force = false;
  const char *config_arg = NULL;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--check")) check_only = true;
    else if (!strcmp(argv[i], "--force")) force = true;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      build_usage(stdout);
      return 0;
    } else if (!config_arg) {
      config_arg = argv[i];
    } else {
      build_usage(stderr);
      return 1;
    }
  }
  if (!config_arg) {
    build_usage(stderr);
    return 1;
  }

  builder_ensure_defaults();
  vm_build_config config;
  vm_build_config_init(&config);
  char *config_path = resolve_builder_config(config_arg);
  vm_build_config_parse_file(&config, config_path);
  int rc = 0;
  if (check_only) validate_config(&config, config_path);
  else rc = builder_image_build(config_path, &config, force);
  free(config_path);
  vm_build_config_free(&config);
  return rc;
}
