#ifndef __ZIP_FS_H__
#define __ZIP_FS_H__

#include <stdio.h>

int zip_fs_init(void);
FILE *zip_fs_fopen(const char *path);
int zip_fs_open(const char *path, int flags);

#endif
