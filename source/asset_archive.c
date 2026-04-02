/* asset_archive.c -- indexed asset resolver for Bully data zips and IMG packs
 *
 * Copyright (C) 2026 givethesourceplox
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "asset_archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "util.h"

#define ASSET_ROOT "assets/"
#define IMG_BLOCK_SIZE 2048u
#define HANDLE_MAGIC 0x4153484Eu

typedef struct
{
  char *path;
  char zip_name[16];
  uint32_t data_offset;
  uint32_t size;
} ZipIndexEntry;

typedef struct
{
  char name[25];
  uint32_t start_block;
  uint32_t block_count;
} ImgDirEntry;

typedef struct
{
  const char *logical_prefix;
  const char *dir_path;
  const char *img_path;
  char zip_name[16];
  uint32_t img_data_offset;
  uint32_t img_size;
  ImgDirEntry *entries;
  size_t entry_count;
} ImgPack;

typedef enum
{
  ASSET_HANDLE_FILE = 0,
  ASSET_HANDLE_SLICE = 1,
} AssetHandleKind;

typedef struct
{
  uint32_t magic;
  AssetHandleKind kind;
  FILE *fp;
  long start;
  long size;
  long pos;
} AssetHandle;

static ZipIndexEntry *g_zip_entries;
static size_t g_zip_entry_count;
static size_t g_zip_entry_cap;

static ImgPack g_img_packs[] = {
    {"act/", "bullyorig/act/act.dir", "bullyorig/act/act.img", "", 0, 0, NULL, 0},
    {"scripts/", "bullyorig/scripts/scripts.dir", "bullyorig/scripts/scripts.img", "", 0, 0, NULL, 0},
    {"dat/", "bullyorig/dat/Trigger.dir", "bullyorig/dat/Trigger.img", "", 0, 0, NULL, 0},
    {"cuts/", "bullyorig/cuts/Cuts.dir", "bullyorig/cuts/Cuts.img", "", 0, 0, NULL, 0},
    {"stream/", "bullyorig/stream/world.dir", "bullyorig/stream/world.img", "", 0, 0, NULL, 0},
    {"objects/", "bullyorig/objects/ide.dir", "bullyorig/objects/ide.img", "", 0, 0, NULL, 0},
};

static const char *g_idx_files[] = {
    "data_0.zip.idx",
    "data_1.zip.idx",
    "data_2.zip.idx",
    "data_3.zip.idx",
    "data_4.zip.idx",
};

static int g_asset_archive_ready;
static int g_asset_archive_attempted;

static uint32_t read_u32le(const unsigned char *ptr)
{
  return ((uint32_t)ptr[0]) |
         ((uint32_t)ptr[1] << 8) |
         ((uint32_t)ptr[2] << 16) |
         ((uint32_t)ptr[3] << 24);
}

static uint16_t read_u16le(const unsigned char *ptr)
{
  return (uint16_t)(((uint16_t)ptr[0]) | ((uint16_t)ptr[1] << 8));
}

static void normalize_path(char *dst, size_t dst_size, const char *src)
{
  size_t j = 0;

  if (!dst || dst_size == 0)
    return;

  if (!src)
  {
    dst[0] = '\0';
    return;
  }

  while ((*src == '.' && src[1] == '/') || *src == '/')
    src += (*src == '/') ? 1 : 2;

  while (*src && j + 1 < dst_size)
  {
    char c = *src++;
    if (c == '\\')
      c = '/';
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    dst[j++] = c;
  }

  dst[j] = '\0';
}

static const char *path_basename(const char *path)
{
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static int add_zip_index_alias(const char *zip_name, const char *path, uint32_t data_offset, uint32_t size)
{
  ZipIndexEntry *entry;
  size_t len;

  if (!path || !path[0])
    return 0;

  if (g_zip_entry_count == g_zip_entry_cap)
  {
    size_t new_cap = (g_zip_entry_cap == 0) ? 512 : g_zip_entry_cap * 2;
    ZipIndexEntry *new_entries = realloc(g_zip_entries, new_cap * sizeof(*new_entries));
    if (!new_entries)
      return -1;
    g_zip_entries = new_entries;
    g_zip_entry_cap = new_cap;
  }

  entry = &g_zip_entries[g_zip_entry_count++];
  len = strlen(path);
  entry->path = malloc(len + 1);
  if (!entry->path)
  {
    g_zip_entry_count--;
    return -1;
  }
  memcpy(entry->path, path, len + 1);
  memset(entry->zip_name, 0, sizeof(entry->zip_name));
  strncpy(entry->zip_name, zip_name, sizeof(entry->zip_name) - 1);
  entry->data_offset = data_offset;
  entry->size = size;
  return 0;
}

static int load_zip_index_file(const char *idx_name)
{
  char fullpath[128];
  char zip_name[32];
  FILE *fp;
  long file_size;
  unsigned char *data;
  uint32_t entry_count;
  size_t pos = 4;
  uint32_t i;

  snprintf(fullpath, sizeof(fullpath), ASSET_ROOT "%s", idx_name);
  fp = fopen(fullpath, "rb");
  if (!fp)
  {
    debugPrintf("asset_archive: missing %s\n", fullpath);
    return -1;
  }

  fseek(fp, 0, SEEK_END);
  file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (file_size < 4)
  {
    fclose(fp);
    return -1;
  }

  data = malloc((size_t)file_size);
  if (!data)
  {
    fclose(fp);
    return -1;
  }

  if (fread(data, 1, (size_t)file_size, fp) != (size_t)file_size)
  {
    free(data);
    fclose(fp);
    return -1;
  }
  fclose(fp);

  memset(zip_name, 0, sizeof(zip_name));
  strncpy(zip_name, idx_name, sizeof(zip_name) - 1);
  {
    char *dot = strstr(zip_name, ".idx");
    if (dot)
      *dot = '\0';
  }

  entry_count = read_u32le(data);
  for (i = 0; i < entry_count; i++)
  {
    uint32_t data_offset;
    uint32_t size;
    uint16_t name_len;
    char path_buf[512];
    char alias_buf[512];

    if (pos + 10 > (size_t)file_size)
      break;

    data_offset = read_u32le(&data[pos + 0]);
    size = read_u32le(&data[pos + 4]);
    name_len = read_u16le(&data[pos + 8]);
    pos += 10;

    if (pos + name_len > (size_t)file_size || name_len >= sizeof(path_buf))
      break;

    memcpy(path_buf, &data[pos], name_len);
    path_buf[name_len] = '\0';
    pos += name_len;

    normalize_path(alias_buf, sizeof(alias_buf), path_buf);
    if (add_zip_index_alias(zip_name, alias_buf, data_offset, size) < 0)
      break;

    if (strncmp(alias_buf, "bullyorig/", 10) == 0)
    {
      if (add_zip_index_alias(zip_name, alias_buf + 10, data_offset, size) < 0)
        break;
    }
    else if (strncmp(alias_buf, "bully/", 6) == 0)
    {
      if (add_zip_index_alias(zip_name, alias_buf + 6, data_offset, size) < 0)
        break;
    }
  }

  free(data);
  return 0;
}

static const ZipIndexEntry *find_zip_entry(const char *normalized_path)
{
  size_t i;
  for (i = 0; i < g_zip_entry_count; i++)
  {
    if (strcmp(g_zip_entries[i].path, normalized_path) == 0)
      return &g_zip_entries[i];
  }
  return NULL;
}

static AssetHandle *asset_handle_from_file(FILE *fp)
{
  AssetHandle *handle;
  if (!fp)
    return NULL;
  handle = calloc(1, sizeof(*handle));
  if (!handle)
  {
    fclose(fp);
    return NULL;
  }
  handle->magic = HANDLE_MAGIC;
  handle->kind = ASSET_HANDLE_FILE;
  handle->fp = fp;
  return handle;
}

static AssetHandle *asset_handle_from_slice(const char *zip_name, uint32_t start, uint32_t size)
{
  char fullpath[128];
  FILE *fp;
  AssetHandle *handle;

  snprintf(fullpath, sizeof(fullpath), ASSET_ROOT "%s", zip_name);
  fp = fopen(fullpath, "rb");
  if (!fp)
    return NULL;

  handle = calloc(1, sizeof(*handle));
  if (!handle)
  {
    fclose(fp);
    return NULL;
  }

  handle->magic = HANDLE_MAGIC;
  handle->kind = ASSET_HANDLE_SLICE;
  handle->fp = fp;
  handle->start = (long)start;
  handle->size = (long)size;
  handle->pos = 0;
  return handle;
}

static int load_img_pack(ImgPack *pack)
{
  const ZipIndexEntry *dir_entry;
  const ZipIndexEntry *img_entry;
  AssetHandle *dir_handle;
  unsigned char *dir_data;
  size_t count;
  size_t i;

  dir_entry = find_zip_entry(pack->dir_path);
  img_entry = find_zip_entry(pack->img_path);
  if (!dir_entry || !img_entry)
    return -1;

  dir_handle = asset_handle_from_slice(dir_entry->zip_name, dir_entry->data_offset, dir_entry->size);
  if (!dir_handle)
    return -1;

  dir_data = malloc(dir_entry->size);
  if (!dir_data)
  {
    asset_close(dir_handle);
    return -1;
  }

  if (asset_read(dir_data, 1, dir_entry->size, dir_handle) != dir_entry->size)
  {
    free(dir_data);
    asset_close(dir_handle);
    return -1;
  }
  asset_close(dir_handle);

  count = dir_entry->size / 32;
  pack->entries = calloc(count, sizeof(*pack->entries));
  if (!pack->entries)
  {
    free(dir_data);
    return -1;
  }

  pack->entry_count = count;
  memset(pack->zip_name, 0, sizeof(pack->zip_name));
  snprintf(pack->zip_name, sizeof(pack->zip_name), "%s", img_entry->zip_name);
  pack->img_data_offset = img_entry->data_offset;
  pack->img_size = img_entry->size;

  for (i = 0; i < count; i++)
  {
    size_t off = i * 32;
    char name_buf[32];

    pack->entries[i].start_block = read_u32le(&dir_data[off + 0]);
    pack->entries[i].block_count = read_u32le(&dir_data[off + 4]);
    memcpy(name_buf, &dir_data[off + 8], 24);
    name_buf[24] = '\0';
    normalize_path(pack->entries[i].name, sizeof(pack->entries[i].name), name_buf);
  }

  free(dir_data);
  return 0;
}

int asset_archive_init(void)
{
  size_t i;

  if (g_asset_archive_attempted)
    return g_asset_archive_ready;

  g_asset_archive_attempted = 1;

  for (i = 0; i < sizeof(g_idx_files) / sizeof(g_idx_files[0]); i++)
    load_zip_index_file(g_idx_files[i]);

  for (i = 0; i < sizeof(g_img_packs) / sizeof(g_img_packs[0]); i++)
    load_img_pack(&g_img_packs[i]);

  g_asset_archive_ready = 1;
  debugPrintf("asset_archive: indexed %zu zip aliases\n", g_zip_entry_count);
  for (i = 0; i < sizeof(g_img_packs) / sizeof(g_img_packs[0]); i++)
  {
    if (g_img_packs[i].entry_count)
    {
      debugPrintf("asset_archive: pack %s entries=%zu zip=%s\n",
                  g_img_packs[i].logical_prefix,
                  g_img_packs[i].entry_count,
                  g_img_packs[i].zip_name);
    }
  }
  return g_asset_archive_ready;
}

static AssetHandle *open_loose_asset(const char *path, const char *normalized_path)
{
  char fullpath[768];
  FILE *fp;

  snprintf(fullpath, sizeof(fullpath), ASSET_ROOT "%s", path);
  fp = fopen(fullpath, "rb");
  if (fp)
    return asset_handle_from_file(fp);

  if (strcmp(path, normalized_path) != 0)
  {
    snprintf(fullpath, sizeof(fullpath), ASSET_ROOT "%s", normalized_path);
    fp = fopen(fullpath, "rb");
    if (fp)
      return asset_handle_from_file(fp);
  }

  return NULL;
}

static AssetHandle *open_zip_asset(const char *normalized_path)
{
  const ZipIndexEntry *entry = find_zip_entry(normalized_path);
  if (!entry)
    return NULL;

  return asset_handle_from_slice(entry->zip_name, entry->data_offset, entry->size);
}

static const ImgDirEntry *find_img_entry(const ImgPack *pack, const char *name)
{
  size_t i;
  for (i = 0; i < pack->entry_count; i++)
  {
    if (strcmp(pack->entries[i].name, name) == 0)
      return &pack->entries[i];
  }
  return NULL;
}

static AssetHandle *open_img_pack_asset(const char *path, const char *normalized_path)
{
  const char *basename = path_basename(normalized_path);
  const ImgPack *fallback_pack = NULL;
  const ImgDirEntry *fallback_entry = NULL;
  size_t i;

  for (i = 0; i < sizeof(g_img_packs) / sizeof(g_img_packs[0]); i++)
  {
    const ImgPack *pack = &g_img_packs[i];
    const ImgDirEntry *entry;

    if (pack->entry_count == 0)
      continue;

    if (strncmp(normalized_path, pack->logical_prefix, strlen(pack->logical_prefix)) != 0)
      continue;

    entry = find_img_entry(pack, basename);
    if (!entry)
      return NULL;

    debugPrintf("asset_archive: %s -> %s!%s [%u blocks @ %u]\n",
                path,
                pack->zip_name,
                pack->img_path,
                entry->block_count,
                entry->start_block);
    return asset_handle_from_slice(pack->zip_name,
                                   pack->img_data_offset + entry->start_block * IMG_BLOCK_SIZE,
                                   entry->block_count * IMG_BLOCK_SIZE);
  }

  for (i = 0; i < sizeof(g_img_packs) / sizeof(g_img_packs[0]); i++)
  {
    const ImgPack *pack = &g_img_packs[i];
    const ImgDirEntry *entry;

    if (pack->entry_count == 0)
      continue;

    entry = find_img_entry(pack, basename);
    if (!entry)
      continue;

    fallback_pack = pack;
    fallback_entry = entry;
    break;
  }

  if (!fallback_pack || !fallback_entry)
    return NULL;

  debugPrintf("asset_archive: %s -> %s!%s [%u blocks @ %u] (basename fallback)\n",
              path,
              fallback_pack->zip_name,
              fallback_pack->img_path,
              fallback_entry->block_count,
              fallback_entry->start_block);
  return asset_handle_from_slice(fallback_pack->zip_name,
                                 fallback_pack->img_data_offset + fallback_entry->start_block * IMG_BLOCK_SIZE,
                                 fallback_entry->block_count * IMG_BLOCK_SIZE);
}

void *asset_open(const char *path)
{
  AssetHandle *handle;
  char normalized_path[512];

  if (!path || !path[0])
    return NULL;

  asset_archive_init();
  normalize_path(normalized_path, sizeof(normalized_path), path);

  handle = open_loose_asset(path, normalized_path);
  if (handle)
    return handle;

  handle = open_zip_asset(normalized_path);
  if (handle)
    return handle;

  handle = open_img_pack_asset(path, normalized_path);
  if (handle)
    return handle;

  return NULL;
}

void asset_close(void *opaque)
{
  AssetHandle *handle = (AssetHandle *)opaque;
  if (!handle || handle->magic != HANDLE_MAGIC)
    return;
  if (handle->fp)
    fclose(handle->fp);
  free(handle);
}

size_t asset_read(void *buf, size_t size, size_t nmemb, void *opaque)
{
  AssetHandle *handle = (AssetHandle *)opaque;

  if (!handle || handle->magic != HANDLE_MAGIC || !buf || size == 0 || nmemb == 0)
    return 0;

  if (handle->kind == ASSET_HANDLE_FILE)
    return fread(buf, size, nmemb, handle->fp);

  if (handle->pos >= handle->size)
    return 0;

  {
    long remaining = handle->size - handle->pos;
    size_t total = size * nmemb;
    size_t to_read = (size_t)((remaining < (long)total) ? remaining : (long)total);
    size_t bytes_read;

    if (fseek(handle->fp, handle->start + handle->pos, SEEK_SET) != 0)
      return 0;

    bytes_read = fread(buf, 1, to_read, handle->fp);
    handle->pos += (long)bytes_read;
    return bytes_read / size;
  }
}

int asset_seek(void *opaque, long offset, int whence)
{
  AssetHandle *handle = (AssetHandle *)opaque;
  long new_pos;

  if (!handle || handle->magic != HANDLE_MAGIC)
    return -1;

  if (handle->kind == ASSET_HANDLE_FILE)
    return fseek(handle->fp, offset, whence);

  switch (whence)
  {
  case SEEK_SET:
    new_pos = offset;
    break;
  case SEEK_CUR:
    new_pos = handle->pos + offset;
    break;
  case SEEK_END:
    new_pos = handle->size + offset;
    break;
  default:
    return -1;
  }

  if (new_pos < 0)
    return -1;

  handle->pos = new_pos;
  return 0;
}

long asset_tell(void *opaque)
{
  AssetHandle *handle = (AssetHandle *)opaque;
  if (!handle || handle->magic != HANDLE_MAGIC)
    return -1;
  if (handle->kind == ASSET_HANDLE_FILE)
    return ftell(handle->fp);
  return handle->pos;
}

long asset_size(void *opaque)
{
  AssetHandle *handle = (AssetHandle *)opaque;
  long cur;
  long size;

  if (!handle || handle->magic != HANDLE_MAGIC)
    return 0;

  if (handle->kind == ASSET_HANDLE_SLICE)
    return handle->size;

  cur = ftell(handle->fp);
  fseek(handle->fp, 0, SEEK_END);
  size = ftell(handle->fp);
  fseek(handle->fp, cur, SEEK_SET);
  return size;
}

int asset_eof(void *opaque)
{
  AssetHandle *handle = (AssetHandle *)opaque;
  if (!handle || handle->magic != HANDLE_MAGIC)
    return 1;
  if (handle->kind == ASSET_HANDLE_FILE)
    return feof(handle->fp);
  return (handle->pos >= handle->size) ? 1 : 0;
}

int asset_getc(void *opaque)
{
  unsigned char c;
  if (asset_read(&c, 1, 1, opaque) != 1)
    return -1;
  return c;
}

char *asset_gets(char *buf, int max, void *opaque)
{
  int i;

  if (!buf || max <= 0)
    return NULL;

  for (i = 0; i < max - 1; i++)
  {
    int c = asset_getc(opaque);
    if (c < 0)
      break;
    buf[i] = (char)c;
    if (c == '\n')
    {
      i++;
      break;
    }
  }

  if (i == 0)
    return NULL;

  buf[i] = '\0';
  return buf;
}
