#pragma once
#include "leash/util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
  VM_BOOT_LINUX,
  VM_BOOT_EFI,
} vm_bootloader_kind;

typedef struct {
  size_t cpu_count;
  uint64_t memory_size;
  uint64_t memory_size_suffix;
  str_list disks;
  str_list cdroms;
  str_list folders;
  str_list networks;
  bool balloon;
  bool nested_virtualization;
  vm_bootloader_kind bootloader;
  char *efi_vars;
  char *kernel;
  char *initrd;
  char *cmdline;
  char *escape_sequence;
  double shutdown_timeout;
} vm_run_options;

void vm_run_options_init(vm_run_options *options);
void vm_run_options_free(vm_run_options *options);
int vm_run_parse_options(vm_run_options *options, int argc, char **argv);
void vm_run_usage(FILE *stream);
