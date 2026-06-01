#include "leash/nocloud.h"
#include "leash/util.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ISO_SECTOR_SIZE 2048
#define ISO_PVD_LBA 16
#define ISO_SVD_LBA 17
#define ISO_VDT_LBA 18
#define ISO_PVD_PATH_LE_LBA 19
#define ISO_PVD_PATH_BE_LBA 20
#define ISO_SVD_PATH_LE_LBA 21
#define ISO_SVD_PATH_BE_LBA 22
#define ISO_PVD_ROOT_LBA 23
#define ISO_SVD_ROOT_LBA 24
#define ISO_FIRST_FILE_LBA 25

typedef struct {
  const char *pvd_name;
  const char *joliet_name;
  const char *data;
  uint32_t size;
  uint32_t lba;
} iso_file;

static void put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void put_both16(uint8_t *p, uint16_t v) {
  put_le16(p, v);
  put_be16(p + 2, v);
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void put_both32(uint8_t *p, uint32_t v) {
  put_le32(p, v);
  put_be32(p + 4, v);
}

static uint32_t sector_count(uint32_t bytes) {
  return (bytes + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
}

static void iso_date7(uint8_t *p) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  p[0] = (uint8_t)(tm.tm_year);
  p[1] = (uint8_t)(tm.tm_mon + 1);
  p[2] = (uint8_t)tm.tm_mday;
  p[3] = (uint8_t)tm.tm_hour;
  p[4] = (uint8_t)tm.tm_min;
  p[5] = (uint8_t)tm.tm_sec;
  p[6] = 0;
}

static void iso_date17(uint8_t *p) {
  time_t now = time(NULL);
  struct tm tm;
  gmtime_r(&now, &tm);
  snprintf((char *)p, 17, "%04d%02d%02d%02d%02d%02d00", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
           tm.tm_min, tm.tm_sec);
}

static size_t dir_record(uint8_t *dst, uint32_t lba, uint32_t size, uint8_t flags, const uint8_t *name,
                         uint8_t name_len) {
  size_t len = 33 + name_len + (name_len % 2 == 0 ? 1 : 0);
  memset(dst, 0, len);
  dst[0] = (uint8_t)len;
  put_both32(dst + 2, lba);
  put_both32(dst + 10, size);
  iso_date7(dst + 18);
  dst[25] = flags;
  put_both16(dst + 28, 1);
  dst[32] = name_len;
  memcpy(dst + 33, name, name_len);
  return len;
}

static size_t ascii_dir_record(uint8_t *dst, uint32_t lba, uint32_t size, uint8_t flags, const char *name) {
  return dir_record(dst, lba, size, flags, (const uint8_t *)name, (uint8_t)strlen(name));
}

static size_t joliet_name(uint8_t *dst, const char *name) {
  size_t len = strlen(name);
  for (size_t i = 0; i < len; i++) {
    dst[i * 2] = 0;
    dst[i * 2 + 1] = (uint8_t)name[i];
  }
  return len * 2;
}

static size_t joliet_dir_record(uint8_t *dst, uint32_t lba, uint32_t size, uint8_t flags, const char *name) {
  uint8_t encoded[256];
  size_t len = joliet_name(encoded, name);
  if (len > UINT8_MAX) die("Joliet file name too long: %s", name);
  return dir_record(dst, lba, size, flags, encoded, (uint8_t)len);
}

static void write_path_table(uint8_t *sector, uint32_t root_lba, bool big_endian) {
  memset(sector, 0, ISO_SECTOR_SIZE);
  sector[0] = 1;
  sector[1] = 0;
  if (big_endian) {
    put_be32(sector + 2, root_lba);
    put_be16(sector + 6, 1);
  } else {
    put_le32(sector + 2, root_lba);
    put_le16(sector + 6, 1);
  }
  sector[8] = 0;
}

static void write_volume_id_ascii(uint8_t *field, const char *label) {
  memset(field, ' ', 32);
  size_t len = strlen(label);
  if (len > 32) len = 32;
  memcpy(field, label, len);
}

static void write_volume_id_joliet(uint8_t *field, const char *label) {
  memset(field, 0, 32);
  size_t len = strlen(label);
  if (len > 16) len = 16;
  for (size_t i = 0; i < len; i++) {
    field[i * 2] = 0;
    field[i * 2 + 1] = (uint8_t)label[i];
  }
}

static void write_descriptor(uint8_t *sector, uint8_t type, uint32_t volume_sectors, uint32_t root_lba,
                             uint32_t path_le_lba, uint32_t path_be_lba, bool joliet) {
  memset(sector, 0, ISO_SECTOR_SIZE);
  sector[0] = type;
  memcpy(sector + 1, "CD001", 5);
  sector[6] = 1;
  if (joliet) {
    sector[7] = 0;
    write_volume_id_joliet(sector + 40, "CIDATA");
    memcpy(sector + 88, "%/@", 3);
  } else {
    write_volume_id_ascii(sector + 40, "CIDATA");
  }
  put_both32(sector + 80, volume_sectors);
  put_both16(sector + 120, 1);
  put_both16(sector + 124, 1);
  put_both16(sector + 128, ISO_SECTOR_SIZE);
  put_both32(sector + 132, 10);
  put_le32(sector + 140, path_le_lba);
  put_be32(sector + 148, path_be_lba);
  uint8_t root_name = 0;
  dir_record(sector + 156, root_lba, ISO_SECTOR_SIZE, 2, &root_name, 1);
  iso_date17(sector + 813);
  iso_date17(sector + 830);
  sector[881] = 1;
}

static void write_root_dir(uint8_t *sector, uint32_t root_lba, iso_file *files, size_t file_count, bool joliet) {
  memset(sector, 0, ISO_SECTOR_SIZE);
  size_t off = 0;
  uint8_t dot = 0;
  uint8_t dotdot = 1;
  off += dir_record(sector + off, root_lba, ISO_SECTOR_SIZE, 2, &dot, 1);
  off += dir_record(sector + off, root_lba, ISO_SECTOR_SIZE, 2, &dotdot, 1);
  for (size_t i = 0; i < file_count; i++) {
    size_t added;
    if (joliet) added = joliet_dir_record(sector + off, files[i].lba, files[i].size, 0, files[i].joliet_name);
    else added = ascii_dir_record(sector + off, files[i].lba, files[i].size, 0, files[i].pvd_name);
    if (off + added > ISO_SECTOR_SIZE) die("NoCloud ISO root directory overflow");
    off += added;
  }
}

static void write_sector(FILE *f, const uint8_t *sector) {
  if (fwrite(sector, 1, ISO_SECTOR_SIZE, f) != ISO_SECTOR_SIZE) die("write ISO failed");
}

static void write_file_sectors(FILE *f, const char *data, uint32_t size) {
  uint8_t sector[ISO_SECTOR_SIZE];
  uint32_t remaining = size;
  const uint8_t *p = (const uint8_t *)data;
  do {
    memset(sector, 0, sizeof(sector));
    uint32_t n = remaining > ISO_SECTOR_SIZE ? ISO_SECTOR_SIZE : remaining;
    if (n) memcpy(sector, p, n);
    write_sector(f, sector);
    p += n;
    remaining -= n;
  } while (remaining > 0);
}

int nocloud_write_seed_iso(const char *path, const char *hostname, const char *user_data, const char *network_config) {
  char *meta_data = xasprintf("instance-id: %s\nlocal-hostname: %s\n", hostname, hostname);
  iso_file files[] = {
    {.pvd_name = "USERDATA.;1", .joliet_name = "user-data", .data = user_data, .size = (uint32_t)strlen(user_data)},
    {.pvd_name = "METADATA.;1", .joliet_name = "meta-data", .data = meta_data, .size = (uint32_t)strlen(meta_data)},
    {.pvd_name = "NETCFG.;1",
     .joliet_name = "network-config",
     .data = network_config,
     .size = (uint32_t)strlen(network_config)},
  };
  uint32_t lba = ISO_FIRST_FILE_LBA;
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    files[i].lba = lba;
    lba += sector_count(files[i].size);
  }

  FILE *f = fopen(path, "wb");
  if (!f) die("create %s: %s", path, strerror(errno));

  uint8_t sector[ISO_SECTOR_SIZE];
  for (uint32_t i = 0; i < ISO_PVD_LBA; i++) {
    memset(sector, 0, sizeof(sector));
    write_sector(f, sector);
  }

  write_descriptor(sector, 1, lba, ISO_PVD_ROOT_LBA, ISO_PVD_PATH_LE_LBA, ISO_PVD_PATH_BE_LBA, false);
  write_sector(f, sector);
  write_descriptor(sector, 2, lba, ISO_SVD_ROOT_LBA, ISO_SVD_PATH_LE_LBA, ISO_SVD_PATH_BE_LBA, true);
  write_sector(f, sector);

  memset(sector, 0, sizeof(sector));
  sector[0] = 255;
  memcpy(sector + 1, "CD001", 5);
  sector[6] = 1;
  write_sector(f, sector);

  write_path_table(sector, ISO_PVD_ROOT_LBA, false);
  write_sector(f, sector);
  write_path_table(sector, ISO_PVD_ROOT_LBA, true);
  write_sector(f, sector);
  write_path_table(sector, ISO_SVD_ROOT_LBA, false);
  write_sector(f, sector);
  write_path_table(sector, ISO_SVD_ROOT_LBA, true);
  write_sector(f, sector);
  write_root_dir(sector, ISO_PVD_ROOT_LBA, files, sizeof(files) / sizeof(files[0]), false);
  write_sector(f, sector);
  write_root_dir(sector, ISO_SVD_ROOT_LBA, files, sizeof(files) / sizeof(files[0]), true);
  write_sector(f, sector);

  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    write_file_sectors(f, files[i].data, files[i].size);

  if (fclose(f) != 0) die("close %s: %s", path, strerror(errno));
  free(meta_data);
  return 0;
}
