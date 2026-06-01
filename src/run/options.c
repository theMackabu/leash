#include "leash/run/options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *take_value(int argc, char **argv, int *i, const char *arg, const char *inline_value) {
  if (inline_value) return xstrdup(inline_value);
  if (*i + 1 >= argc) die("missing value for %s", arg);
  *i += 1;
  return xstrdup(argv[*i]);
}

static uint64_t parse_u64(const char *s, const char *name) {
  char *end = NULL;
  unsigned long long value = strtoull(s, &end, 10);
  if (!*s || *end) die("invalid value for %s: %s", name, s);
  return (uint64_t)value;
}

static double parse_double_value(const char *s, const char *name) {
  char *end = NULL;
  double value = strtod(s, &end);
  if (!*s || *end || value < 0) die("invalid value for %s: %s", name, s);
  return value;
}

static bool parse_bool_value(const char *s, const char *name) {
  if (!strcmp(s, "true") || !strcmp(s, "yes") || !strcmp(s, "1")) return true;
  if (!strcmp(s, "false") || !strcmp(s, "no") || !strcmp(s, "0")) return false;
  die("invalid value for %s: %s", name, s);
}

static uint64_t parse_suffix(const char *s) {
  if (!strcmp(s, "none")) return 1;
  if (!strcmp(s, "KB")) return 1000;
  if (!strcmp(s, "KiB")) return 0x400;
  if (!strcmp(s, "MB")) return 1000000;
  if (!strcmp(s, "MiB")) return 0x100000;
  if (!strcmp(s, "GB")) return 1000000000;
  if (!strcmp(s, "GiB")) return 0x40000000;
  die("invalid memory-size-suffix: %s", s);
}

void vm_run_options_init(vm_run_options *options) {
  options->cpu_count = 1;
  options->memory_size = 512;
  options->memory_size_suffix = 0x100000;
  str_list_init(&options->disks);
  str_list_init(&options->cdroms);
  str_list_init(&options->folders);
  str_list_init(&options->networks);
  options->balloon = true;
  options->bootloader = VM_BOOT_LINUX;
  options->efi_vars = NULL;
  options->kernel = NULL;
  options->initrd = NULL;
  options->cmdline = NULL;
  options->escape_sequence = xstrdup("q");
  options->shutdown_timeout = 120.0;
}

void vm_run_options_free(vm_run_options *options) {
  str_list_free(&options->disks);
  str_list_free(&options->cdroms);
  str_list_free(&options->folders);
  str_list_free(&options->networks);
  free(options->efi_vars);
  free(options->kernel);
  free(options->initrd);
  free(options->cmdline);
  free(options->escape_sequence);
}

void vm_run_usage(FILE *stream) {
  fprintf(stream, "usage: leash run [options]\n"
                  "\n"
                  "  -c, --cpu-count N\n"
                  "  -m, --memory-size N\n"
                  "      --memory-size-suffix none|KB|KiB|MB|MiB|GB|GiB\n"
                  "  -d, --disk PATH\n"
                  "      --cdrom PATH\n"
                  "  -f, --folder PATH[:TAG[:ro]]\n"
                  "  -n, --network nat|[MAC@]IFACE\n"
                  "      --balloon true|false\n"
                  "  -b, --bootloader linux|efi\n"
                  "  -e, --efi-vars PATH\n"
                  "  -k, --kernel PATH\n"
                  "      --initrd PATH\n"
                  "      --cmdline CMDLINE\n"
                  "      --escape-sequence SEQ\n"
                  "      --shutdown-timeout SECONDS\n");
}

int vm_run_parse_options(vm_run_options *options, int argc, char **argv) {
  for (int i = 0; i < argc; i++) {
    const char *raw = argv[i];
    const char *arg = raw;
    const char *inline_value = NULL;
    char *arg_copy = NULL;

    if (!strncmp(raw, "--", 2)) {
      const char *eq = strchr(raw, '=');
      if (eq) {
        arg_copy = strndup(raw, (size_t)(eq - raw));
        if (!arg_copy) die("out of memory");
        arg = arg_copy;
        inline_value = eq + 1;
      }
    }

    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      vm_run_usage(stdout);
      free(arg_copy);
      return 1;
    } else if (!strcmp(arg, "-c") || !strcmp(arg, "--cpu-count")) {
      char *v = take_value(argc, argv, &i, arg, inline_value);
      options->cpu_count = (size_t)parse_u64(v, arg);
      free(v);
    } else if (!strcmp(arg, "-m") || !strcmp(arg, "--memory-size")) {
      char *v = take_value(argc, argv, &i, arg, inline_value);
      options->memory_size = parse_u64(v, arg);
      free(v);
    } else if (!strcmp(arg, "--memory-size-suffix")) {
      char *v = take_value(argc, argv, &i, arg, inline_value);
      options->memory_size_suffix = parse_suffix(v);
      free(v);
    } else if (!strcmp(arg, "-d") || !strcmp(arg, "--disk")) {
      char *v = take_value(argc, argv, &i, arg, inline_value);
      str_list_push(&options->disks, v);
      free(v);
    } else if (!strcmp(arg, "--cdrom")) {
      char *v = take_value(argc, argv, &i, arg, inline_value);
      str_list_push(&options->cdroms, v);
      free(v);
    } else if (!strcmp(arg, "-f") || !strcmp(arg, "--folder")) {
      char *v = take_value(argc, argv, &i, arg, inline_value);
      str_list_push(&options->folders, v);
      free(v);
    } else if (!strcmp(arg, "-n") || !strcmp(arg, "--network")) {
      char *v = take_value(argc, argv, &i, arg, inline_value);
      str_list_push(&options->networks, v);
      free(v);
    } else if (!strcmp(arg, "--balloon")) {
      char *v = take_value(argc, argv, &i, arg, inline_value);
      options->balloon = parse_bool_value(v, arg);
      free(v);
    } else if (!strcmp(arg, "-b") || !strcmp(arg, "--bootloader")) {
      char *v = take_value(argc, argv, &i, arg, inline_value);
      if (!strcmp(v, "linux")) options->bootloader = VM_BOOT_LINUX;
      else if (!strcmp(v, "efi")) options->bootloader = VM_BOOT_EFI;
      else die("invalid bootloader: %s", v);
      free(v);
    } else if (!strcmp(arg, "-e") || !strcmp(arg, "--efi-vars")) {
      free(options->efi_vars);
      options->efi_vars = take_value(argc, argv, &i, arg, inline_value);
    } else if (!strcmp(arg, "-k") || !strcmp(arg, "--kernel")) {
      free(options->kernel);
      options->kernel = take_value(argc, argv, &i, arg, inline_value);
    } else if (!strcmp(arg, "--initrd")) {
      free(options->initrd);
      options->initrd = take_value(argc, argv, &i, arg, inline_value);
    } else if (!strcmp(arg, "--cmdline")) {
      free(options->cmdline);
      options->cmdline = take_value(argc, argv, &i, arg, inline_value);
    } else if (!strcmp(arg, "--escape-sequence")) {
      free(options->escape_sequence);
      options->escape_sequence = take_value(argc, argv, &i, arg, inline_value);
    } else if (!strcmp(arg, "--shutdown-timeout")) {
      char *v = take_value(argc, argv, &i, arg, inline_value);
      options->shutdown_timeout = parse_double_value(v, arg);
      free(v);
    } else {
      die("unknown run argument: %s", raw);
    }
    free(arg_copy);
  }
  if (options->networks.count == 0) str_list_push(&options->networks, "nat");
  return 0;
}
