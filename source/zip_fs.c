/* zip_fs.c -- transparent zip-backed file I/O using minizip
 *
 * Copyright (C) 2026 givethesourceplox
 *
 * Opens assets/data_*.zip at init, then serves fopen() requests by
 * extracting matching entries to memory and returning a FILE* via funopen().
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <minizip/unzip.h>

#include "zip_fs.h"
#include "util.h"

#define MAX_ZIPS 8

static unzFile s_zips[MAX_ZIPS];
static int s_zip_count = 0;
static pthread_mutex_t s_zip_mutex = PTHREAD_MUTEX_INITIALIZER;

// Memory-backed FILE* via funopen
typedef struct
{
  uint8_t *buf;
  size_t size;
  size_t pos;
} ZipMem;

static int zmf_read(void *cookie, char *out, size_t n)
{
  ZipMem *m = (ZipMem *)cookie;
  size_t avail = m->size - m->pos;
  if ((size_t)n > avail)
    n = (int)avail;
  if (n > 0)
  {
    memcpy(out, m->buf + m->pos, n);
    m->pos += n;
  }
  return n;
}

static fpos_t zmf_seek(void *cookie, fpos_t offset, int whence)
{
  ZipMem *m = (ZipMem *)cookie;
  fpos_t newpos;
  switch (whence)
  {
  case SEEK_SET:
    newpos = offset;
    break;
  case SEEK_CUR:
    newpos = (fpos_t)m->pos + offset;
    break;
  case SEEK_END:
    newpos = (fpos_t)m->size + offset;
    break;
  default:
    return -1;
  }
  if (newpos < 0 || (size_t)newpos > m->size)
    return -1;
  m->pos = (size_t)newpos;
  return newpos;
}

static int zmf_close(void *cookie)
{
  ZipMem *m = (ZipMem *)cookie;
  free(m->buf);
  free(m);
  return 0;
}

int zip_fs_init(void)
{
  char path[256];
  s_zip_count = 0;

  for (int i = 0; i < MAX_ZIPS; i++)
  {
    snprintf(path, sizeof(path), "assets/data_%d.zip", i);
    unzFile zf = unzOpen(path);
    if (!zf)
      break;
    s_zips[s_zip_count++] = zf;
    debugPrintf("zip_fs: opened %s\n", path);
  }

  debugPrintf("zip_fs: %d archives loaded\n", s_zip_count);
  return s_zip_count;
}

FILE *zip_fs_fopen(const char *path)
{
  if (!path || s_zip_count == 0)
    return NULL;

  // Strip leading "assets/" if present — zip entries don't have it
  const char *lookup = path;
  if (strncmp(path, "assets/", 7) == 0)
    lookup = path + 7;

  pthread_mutex_lock(&s_zip_mutex);

  for (int i = 0; i < s_zip_count; i++)
  {
    if (unzLocateFile(s_zips[i], lookup, 1) != UNZ_OK)
      continue;

    unz_file_info info;
    if (unzGetCurrentFileInfo(s_zips[i], &info, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK)
      continue;

    if (unzOpenCurrentFile(s_zips[i]) != UNZ_OK)
      continue;

    uint8_t *buf = malloc(info.uncompressed_size);
    if (!buf)
    {
      unzCloseCurrentFile(s_zips[i]);
      continue;
    }

    int rd = unzReadCurrentFile(s_zips[i], buf, info.uncompressed_size);
    unzCloseCurrentFile(s_zips[i]);

    if (rd != (int)info.uncompressed_size)
    {
      free(buf);
      continue;
    }

    pthread_mutex_unlock(&s_zip_mutex);

    ZipMem *m = calloc(1, sizeof(ZipMem));
    m->buf = buf;
    m->size = info.uncompressed_size;
    m->pos = 0;

    FILE *f = funopen(m, zmf_read, NULL, zmf_seek, zmf_close);
    if (!f)
    {
      free(buf);
      free(m);
      return NULL;
    }

    debugPrintf("zip_fs: served \"%s\" (%u bytes) from data_%d.zip\n",
                lookup, (unsigned)info.uncompressed_size, i);
    return f;
  }

  pthread_mutex_unlock(&s_zip_mutex);
  return NULL;
}

int zip_fs_open(const char *path, int flags)
{
  if (!path || (flags & O_WRONLY) || (flags & O_RDWR))
    return -1;

  FILE *f = zip_fs_fopen(path);
  if (!f)
    return -1;

  // Extract to a temp file on sdmc and return its fd
  static int tmp_id = 0;
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "sdmc:/switch/bully/.tmp_%d", tmp_id++);
  int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
  {
    fclose(f);
    return -1;
  }

  char buf[4096];
  int n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    write(fd, buf, n);
  fclose(f);
  unlink(tmp);
  lseek(fd, 0, SEEK_SET);
  return fd;
}
