/* config.h -- global configuration and config file handling
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen, givethesourceplox
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// The 64-bit Bully runtime streams much heavier mesh/audio/script data than the
// original Vita-era ports this loader started from. Give newlib a larger heap
// budget so medium-sized refcounted mesh buffers don't fail late during area
// transitions and cutscene setup.
#define MEMORY_MB 1024

#define SO_NAME "libBully.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "debug.log"

#define DEBUG_LOG 1
#define DEBUG_FILE_LOG 0

// actual screen size
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  int use_bloom;
  int trilinear_filter;
  int disable_mipmaps;
  int language;
  int crouch_toggle;
  int character_shadows;
  int drop_highest_lod;
  int show_weapon_menu;
  float decal_limit;
  float debris_limit;
  char mod_file[0x100];
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
