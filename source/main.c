/* main.c
 *
 * Copyright (C) 2026 givethesourceplox, fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "zip_fs.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

// provide replacement heap init function to separate newlib heap from the .so
void __libnx_initheap(void)
{
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride())
  {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  }
  else
  {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  // only allocate a fixed amount for the newlib heap
  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size = umin(size, MEMORY_MB * 1024 * 1024);
  fake_heap_start = (char *)addr;
  fake_heap_end = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000); // align to page size
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_data(void)
{
  const char *files[] = {
      // Basic structural check for loose files (assets folder)
      "assets/data_0.zip",
      // mod file goes here
      "",
  };
  struct stat st;
  unsigned int numfiles = (sizeof(files) / sizeof(*files)) - 1;
  // if mod is enabled, also check for mod file
  if (config.mod_file[0])
    files[numfiles++] = config.mod_file;
  // check if all the required files are present
  for (unsigned int i = 0; i < numfiles; ++i)
  {
    if (stat(files[i], &st) < 0)
    {
      fatal_error("Could not find\n%s.\nCheck your data files.", files[i]);
      break;
    }
  }
}

static void check_syscalls(void)
{
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.\nTitle Takeover required:\n1. Install a game in Ryujinx\n2. Hold R while launching it\n3. Select bully_nx.nro from Hbmenu");
}

static void set_screen_size(int w, int h)
{
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080)
  {
    // auto; prefer 720p on both handheld and docked.
    // Full 1080p docked works, but it drives GPU load unnecessarily high for
    // this Android wrapper and leaves little headroom for the heavier scenes.
    if (appletGetOperationMode() == AppletOperationMode_Console)
    {
      screen_width = 960;
      screen_height = 540;
    }
    else
    {
      screen_width = 960;
      screen_height = 540;
    }
  }
  else
  {
    screen_width = w;
    screen_height = h;
  }
  debugPrintf("screen mode: %dx%d\n", screen_width, screen_height);
}

int main(void)
{
  int compat_delay_ms = 0;

  // try to read the config file and create one with default values if it's missing
  if (read_config(CONFIG_NAME) < 0)
    write_config(CONFIG_NAME);

  compat_delay_ms = config.timing_workaround_ms;
  if (compat_delay_ms < 0)
    compat_delay_ms = 0;
  debugPrintf_setCompatDelayMs(compat_delay_ms);

  zip_fs_init();

  check_syscalls();
  check_data();

  // calculate actual screen size
  set_screen_size(config.screen_width, config.screen_height);

  debugPrintf("heap size = %u KB\n", MEMORY_MB * 1024);
  debugPrintf(" lib base = %p\n", heap_so_base);
  debugPrintf("  lib max = %u KB\n", heap_so_limit / 1024);

  // Load libc++_shared.so first — provides __ndk1 C++ stdlib symbols
  extern int cpplib_load(const char *filename);
  if (cpplib_load("libc++_shared.so") < 0)
    fatal_error("Could not load libc++_shared.so.\nCopy it from the APK\nlib/arm64-v8a/ folder\nto the game directory.");

  if (so_load(SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  // won't save without it
  mkdir("savegames", 0777);

  update_imports();

  so_relocate();
  so_resolve(dynlib_functions, dynlib_numfunctions, 1);

  patch_openal();
  // patch_opengl();
  patch_game();
  // patch_movie();

  // can't set it in the initializer because it's not constant
  stderr_fake = stderr;
  extern FILE *stdin_fake;
  stdin_fake = stdin;

  // Resolve all symbol addresses BEFORE so_finalize.
  // After svcMapProcessCodeMemory, the source heap pages become Perm_None
  // and so_find_addr (which reads syms/dynstrtab from text_base) will crash.
  // Use so_find_addr_rx to get the executable text_virtbase address.
  extern void jni_init(void);
  jni_init(); // sets up fake_vm and resolves JNI_OnLoad address

  // Set StorageRootPath directly — the game uses this for data file paths.
  // On Android, NvUtilInit sets this via JNI getAppLocalValue("STORAGE_ROOT").
  // We set it here to bypass the JNI roundtrip.
  strcpy((char *)so_find_addr("StorageRootPath"), ".");

  so_finalize();
  so_flush_caches();

  // GOT-based hooks: patch PLT/GOT entries now that data segment is RW.
  // Must be after so_finalize() (GOT accessible) and before jni_start() (game runs).
  patch_game_post_finalize();

  // Run C++ global constructors (.init_array).
  // This populates AllWarTypes, registers WarLangType objects, and performs
  // other critical static initialization that the game expects before main logic.
  so_execute_init_array();

  // Now call into the loaded .so - code is executable at text_virtbase
  extern void jni_start(void);
  jni_start();

  return 0;
}
