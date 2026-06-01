#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  VM_BUILD_KERNEL_AUTO_NON_AMD64 = -1,
  VM_BUILD_KERNEL_UNCOMPRESSED = 0,
  VM_BUILD_KERNEL_COMPRESSED = 1,
} vm_build_kernel_compression;

typedef struct {
  char *type;
  char *output_dir;
  char *arch;
  char *release;
  char *version;
  int disk_size_mb;
  bool customize;

  size_t cpu_count;
  int memory_mb;
  char *network;
  char *network_ip;
  char *cmdline;
  char *kernel_path;
  char *initrd_path;
  char *disk_path;

  bool cloud_init_enabled;
  char *cloud_user;
  char *ssh_key;
  char *ssh_key_file;
  char *cloud_init_path;
  bool remove_irqbalance;

  char *kernel_url;
  char *initrd_url;
  char *disk_url;
  char *disk_archive;
  char *disk_image;
  vm_build_kernel_compression kernel_compression;
} vm_build_config;

void vm_build_config_init(vm_build_config *config);
void vm_build_config_free(vm_build_config *config);

int builder_build_main(int argc, char **argv);
