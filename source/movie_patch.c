/* movie_patch.c -- Video playback for movies (STUBS for Switch)
 *
 * Copyright (C) 2026 givethesourceplox, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "config.h"
#include "so_util.h"

int OS_MoviePlay(const char *file, int a2, int a3, float a4) {
  debugPrintf("OS_MoviePlay stubbed! File: %s\n", file);
  return 0;
}

void OS_MovieStop(void) {
}

int OS_MovieIsPlaying(int *loops) {
  return 0; // Not playing
}

void OS_MovieSetSkippable(void) {
}

void movie_draw_frame(void) {
}

void patch_movie(void) {
  hook_arm64(so_find_addr("_Z12OS_MoviePlayPKcbbf"), (uintptr_t)OS_MoviePlay);
  hook_arm64(so_find_addr("_Z20OS_MovieSetSkippableb"), (uintptr_t)OS_MovieSetSkippable);
  hook_arm64(so_find_addr("_Z12OS_MovieStopv"), (uintptr_t)OS_MovieStop);
  hook_arm64(so_find_addr("_Z17OS_MovieIsPlayingPi"), (uintptr_t)OS_MovieIsPlaying);
}
