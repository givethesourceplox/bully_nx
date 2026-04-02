/* asset_archive.h -- indexed asset resolver for Bully data zips and IMG packs
 *
 * Copyright (C) 2026 givethesourceplox
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ASSET_ARCHIVE_H__
#define __ASSET_ARCHIVE_H__

#include <stddef.h>

int asset_archive_init(void);

void *asset_open(const char *path);
void asset_close(void *handle);
size_t asset_read(void *buf, size_t size, size_t nmemb, void *handle);
int asset_seek(void *handle, long offset, int whence);
long asset_tell(void *handle);
long asset_size(void *handle);
int asset_eof(void *handle);
int asset_getc(void *handle);
char *asset_gets(char *buf, int max, void *handle);

#endif
