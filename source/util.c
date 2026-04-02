/* util.c -- misc utility functions
 *
 * Copyright (C) 2026 givethesourceplox, fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "util.h"
#include "config.h"

static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_main_thread;
static int s_main_thread_set = 0;
#define DEBUG_EMULATED_FILE_IO_NS 2000000LL

void debugPrintf_setMainThread(void)
{
  s_main_thread = pthread_self();
  s_main_thread_set = 1;
}

#ifdef DEBUG_LOG

static int s_nxlinkSock = -1;

static void initNxLink(void)
{
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void)
{
  if (s_nxlinkSock >= 0)
  {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

void userAppInit(void)
{
  initNxLink();
}

void userAppExit(void)
{
  deinitNxLink();
}

#endif

int debugPrintf(const char *text, ...)
{
#ifdef DEBUG_LOG
  va_list list;
  const char *tag = "?";
  if (s_main_thread_set)
    tag = pthread_equal(pthread_self(), s_main_thread) ? "M" : "G";

  pthread_mutex_lock(&s_log_mutex);

#if DEBUG_FILE_LOG
  FILE *f = fopen(LOG_NAME, "a");
  if (f)
  {
    fprintf(f, "[%s] ", tag);
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);
  }
#endif

  printf("[%s] ", tag);
  va_start(list, text);
  vprintf(text, list);
  va_end(list);

#if !DEBUG_FILE_LOG
  // DEBUG_FILE_LOG=1 keeps this mutex held much longer because every log line
  // does synchronous file I/O. That extra serialization is currently masking a
  // startup/gameplay race in the port, so emulate a small portion of that
  // delay without actually writing debug.log.
  svcSleepThread(DEBUG_EMULATED_FILE_IO_NS);
#endif

  pthread_mutex_unlock(&s_log_mutex);
#endif
  return 0;
}

int ret0(void) { return 0; }

int ret1(void) { return 1; }

int retm1(void) { return -1; }
