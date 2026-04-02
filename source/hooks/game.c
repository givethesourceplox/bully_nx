/* game.c -- hooks and patches for everything other than AL and GL
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <threads.h>

#include <EGL/egl.h>

#include "../asset_archive.h"
#include "../config.h"
#include "../hooks.h"
#include "../jni_patch.h"
#include "../so_util.h"
#include "../util.h"

extern EGLDisplay g_egl_display;
extern EGLSurface g_egl_surface;
extern EGLContext g_egl_context;

#define APK_PATH "main.obb"

extern uintptr_t __cxa_guard_acquire;
extern uintptr_t __cxa_guard_release;
extern void __cxa_throw_wrapper(void *, void *, void (*)(void *));
void *memalign_wrapper(size_t alignment, size_t size);

// Address of the isPhone flag in libGame.so's .bss
// AND_SystemInitialize writes: *(uint32_t*)(0x125da04) = (isPhone == 1) ? 1 : 0
// We set it to 1 (phone) to avoid enabling heavy desktop-class effects.
#define ISPHONE_FLAG_OFFSET 0x125da04

#define BULLY_SETTINGS_RESET_DISPLAY_IMPL_OFFSET 0x103384c
#define BULLY_SETTINGS_APPLY_DISPLAY_IMPL_OFFSET 0x10344dc
#define BULLY_SETTINGS_LOAD_IMPL_OFFSET 0x1034a14
#define BULLY_SETTINGS_GET_MAX_SHADOW_OPTION_IMPL_OFFSET 0x1033d24

#define BULLY_SETTINGS_RESET_DISPLAY_PLT_OFFSET 0x117dd60
#define BULLY_SETTINGS_LOAD_PLT_OFFSET 0x117de40

static uint8_t fake_tls[0x200];

typedef struct
{
  int game_button;
  u64 nx_mask;
} NxGamepadButtonMap;

typedef struct
{
  int valid;
  int connected;
  u64 held;
  u64 down;
  u64 up;
  float lx;
  float ly;
  float rx;
  float ry;
} NxInputSnapshot;

static NxInputSnapshot g_nx_input;
static int g_nx_input_last_connected = -1;
static int g_nx_input_log_count;

typedef struct
{
  char pad0[0x18];
  float brightness;
  int shadow_setting;
  int resolution_setting;
  int language_setting;
  char pad1[0x4];
  int resolution_profile;
  char pad2[0x8];
  int shadow_profile;
  char pad3[0x70];
  int effects_level;
  uint8_t dirty;
} BullySettingsConfigLayout;

static const NxGamepadButtonMap g_nx_gamepad_button_map[] = {
    {0, HidNpadButton_B},
    {1, HidNpadButton_A},
    {2, HidNpadButton_Y},
    {3, HidNpadButton_X},
    {4, HidNpadButton_Plus},
    {5, HidNpadButton_Minus},
    {8, HidNpadButton_Up},
    {9, HidNpadButton_Down},
    {10, HidNpadButton_Left},
    {11, HidNpadButton_Right},
    {12, HidNpadButton_StickL},
    {13, HidNpadButton_StickR},
    {16, HidNpadButton_L},
    {17, HidNpadButton_ZL},
    {18, HidNpadButton_R},
    {19, HidNpadButton_ZR},
};

static float nx_input_deadzone(float val)
{
  if (fabsf(val) < 0.2f)
    return 0.0f;
  return val;
}

static void BullySettings_ResetDisplay_impl(void *this_ptr)
{
  void (*fn)(void *) =
      (void (*)(void *))((uintptr_t)text_virtbase + BULLY_SETTINGS_RESET_DISPLAY_IMPL_OFFSET);
  fn(this_ptr);
}

static void BullySettings_ApplyDisplay_impl(void *this_ptr)
{
  void (*fn)(void *) =
      (void (*)(void *))((uintptr_t)text_virtbase + BULLY_SETTINGS_APPLY_DISPLAY_IMPL_OFFSET);
  fn(this_ptr);
}

static void *BullySettings_Load_impl(void *this_ptr)
{
  void *(*fn)(void *) =
      (void *(*)(void *))((uintptr_t)text_virtbase + BULLY_SETTINGS_LOAD_IMPL_OFFSET);
  return fn(this_ptr);
}

static int BullySettings_GetMaxShadowOption_impl(void *this_ptr)
{
  int (*fn)(void *) =
      (int (*)(void *))((uintptr_t)text_virtbase + BULLY_SETTINGS_GET_MAX_SHADOW_OPTION_IMPL_OFFSET);
  return fn(this_ptr);
}

static void BullySettings_ApplyConfig(void *this_ptr)
{
  BullySettingsConfigLayout *settings = (BullySettingsConfigLayout *)this_ptr;
  int clarity;
  int native_resolution;
  int shadows;
  int shadow_profile;

  if (!settings)
    return;

  clarity = config.clarity;
  if (clarity < 0)
    clarity = 0;
  if (clarity > 2)
    clarity = 2;
  native_resolution = 2 - clarity;

  shadows = config.shadows;
  if (shadows < 0)
    shadows = 0;
  {
    int max_shadow = BullySettings_GetMaxShadowOption_impl(this_ptr);
    if (max_shadow >= 0 && shadows > max_shadow)
      shadows = max_shadow;
  }
  shadow_profile = shadows > 0 ? shadows + 1 : 0;

  // Native ResolutionSetting is ordered High/Med/Low in this build, while the
  // port config is exposed as Low/Medium/High.
  settings->resolution_profile = native_resolution;
  settings->resolution_setting = native_resolution;
  // Native profile shadows use Off=0, Low=2, Medium=3, High=4.
  settings->shadow_profile = shadow_profile;
  settings->effects_level = clarity + 1;
  settings->shadow_setting = shadows;
}

static void BullySettings_ResetDisplay_hook(void *this_ptr)
{
  BullySettings_ResetDisplay_impl(this_ptr);
  BullySettings_ApplyConfig(this_ptr);
  BullySettings_ApplyDisplay_impl(this_ptr);
}

static void *BullySettings_Load_hook(void *this_ptr)
{
  void *ret = BullySettings_Load_impl(this_ptr);
  if (ret)
  {
    BullySettings_ApplyConfig(ret);
    BullySettings_ApplyDisplay_impl(ret);
  }
  return ret;
}

static u64 nx_input_mask_for_button(int game_button)
{
  unsigned i;

  for (i = 0; i < sizeof(g_nx_gamepad_button_map) / sizeof(g_nx_gamepad_button_map[0]); i++)
  {
    if (g_nx_gamepad_button_map[i].game_button == game_button)
      return g_nx_gamepad_button_map[i].nx_mask;
  }

  return 0;
}

static int nx_input_button_from_mask(u64 buttons, int game_button)
{
  u64 mask = nx_input_mask_for_button(game_button);
  return mask != 0 && (buttons & mask) != 0;
}

static int nx_input_lib_state_for_button(int game_button)
{
  u64 mask = nx_input_mask_for_button(game_button);

  if (!g_nx_input.connected || mask == 0)
    return 0;
  if (g_nx_input.down & mask)
    return 2;
  if (g_nx_input.held & mask)
    return 3;
  return 0;
}

static void nx_input_refresh_snapshot(void)
{
  int connected = 0;
  u64 buttons = 0;
  float axes[6];
  u64 prev_held = g_nx_input.connected ? g_nx_input.held : 0;

  memset(axes, 0, sizeof(axes));
  jni_gamepad_get_state(&connected, &buttons, axes);

  g_nx_input.connected = connected;
  if (!connected)
  {
    g_nx_input.held = 0;
    g_nx_input.down = 0;
    g_nx_input.up = prev_held;
    g_nx_input.lx = 0.0f;
    g_nx_input.ly = 0.0f;
    g_nx_input.rx = 0.0f;
    g_nx_input.ry = 0.0f;
  }
  else
  {
    g_nx_input.held = buttons;
    g_nx_input.down = buttons & ~prev_held;
    g_nx_input.up = prev_held & ~buttons;
    g_nx_input.lx = axes[0];
    g_nx_input.ly = axes[1];
    g_nx_input.rx = axes[2];
    g_nx_input.ry = axes[3];
  }

  g_nx_input.valid = 1;

  if (g_nx_input.connected != g_nx_input_last_connected)
  {
    debugPrintf("nx_input: connected=%d\n", g_nx_input.connected);
    g_nx_input_last_connected = g_nx_input.connected;
  }

  if (g_nx_input_log_count < 16 &&
      (g_nx_input.down != 0 ||
       fabsf(g_nx_input.lx) > 0.0f || fabsf(g_nx_input.ly) > 0.0f ||
       fabsf(g_nx_input.rx) > 0.0f || fabsf(g_nx_input.ry) > 0.0f))
  {
    debugPrintf("nx_input: held=%#llx down=%#llx up=%#llx lx=%.2f ly=%.2f rx=%.2f ry=%.2f\n",
                (unsigned long long)g_nx_input.held,
                (unsigned long long)g_nx_input.down,
                (unsigned long long)g_nx_input.up,
                g_nx_input.lx, g_nx_input.ly, g_nx_input.rx, g_nx_input.ry);
    g_nx_input_log_count++;
  }
}

static void nx_input_ensure_snapshot(void)
{
  if (!g_nx_input.valid)
    nx_input_refresh_snapshot();
}

static float nx_input_digital_state(int game_button)
{
  return nx_input_button_from_mask(g_nx_input.held, game_button) ? 1.0f : 0.0f;
}

static int InputController_GetGBPressed_hook(void *this_ptr, unsigned player, int game_button)
{
  (void)this_ptr;
  (void)player;
  nx_input_ensure_snapshot();
  return g_nx_input.connected && nx_input_button_from_mask(g_nx_input.down, game_button);
}

static int InputController_GetGBDown_hook(void *this_ptr, unsigned player, int game_button)
{
  (void)this_ptr;
  (void)player;
  nx_input_ensure_snapshot();
  return g_nx_input.connected && nx_input_button_from_mask(g_nx_input.held, game_button);
}

static int InputController_GetGBReleased_hook(void *this_ptr, unsigned player, int game_button)
{
  (void)this_ptr;
  (void)player;
  nx_input_ensure_snapshot();
  return g_nx_input.connected && nx_input_button_from_mask(g_nx_input.up, game_button);
}

static int LIB_GamepadState_hook(int padnum, int button)
{
  (void)padnum;
  nx_input_ensure_snapshot();

  switch (button)
  {
  case 0:
    return nx_input_lib_state_for_button(0);
  case 1:
    return nx_input_lib_state_for_button(1);
  case 2:
    return nx_input_lib_state_for_button(2);
  case 3:
    return nx_input_lib_state_for_button(3);
  case 4:
    return nx_input_lib_state_for_button(4);
  case 5:
    return nx_input_lib_state_for_button(5);
  case 6:
    return nx_input_lib_state_for_button(17);
  case 7:
    return nx_input_lib_state_for_button(19);
  case 8:
    return nx_input_lib_state_for_button(8);
  case 9:
    return nx_input_lib_state_for_button(9);
  case 10:
    return nx_input_lib_state_for_button(10);
  case 11:
    return nx_input_lib_state_for_button(11);
  case 12:
    return nx_input_lib_state_for_button(16);
  case 13:
    return nx_input_lib_state_for_button(18);
  default:
    return 0;
  }
}

static float Pad_GetState_hook(const void *this_ptr, int type)
{
  (void)this_ptr;
  nx_input_ensure_snapshot();

  if (!g_nx_input.connected)
    return 0.0f;

  switch (type)
  {
  case 0:
    return nx_input_digital_state(14);
  case 1:
    return nx_input_digital_state(15);
  case 2:
    return nx_input_digital_state(12);
  case 3:
    return nx_input_digital_state(13);
  case 4:
    return nx_input_digital_state(5);
  case 5:
    return nx_input_digital_state(4);
  case 6:
    return nx_input_digital_state(2);
  case 7:
    return nx_input_digital_state(0);
  case 8:
    return nx_input_digital_state(1);
  case 9:
    return nx_input_digital_state(3);
  case 10:
    return nx_input_digital_state(16);
  case 11:
    return nx_input_digital_state(17);
  case 12:
    return nx_input_digital_state(18);
  case 13:
    return nx_input_digital_state(19);
  case 14:
    return nx_input_digital_state(6);
  case 15:
    return nx_input_digital_state(7);
  case 16:
    return g_nx_input.lx;
  case 17:
    return g_nx_input.ly;
  case 18:
    return g_nx_input.rx;
  case 19:
    return g_nx_input.ry;
  case 20:
    return -g_nx_input.lx;
  case 21:
    return -g_nx_input.ly;
  case 22:
    return -g_nx_input.rx;
  case 23:
    return -g_nx_input.ry;
  default:
    return 0.0f;
  }
}

#define NDK_ISTREAM_READ_PLT_OFFSET 0x11869a0

typedef struct ActionTreeStringNode
{
  struct ActionTreeStringNode *next;
  uint64_t hash;
  uint32_t key;
  uint32_t pad;
  unsigned char string_obj[24];
} ActionTreeStringNode;

typedef struct
{
  char pad0[0x30];
  ActionTreeStringNode ***buckets;
  uint64_t bucket_count;
} ActionTreeStringMap;

typedef struct
{
  void *stream_owner;
  ActionTreeStringMap *string_map;
} ActionTreeDecoderLayout;

typedef struct MissingActionTreeString
{
  uint32_t key;
  char *text;
  struct MissingActionTreeString *next;
} MissingActionTreeString;

static MissingActionTreeString *g_missing_action_tree_strings;
static int g_missing_action_tree_string_logs;

static void *action_tree_istream_read(void *stream, char *dst, long count)
{
  typedef void *(*istream_read_t)(void *, char *, long);
  istream_read_t fn = (istream_read_t)((uintptr_t)text_virtbase + NDK_ISTREAM_READ_PLT_OFFSET);
  return fn(stream, dst, count);
}

static uint64_t action_tree_bucket_index(uint64_t bucket_count, uint64_t hash)
{
  if (bucket_count == 0)
    return 0;
  if ((bucket_count & (bucket_count - 1)) == 0)
    return hash & (bucket_count - 1);
  return hash % bucket_count;
}

static const ActionTreeStringNode *action_tree_find_string_node(const ActionTreeStringMap *map, uint32_t key)
{
  uint64_t bucket_count;
  uint64_t index;
  ActionTreeStringNode **slot;
  ActionTreeStringNode *node;

  if (!map)
    return NULL;

  bucket_count = map->bucket_count;
  if (bucket_count == 0 || !map->buckets)
    return NULL;

  index = action_tree_bucket_index(bucket_count, key);
  slot = map->buckets[index];
  if (!slot)
    return NULL;

  node = slot[0];
  while (node)
  {
    if (node->hash == key && node->key == key)
      return node;
    if (action_tree_bucket_index(bucket_count, node->hash) != index)
      return NULL;
    node = node->next;
  }

  return NULL;
}

static const char *action_tree_node_string(const ActionTreeStringNode *node)
{
  if (!node)
    return "";
  if ((node->string_obj[0] & 1) == 0)
    return (const char *)&node->string_obj[1];
  return *(const char *const *)&node->string_obj[16];
}

static const char *action_tree_missing_string(uint32_t key)
{
  MissingActionTreeString *entry = g_missing_action_tree_strings;

  while (entry)
  {
    if (entry->key == key)
      return entry->text;
    entry = entry->next;
  }

  entry = calloc(1, sizeof(*entry));
  if (!entry)
    return "";

  entry->text = malloc(32);
  if (!entry->text)
  {
    free(entry);
    return "";
  }

  snprintf(entry->text, 32, "missing_%08x", key);
  entry->key = key;
  entry->next = g_missing_action_tree_strings;
  g_missing_action_tree_strings = entry;
  return entry->text;
}

static const char *ActionTreeDecoder_decodeStringRef_hook(void *this_ptr)
{
  ActionTreeDecoderLayout *decoder = (ActionTreeDecoderLayout *)this_ptr;
  uint32_t key = 0;
  const ActionTreeStringNode *node;

  if (!decoder || !decoder->stream_owner)
    return "";

  action_tree_istream_read((char *)decoder->stream_owner + 0x28, (char *)&key, 4);
  node = action_tree_find_string_node(decoder->string_map, key);
  if (node)
    return action_tree_node_string(node);

  if (g_missing_action_tree_string_logs < 32)
  {
    uint64_t bucket_count = decoder->string_map ? decoder->string_map->bucket_count : 0;
    debugPrintf("ActionTreeDecoder::decodeStringRef MISS key=%u (0x%08x) buckets=%llu -> %s\n",
                key, key, (unsigned long long)bucket_count, action_tree_missing_string(key));
    g_missing_action_tree_string_logs++;
  }

  return action_tree_missing_string(key);
}

// ============================================================================
// Stack canary binary patch — Android ARM64 uses TLS-based stack canaries:
// mrs Xn, tpidr_el0; ldr Xt, [Xn, #0x28] to load __stack_chk_guard.
// On Switch, tpidr_el0+0x28 is inside the IPC buffer that gets clobbered
// by every service call, causing canary mismatches → __stack_chk_fail → crash.
// Fix: scan the binary for all 'bl __stack_chk_fail' call sites and NOP
// the guarding b.ne branches so the canary check is effectively disabled.
// ============================================================================
#define STACK_CHK_FAIL_PLT 0x11787e0
#define NOP_INSN 0xD503201F

static void patch_stack_canary(void) {
  uint32_t *code = (uint32_t *)text_base;
  int text_insns = (int)(text_size / 4);
  int patched_bne = 0;
  int patched_bl = 0;

  for (int i = 0; i < text_insns; i++) {
    uint32_t insn = code[i];

    // Check for BL instruction: 1001 01xx xxxx xxxx xxxx xxxx xxxx xxxx
    if ((insn & 0xFC000000) != 0x94000000)
      continue;

    // Decode signed imm26 offset
    int32_t imm26 = (int32_t)(insn & 0x03FFFFFF);
    if (imm26 & 0x02000000)
      imm26 |= (int32_t)0xFC000000; // sign-extend
    uint64_t target = (uint64_t)((int64_t)(i * 4) + ((int64_t)imm26 << 2));

    if (target != STACK_CHK_FAIL_PLT)
      continue;

    // Found bl __stack_chk_fail at instruction index i (vaddr = i*4)
    uint32_t bl_vaddr = (uint32_t)(i * 4);

    // NOP the bl itself
    code[i] = NOP_INSN;
    patched_bl++;

    // Search backward for b.ne targeting this bl
    // B.cond encoding: 0101 0100 imm19:0 cond
    // B.NE has cond=0001
    for (int j = i - 1; j >= 0 && j >= i - 512; j--) {
      uint32_t prev = code[j];
      // Check B.NE: (prev & 0xFF00001F) == 0x54000001
      if ((prev & 0xFF00001F) != 0x54000001)
        continue;
      int32_t imm19 = (int32_t)((prev >> 5) & 0x7FFFF);
      if (imm19 & 0x40000)
        imm19 |= (int32_t)0xFFF80000; // sign-extend
      uint32_t branch_target =
          (uint32_t)((int64_t)(j * 4) + ((int64_t)imm19 << 2));
      if (branch_target == bl_vaddr) {
        code[j] = NOP_INSN;
        patched_bne++;
        break;
      }
    }
  }

  debugPrintf(
      "patch_stack_canary: disabled %d canary checks (%d b.ne + %d bl)\n",
      patched_bne + patched_bl, patched_bne, patched_bl);
}

// ============================================================================
// NvAPK replacement — filesystem-backed implementations
// The game's built-in NvAPK code calls AAssetManager via JNI, which we can't
// fully emulate. Instead we hook ALL NvAPK functions to read directly from
// the extracted assets/ directory on the filesystem.
// ============================================================================

int NvAPKInit_hook(void *assetManager, void *mainObbPaths,
                   void *patchObbPaths) {
  asset_archive_init();
  debugPrintf("NvAPKInit_hook: initialized (filesystem mode, ignoring "
              "AAssetManager)\n");
  return 0;
}

void *NvAPKOpen_hook(const char *path) { return asset_open(path); }

void *NvAPKOpenFromPack_hook(const char *path) { return asset_open(path); }

void NvAPKClose_hook(void *handle) { asset_close(handle); }

static int nvapk_read_count = 0;
int NvAPKRead_hook(void *buf, size_t size, size_t nmemb, void *handle) {
  if (!handle)
    return 0;
  int ret = (int)asset_read(buf, size, nmemb, handle);
  nvapk_read_count++;
  return ret;
}

static int nvapk_seek_count = 0;
int NvAPKSeek_hook(void *handle, long offset, int whence) {
  if (!handle)
    return -1;
  int ret = asset_seek(handle, offset, whence);
  nvapk_seek_count++;
  return ret;
}

long NvAPKTell_hook(void *handle) {
  if (!handle)
    return -1;
  return asset_tell(handle);
}

long NvAPKSize_hook(void *handle) {
  if (!handle)
    return 0;
  return asset_size(handle);
}

int NvAPKEOF_hook(void *handle) {
  if (!handle)
    return 1;
  return asset_eof(handle);
}

int NvAPKGetc_hook(void *handle) {
  if (!handle)
    return -1;
  return asset_getc(handle);
}

char *NvAPKGets_hook(char *buf, int max, void *handle) {
  if (!handle)
    return NULL;
  return asset_gets(buf, max, handle);
}

// ============================================================================
// NvF* wrapper layer hooks — the game's intermediate file API that wraps
// NvAPK* (for APK/asset files) and fread/fseek (for regular files).
// We own the handle layout here, so keep extra path/offset state for targeted
// diagnostics around the action-tree container reads.
// ============================================================================

typedef struct {
  int type;
  int trace_reads_left;
  void *fp;
  long last_pos;
  char path[192];
} NvFHandle;

static int nvf_open_count = 0;

static void nvf_normalize_path(char *dst, size_t dst_size, const char *src) {
  size_t j = 0;

  if (!dst || dst_size == 0)
    return;

  if (!src) {
    dst[0] = '\0';
    return;
  }

  while ((*src == '.' && src[1] == '/') || *src == '/')
    src += (*src == '/') ? 1 : 2;

  while (*src && j + 1 < dst_size) {
    char c = *src++;
    if (c == '\\')
      c = '/';
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    dst[j++] = c;
  }

  dst[j] = '\0';
}

static const char *nvf_trim_path(const char *path) {
  if (!path)
    return "";

  while ((*path == '.' && path[1] == '/') || *path == '/')
    path += (*path == '/') ? 1 : 2;

  return path;
}

static FILE *nvf_open_loose_file(const char *path, const char *normalized_path,
                                 char *opened_path, size_t opened_path_size) {
  const char *trimmed_path = nvf_trim_path(path);
  const char *trimmed_normalized = nvf_trim_path(normalized_path);
  char savegames_path[256];
  char savegames_normalized[256];
  const char *candidates[6];
  size_t i;

  if (opened_path && opened_path_size > 0)
    opened_path[0] = '\0';

  snprintf(savegames_path, sizeof(savegames_path), "savegames/%s",
           trimmed_path);
  snprintf(savegames_normalized, sizeof(savegames_normalized), "savegames/%s",
           trimmed_normalized);

  candidates[0] = path;
  candidates[1] = trimmed_path;
  candidates[2] = normalized_path;
  candidates[3] = trimmed_normalized;
  candidates[4] = savegames_path;
  candidates[5] = savegames_normalized;

  for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    FILE *fp;
    size_t j;
    const char *candidate = candidates[i];
    int duplicate = 0;

    if (!candidate || !candidate[0])
      continue;

    for (j = 0; j < i; j++) {
      if (candidates[j] && strcmp(candidates[j], candidate) == 0) {
        duplicate = 1;
        break;
      }
    }
    if (duplicate)
      continue;

    fp = fopen(candidate, "rb");
    if (!fp)
      continue;

    if (opened_path && opened_path_size > 0) {
      snprintf(opened_path, opened_path_size, "%s", candidate);
    }
    return fp;
  }

  return NULL;
}

static int nvf_should_trace_path(const char *path) {
  if (!path || !path[0])
    return 0;

  return (strstr(path, "act/act.img") != NULL) ||
         (strstr(path, "act/act.dir") != NULL) ||
         (strstr(path, "globals.act") != NULL);
}

static void nvf_dump_read_prefix(const char *path, long pos, const void *buf,
                                 size_t bytes) {
  char hex[3 * 16 + 1];
  char ascii[16 + 1];
  const unsigned char *src = (const unsigned char *)buf;
  size_t i;
  size_t dump_len = (bytes < 16) ? bytes : 16;

  for (i = 0; i < dump_len; i++) {
    snprintf(&hex[i * 3], sizeof(hex) - i * 3, "%02X ", src[i]);
    ascii[i] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : '.';
  }

  hex[dump_len * 3] = '\0';
  ascii[dump_len] = '\0';

  debugPrintf("NvFRead trace: \"%s\" pos=%ld bytes=%zu hdr=[%s] ascii=\"%s\"\n",
              path ? path : "(null)", pos, bytes, hex, ascii);
}

void NvFInit_hook(void) { debugPrintf("NvFInit: initialized\n"); }

void *NvFOpen_hook(const char *path) {
  void *asset = NvAPKOpen_hook(path);
  if (asset) {
    NvFHandle *h = calloc(1, sizeof(*h));
    if (!h)
      return NULL;
    h->type = 0;
    h->fp = asset;
    h->last_pos = 0;
    nvf_normalize_path(h->path, sizeof(h->path), path);
    if (nvf_should_trace_path(h->path)) {
      h->trace_reads_left = 8;
      debugPrintf("NvFOpen trace: \"%s\" -> asset=%p size=%ld\n", h->path,
                  asset, NvAPKSize_hook(asset));
    }
    nvf_open_count++;
    return h;
  }
  {
    char normalized_path[192];
    char opened_path[256];
    FILE *fp;
    nvf_normalize_path(normalized_path, sizeof(normalized_path), path);
    fp = nvf_open_loose_file(path, normalized_path, opened_path,
                             sizeof(opened_path));
    if (fp) {
      NvFHandle *h = calloc(1, sizeof(*h));
      if (!h) {
        fclose(fp);
        return NULL;
      }
      h->type = 1;
      h->fp = fp;
      h->last_pos = 0;
      nvf_normalize_path(h->path, sizeof(h->path), opened_path);
      if (nvf_should_trace_path(h->path) || nvf_should_trace_path(normalized_path)) {
        h->trace_reads_left = 8;
        debugPrintf("NvFOpen trace: \"%s\" -> file=\"%s\" fp=%p\n",
                    normalized_path, opened_path, fp);
      }
      nvf_open_count++;
      return h;
    }
    if (nvf_should_trace_path(normalized_path))
      debugPrintf("NvFOpen trace: FAILED \"%s\"\n", normalized_path);
  }
  return NULL;
}

int NvFIsApkFile_hook(void *handle) {
  if (!handle)
    return 0;
  // These are no longer true Android APK/AAsset handles. They are our own
  // archive/filesystem wrappers, and the game's async streamer only queues
  // handles when this returns 0.
  return 0;
}

void NvFClose_hook(void *handle) {
  NvFHandle *h = (NvFHandle *)handle;
  if (!handle)
    return;
  if (nvf_should_trace_path(h->path))
    debugPrintf("NvFClose trace: \"%s\" final_pos=%ld\n", h->path, h->last_pos);
  if (h->type == 1) {
    if (h->fp)
      fclose((FILE *)h->fp);
  } else {
    NvAPKClose_hook(h->fp);
  }
  free(h);
}

static int nvf_read_count = 0;
int NvFRead_hook(void *buf, size_t size, size_t nmemb, void *handle) {
  NvFHandle *h = (NvFHandle *)handle;
  size_t bytes_requested;
  size_t bytes_read;
  long before_pos;
  if (!handle)
    return 0;
  before_pos = h->last_pos;
  bytes_requested = size * nmemb;
  int ret;
  if (h->type == 1) {
    ret = (int)fread(buf, size, nmemb, (FILE *)h->fp);
    h->last_pos = ftell((FILE *)h->fp);
  } else {
    ret = NvAPKRead_hook(buf, size, nmemb, h->fp);
    h->last_pos = NvAPKTell_hook(h->fp);
  }
  bytes_read = (size_t)ret * size;
  if (h->trace_reads_left > 0) {
    debugPrintf(
        "NvFRead trace: \"%s\" pos=%ld req=%zu got=%zu nmemb=%zu size=%zu\n",
        h->path, before_pos, bytes_requested, bytes_read, nmemb, size);
    if (buf && bytes_read > 0)
      nvf_dump_read_prefix(h->path, before_pos, buf, bytes_read);
    h->trace_reads_left--;
  }
  nvf_read_count++;
  return ret;
}

long NvFSize_hook(void *handle) {
  NvFHandle *h = (NvFHandle *)handle;
  if (!handle)
    return 0;
  long sz;
  if (h->type == 1) {
    long cur = ftell((FILE *)h->fp);
    fseek((FILE *)h->fp, 0, SEEK_END);
    sz = ftell((FILE *)h->fp);
    fseek((FILE *)h->fp, cur, SEEK_SET);
  } else {
    sz = NvAPKSize_hook(h->fp);
  }
  return sz;
}

static int nvf_seek_count = 0;
int NvFSeek_hook(void *handle, long offset, int whence) {
  NvFHandle *h = (NvFHandle *)handle;
  if (!handle)
    return -1;
  int ret;
  if (h->type == 1) {
    ret = fseek((FILE *)h->fp, offset, whence);
    if (ret == 0)
      h->last_pos = ftell((FILE *)h->fp);
  } else {
    ret = NvAPKSeek_hook(h->fp, offset, whence);
    if (ret == 0)
      h->last_pos = NvAPKTell_hook(h->fp);
  }
  if (nvf_should_trace_path(h->path)) {
    debugPrintf("NvFSeek trace: \"%s\" off=%ld whence=%d -> ret=%d pos=%ld\n",
                h->path, offset, whence, ret, h->last_pos);
  }
  nvf_seek_count++;
  return ret;
}

long NvFTell_hook(void *handle) {
  NvFHandle *h = (NvFHandle *)handle;
  if (!handle)
    return -1;
  if (h->type == 1) {
    h->last_pos = ftell((FILE *)h->fp);
  } else {
    h->last_pos = NvAPKTell_hook(h->fp);
  }
  return h->last_pos;
}

int NvFEOF_hook(void *handle) {
  NvFHandle *h = (NvFHandle *)handle;
  if (!handle)
    return 1;
  if (h->type == 1) {
    return feof((FILE *)h->fp);
  } else {
    return NvAPKEOF_hook(h->fp);
  }
}

int NvFGetc_hook(void *handle) {
  NvFHandle *h = (NvFHandle *)handle;
  if (!handle)
    return -1;
  if (h->type == 1) {
    return fgetc((FILE *)h->fp);
  } else {
    return NvAPKGetc_hook(h->fp);
  }
}

char *NvFGets_hook(char *buf, int max, void *handle) {
  NvFHandle *h = (NvFHandle *)handle;
  if (!handle)
    return NULL;
  if (h->type == 1) {
    return fgets(buf, max, (FILE *)h->fp);
  } else {
    return NvAPKGets_hook(buf, max, h->fp);
  }
}

int ProcessEvents(void) {
  return 0; // 1 is exit!
}

int AND_DeviceType(void) {
  // 0x1: phone
  // 0x2: tegra
  // low memory is < 256
  return (MEMORY_MB << 6) | (3 << 2) | 0x2;
}

int AND_DeviceLocale(void) {
  return 0; // english
}

int OS_ScreenGetHeight(void) { return screen_height; }

int OS_ScreenGetWidth(void) { return screen_width; }

int OS_CanGameRender(void) { return 1; }

int OS_IsGameSuspended(void) { return 0; }

static void iobuffer_call_vfunc(void *obj, size_t slot_offset)
{
  uintptr_t *vtable;
  uintptr_t fn;

  if (!obj)
    return;

  vtable = (uintptr_t *)*(uintptr_t *)obj;
  if (!vtable)
    return;

  fn = *(uintptr_t *)((uintptr_t)vtable + slot_offset);
  if (!fn)
    return;

  ((void (*)(void *))fn)(obj);
}

static void IOBuffer_BlockUntilComplete_hook(void *this_ptr)
{
  if (!this_ptr)
    return;

  while (1)
  {
    iobuffer_call_vfunc(this_ptr, 96);
    if (*(volatile int *)((uintptr_t)this_ptr + 40) == 2)
      return;

    if (jni_async_file_pending())
    {
      jni_async_file_kick();
      svcSleepThread(100000LL);
      continue;
    }

    svcSleepThread(1000000LL);
  }
}

static void FileReadBuffer_BlockUntilComplete_hook(void *this_ptr)
{
  void *platform_reader;

  if (!this_ptr)
    return;

  platform_reader = *(void **)((uintptr_t)this_ptr + 48);
  iobuffer_call_vfunc(platform_reader, 24);
  IOBuffer_BlockUntilComplete_hook(this_ptr);
}

// ============================================================================
// AND_*Initialize debug wrappers / stubs
// implOnInitialSetup calls these in order. We hook each one to trace progress.
// Functions that only register JNI method IDs for features not needed on Switch
// are stubbed out to avoid potential crashes from JNI dispatch.
// ============================================================================

// Stubs for subsystems that just register method IDs — not needed on Switch
void AND_PlaylistInitialize_hook(void *env) {
  debugPrintf("AND_PlaylistInitialize_hook: stubbed (not needed on Switch)\n");
}

void AND_MiscInitialize_hook(void *env) {
  debugPrintf("AND_MiscInitialize_hook: stubbed (not needed on Switch)\n");
}

void AND_KeyboardInitialize_hook(void *env) {
  debugPrintf("AND_KeyboardInitialize_hook: stubbed (not needed on Switch)\n");
}

void AND_MovieInitialize_hook(void *env) {
  debugPrintf("AND_MovieInitialize_hook: stubbed (no movie playback)\n");
}

void AND_HapticInitialize_hook(void *env) {
  debugPrintf("AND_HapticInitialize_hook: stubbed\n");
}

void AND_TouchInitialize_hook(void *env, void *activity) {
  debugPrintf("AND_TouchInitialize_hook: stubbed (using gamepad)\n");
}

void AND_DisplayInitialize_hook(void *env) {
  debugPrintf("AND_DisplayInitialize_hook: stubbed (no splash screen)\n");
}

static void ControllerScene_ShowGamepadInstructions_hook(void *this_ptr) {
  (void)this_ptr;
}

static void HUDScene_OnSwitchToGamepad_hook(void *this_ptr) {
  (void)this_ptr;
}

static void MenuWrapper_CheckGamepad_hook(void *this_ptr) {
  (void)this_ptr;
}

static void cSCREAMAudioManager_PrepareForAreaTransition_hook(void *this_ptr) {
  (void)this_ptr;
}

static void cSCREAMBankManager_AreaTransition_hook(void *this_ptr) {
  (void)this_ptr;
}

static void cSCREAMBankManager_StartDVDLoadingMusic_hook(void *this_ptr) {
  (void)this_ptr;
}

static void cSCREAMBankManager_StopDVDLoadingMusic_hook(void *this_ptr) {
  (void)this_ptr;
}

void AND_RockstarInitialize_hook(void *env) {
  debugPrintf("AND_RockstarInitialize_hook: stubbed (no social club)\n");
}

// ============================================================================
// OS_Rockstar* stubs — the game calls these at runtime even though we stubbed
// AND_RockstarInitialize. They do JNI dispatch with method IDs that were never
// registered, causing NULL dereferences. Stub them all.
// ============================================================================

int OS_RockstarShowInitial_hook(void) {
  debugPrintf("OS_RockstarShowInitial: stubbed, triggering gate completion\n");
  // Signal both "initial complete" and "gate complete" flags directly.
  // These are the native completion callbacks that the Android Java side
  // would call after showing the SC splash.
  extern void *text_virtbase;
  // OS_OnRockstarInitialComplete sets byte at 0x126bb80 = 1
  volatile uint8_t *initialComplete =
      (volatile uint8_t *)((uintptr_t)text_virtbase + 0x126bb80);
  *initialComplete = 1;
  debugPrintf("OS_RockstarShowInitial: set initial_complete flag at %p\n",
              (void *)initialComplete);
  // OS_OnRockstarGateComplete(true) sets byte at 0x126bb88 = 1
  volatile uint8_t *gateComplete =
      (volatile uint8_t *)((uintptr_t)text_virtbase + 0x126bb88);
  *gateComplete = 1;
  debugPrintf("OS_RockstarShowInitial: set gate_complete flag at %p\n",
              (void *)gateComplete);
  return 1; // report success so startup flow doesn't wait for Java UI
}

int OS_RockstarCloudInit_hook(void) {
  debugPrintf("OS_RockstarCloudInit: stubbed\n");
  return 0; // disable SC cloud pipeline (prevents cloud phase timeouts/crashes)
}

int OS_RockstarCloudUpdate_hook(void) {
  // Silence per-frame spam. The game polls this every frame.
  return 0;
}

int OS_RockstarSignedIn_hook(void) {
  return 1; // no Java login UI on Switch; force signed-in state to continue
            // boot
}

int OS_RockstarCloudAvailable_hook(void) {
  return 0; // no cloud
}

int OS_IsRockstarCloudEnabled_hook(void) { return 0; }

// Startup can get stuck forever in WaitForNextPhase(phase=3) when Rockstar/
// cloud Java-side callbacks are absent. Force the phase gate open.
int WaitForNextPhase_hook(int phase) {
  debugPrintf("WaitForNextPhase_hook: force pass phase=%d\n", phase);
  return 1;
}

typedef struct {
  uint8_t raw[24];
} string8_ret;

typedef struct {
  void *ref;
  uint32_t len;
  int32_t offset;
} string8_obj;

typedef struct Name8CacheEntry {
  uint32_t hash;
  uint32_t len;
  void *refbuf;
  struct Name8CacheEntry *next;
} Name8CacheEntry;

#define NAME8_TOSTRING_IMPL_OFFSET 0x925750
#define NAME8_CACHE_BUCKET_COUNT 1024

static pthread_mutex_t g_name8_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static Name8CacheEntry *g_name8_cache_buckets[NAME8_CACHE_BUCKET_COUNT];
static int g_name8_cache_miss_logs;
static int g_name8_cache_collision_logs;

static unsigned char name8_ascii_lower(unsigned char c) {
  if (c >= 'A' && c <= 'Z')
    return (unsigned char)(c + 0x20);
  return c;
}

static uint32_t name8_hash_text(const char *text, uint32_t len) {
  uint32_t h;
  uint32_t i;

  if (!text || len == 0)
    return 0;

  if (len == 1)
    return ((uint32_t)(uint8_t)text[0]) << 16;

  if (len == 2)
    return (((uint32_t)(uint8_t)text[0]) << 16) |
           (((uint32_t)(uint8_t)text[1]) << 8);

  if (len == 3)
    return (((uint32_t)(uint8_t)text[0]) << 16) |
           (((uint32_t)(uint8_t)text[1]) << 8) |
           ((uint32_t)(uint8_t)text[2]);

  h = (((uint32_t)(uint8_t)text[0]) << 24) |
      (((uint32_t)(uint8_t)text[1]) << 16) |
      (((uint32_t)(uint8_t)text[2]) << 8) |
      ((uint32_t)(uint8_t)text[3]);
  h ^= 0xd2615b2eU;

  for (i = 4; i < len; i++) {
    h += h << 5;
    h += (uint8_t)text[i];
  }

  return h;
}

static char *name8_lowerdup(const char *text, uint32_t len) {
  char *out;
  uint32_t i;

  out = malloc((size_t)len + 1);
  if (!out)
    return NULL;

  for (i = 0; i < len; i++)
    out[i] = (char)name8_ascii_lower((unsigned char)text[i]);
  out[len] = '\0';
  return out;
}

static void *name8_make_refbuf(const char *text, uint32_t len) {
  unsigned char *buf = memalign_wrapper(8, (size_t)len + 3);
  if (!buf)
    return NULL;

  *(uint16_t *)buf = 1;
  if (len)
    memcpy(buf + 2, text, len);
  buf[2 + len] = '\0';
  return buf;
}

static void name8_string8_set(string8_obj *out, void *refbuf, uint32_t len) {
  if (!out)
    return;

  out->ref = refbuf;
  out->len = len;
  out->offset = 0;

  if (refbuf)
    (*(uint16_t *)refbuf)++;
}

static void name8_string8_clear(string8_obj *out) {
  if (!out)
    return;

  out->ref = NULL;
  out->len = 0;
  out->offset = 0;
}

static Name8CacheEntry *name8_cache_find_locked(uint32_t hash) {
  Name8CacheEntry *entry =
      g_name8_cache_buckets[hash & (NAME8_CACHE_BUCKET_COUNT - 1)];

  while (entry) {
    if (entry->hash == hash)
      return entry;
    entry = entry->next;
  }

  return NULL;
}

static Name8CacheEntry *name8_cache_store_locked(uint32_t hash, const char *text,
                                                 uint32_t len) {
  Name8CacheEntry *entry = name8_cache_find_locked(hash);
  uint32_t bucket = hash & (NAME8_CACHE_BUCKET_COUNT - 1);
  void *refbuf;

  if (entry) {
    const char *existing = entry->refbuf ? (const char *)entry->refbuf + 2 : "";
    if ((entry->len != len || memcmp(existing, text, len) != 0) &&
        g_name8_cache_collision_logs < 32) {
      debugPrintf("name8 cache collision: hash=%08x old=%s new=%.*s\n", hash,
                  existing, (int)len, text);
      g_name8_cache_collision_logs++;
    }
    return entry;
  }

  refbuf = name8_make_refbuf(text, len);
  if (!refbuf)
    return NULL;

  entry = calloc(1, sizeof(*entry));
  if (!entry) {
    free(refbuf);
    return NULL;
  }

  entry->hash = hash;
  entry->len = len;
  entry->refbuf = refbuf;
  entry->next = g_name8_cache_buckets[bucket];
  g_name8_cache_buckets[bucket] = entry;
  return entry;
}

static void name8_cache_store_text(uint32_t hash, const char *text,
                                   uint32_t len) {
  if (!hash || !text)
    return;

  pthread_mutex_lock(&g_name8_cache_lock);
  name8_cache_store_locked(hash, text, len);
  pthread_mutex_unlock(&g_name8_cache_lock);
}

static const Name8CacheEntry *name8_cache_lookup(uint32_t hash) {
  const Name8CacheEntry *entry;

  if (!hash)
    return NULL;

  pthread_mutex_lock(&g_name8_cache_lock);
  entry = name8_cache_find_locked(hash);
  pthread_mutex_unlock(&g_name8_cache_lock);
  return entry;
}

static void name8_store_hash(void *this_ptr, uint32_t hash) {
  if (this_ptr)
    *(uint32_t *)this_ptr = hash;
}

static void name8_setFromLowerText(void *this_ptr, const char *lower,
                                   uint32_t len) {
  uint32_t hash;

  if (!this_ptr)
    return;

  if (!lower || len == 0 || (len == 4 && memcmp(lower, "null", 4) == 0)) {
    name8_store_hash(this_ptr, 0);
    return;
  }

  hash = name8_hash_text(lower, len);
  name8_store_hash(this_ptr, hash);
  name8_cache_store_text(hash, lower, len);
}

static void name8_setWithString_hook(void *this_ptr, const string8_obj *value) {
  const char *text = NULL;
  uint32_t len = 0;
  char *lower;

  if (value && value->ref) {
    text = (const char *)value->ref + value->offset + 2;
    len = value->len;
  }

  if (!text || len == 0) {
    name8_store_hash(this_ptr, 0);
    return;
  }

  lower = name8_lowerdup(text, len);
  if (!lower) {
    name8_store_hash(this_ptr, 0);
    return;
  }

  name8_setFromLowerText(this_ptr, lower, len);
  free(lower);
}

static void name8_setWithText_hook(void *this_ptr, const char *text) {
  uint32_t len;
  char *lower;

  if (!text) {
    name8_store_hash(this_ptr, 0);
    return;
  }

  len = (uint32_t)strlen(text);
  if (!len) {
    name8_store_hash(this_ptr, 0);
    return;
  }

  lower = name8_lowerdup(text, len);
  if (!lower) {
    name8_store_hash(this_ptr, 0);
    return;
  }

  name8_setFromLowerText(this_ptr, lower, len);
  free(lower);
}

static void name8_toString_impl_hook(string8_obj *out, uint32_t hash) {
  const Name8CacheEntry *entry = name8_cache_lookup(hash);

  if (!entry) {
    if (g_name8_cache_miss_logs < 32 && hash != 0) {
      debugPrintf("name8 cache miss: hash=%08x\n", hash);
      g_name8_cache_miss_logs++;
    }
    name8_string8_clear(out);
    return;
  }

  name8_string8_set(out, entry->refbuf, entry->len);
}

static string8_ret make_string8_short(const char *s) {
  string8_ret out;
  memset(&out, 0, sizeof(out));

  size_t len = s ? strlen(s) : 0;
  if (len > 22)
    len = 22;

  // string8 short layout: byte0 = len << 1 (bit0 = 0 short mode),
  // bytes [1..] hold chars + trailing NUL.
  out.raw[0] = (uint8_t)(len << 1);
  if (len)
    memcpy(out.raw + 1, s, len);
  out.raw[1 + len] = '\0';

  return out;
}

string8_ret OS_GetAppVersion_hook(void) {
  // Return a stable semantic version in short-string form.
  return make_string8_short("1.0.0");
}

// OS_Playlist* stubs — same issue, AND_PlaylistInitialize was stubbed
int OS_PlaylistOpen_hook(const char *name) {
  debugPrintf("OS_PlaylistOpen: stubbed (%s)\n", name ? name : "null");
  return 0;
}

int OS_PlaylistPlay_hook(void) { return 0; }
int OS_PlaylistStop_hook(void) { return 0; }
int OS_PlaylistPause_hook(void) { return 0; }
int OS_PlaylistCount_hook(void) { return 0; }
int OS_PlaylistIsPlaying_hook(void) { return 0; }
int OS_PlaylistAvailable_hook(void) { return 0; }
void OS_PlaylistSetVolume_hook(float vol) {}

// AND_SystemInitialize — fully stubbed.
// The original does JNI dispatch (FindClass "DeviceInfo", GetStaticFieldID
// "isPhone") which re-enters the hook through the patched GOT, causing infinite
// recursion. We write the isPhone flag directly at the known .bss offset.
void AND_SystemInitialize_hook(void *env, void *activity) {
  debugPrintf("AND_SystemInitialize_hook: stubbed (setting isPhone=1)\n");
  // After so_finalize(), text_base is inaccessible (unmapped by
  // svcMapProcessCodeMemory). text_virtbase is the runtime mapping base.
  // Data/BSS past text_asize is RW.
  extern void *text_virtbase;
  volatile uint32_t *isPhone =
      (volatile uint32_t *)((uintptr_t)text_virtbase + ISPHONE_FLAG_OFFSET);
  *isPhone = 1;
  debugPrintf("AND_SystemInitialize_hook: isPhone flag set at %p\n",
              (void *)isPhone);
}

char *OS_FileGetArchiveName(int mode) {
  char *out = malloc(strlen(APK_PATH) + 1);
  out[0] = '\0';
  if (mode == 1) // main
    strcpy(out, APK_PATH);
  return out;
}

void ExitAndroidGame(int code) {
  // deinit openal
  deinit_openal();
  // deinit EGL
  deinit_opengl();
  // unmap lib
  so_unload();
  // die
  // exit(0); // doesn't actually exit?
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
}

// OS_ThreadMakeCurrent / OS_ThreadUnmakeCurrent — the game stores EGL objects
// in internal globals that are never populated on Switch. Hook these to use
// our known-good EGL display/surface/context directly.
void OS_ThreadMakeCurrent_hook(void) {
  debugPrintf("OS_ThreadMakeCurrent_hook: acquiring context on thread\n");
  EGLBoolean ret = eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface,
                                  g_egl_context);
  if (!ret)
    debugPrintf("OS_ThreadMakeCurrent_hook: eglMakeCurrent FAILED (err=%x)\n",
                eglGetError());
}

void OS_ThreadUnmakeCurrent_hook(void) {
  debugPrintf("OS_ThreadUnmakeCurrent_hook: releasing context on thread\n");
  eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

// OS_ScreenSwapBuffers — the original reads EGL display/surface from globals
// at 0x158a978/0x158a980, which are populated by the Android Java launcher.
// On Switch those globals are NULL. Hook to use our known-good EGL handles.
static int g_swap_count = 0;
void OS_ScreenSwapBuffers_hook(void) {
  if (g_swap_count < 5 || (g_swap_count % 300 == 0))
    debugPrintf("OS_ScreenSwapBuffers_hook: swap #%d\n", g_swap_count);
  g_swap_count++;
  eglSwapBuffers(g_egl_display, g_egl_surface);
}
extern int diag_get_egl_swap_count(void);
int diag_get_swap_count(void) {
  return g_swap_count + diag_get_egl_swap_count();
}

// BullyApplication::SetGameState hook — log state transitions
// Original at 0xf20308: stores w1 to this+0x68, then if this+0x98 != NULL
// tail-calls MainMenu::UpdateAccess
static void (*saved_MainMenu_UpdateAccess)(void *) = NULL;

void BullyApplication_SetGameState_hook(void *this_ptr, int state) {
  debugPrintf("SetGameState: state=%d (this=%p)\n", state, this_ptr);
  *(volatile int *)((uintptr_t)this_ptr + 0x68) = state;
  // Replicate tail-call: MainMenu::UpdateAccess(this->0x98)
  void *menu_obj = *(void **)((uintptr_t)this_ptr + 0x98);
  if (menu_obj && saved_MainMenu_UpdateAccess)
    saved_MainMenu_UpdateAccess(menu_obj);
}

// Thread wrapper: each game thread needs its own TPIDR_EL0 buffer for stack
// guard canary
typedef struct {
  int (*func)(void *);
  void *arg;
  char *handle;
  int is_gamemain;
} OsThreadData;

volatile int g_gamemain_alive = 0; // 0=not started, 1=running, 2=returned

// imports.c wrapper: allocates per-thread TPIDR_EL0 storage before entry.
int pthread_create_fake(pthread_t *thread, const void *unused, void *entry,
                        void *arg);
int thread_guard_run_int(int (*func)(void *), void *arg);

static void *os_thread_entry(void *data) {
  OsThreadData *td = (OsThreadData *)data;
  int (*func)(void *) = td->func;
  void *arg = td->arg;
  char *handle = td->handle;
  int is_gamemain = td->is_gamemain;
  free(td);

  uint8_t *tls = calloc(1, 0x200);
  armSetTlsRw(tls);

  // In the 64-bit Bully binary, OS_ThreadIsRunning reads byte 0x69 from
  // the returned handle. It must stay non-zero while the thread is alive,
  // then be cleared when the thread exits.
  if (handle)
    handle[0x69] = 1;

  debugPrintf("os_thread_entry: starting thread func=%p arg=%p\n", (void *)func,
              arg);

  if (is_gamemain)
    g_gamemain_alive = 1;

  int ret = thread_guard_run_int(func, arg);

  if (handle)
    handle[0x69] = 0;

  if (is_gamemain)
    g_gamemain_alive = 2;

  debugPrintf("os_thread_entry: thread func returned %d\n", ret);
  return (void *)(intptr_t)ret;
}

// Returns a thread handle struct large enough for the engine's private data.
// The 64-bit Bully binary reads:
// - byte 0x69 for OS_ThreadIsRunning
// - qword 0x28 for OS_ThreadWait / pthread_join
void *OS_ThreadLaunch(int (*func)(void *), void *arg, int r2, char *name,
                      int r4, int priority) {
  debugPrintf("OS_ThreadLaunch: '%s'\n", name ? name : "unnamed");

  // Allocate a handle large enough for the engine's thread struct.
  char *handle = calloc(1, 0x400);
  if (!handle)
    return NULL;

  OsThreadData *td = malloc(sizeof(OsThreadData));
  if (!td) {
    free(handle);
    return NULL;
  }
  td->func = func;
  td->arg = arg;
  td->handle = handle;
  td->is_gamemain = (name && strcmp(name, "GameMain") == 0);

  pthread_t thrd;
  int rc = pthread_create(&thrd, NULL, os_thread_entry, td);
  if (rc != 0) {
    debugPrintf("OS_ThreadLaunch: pthread_create failed (%d)\n", rc);
    free(td);
    free(handle);
    return NULL;
  }

  // Mark as running; entry keeps this set until thread returns.
  handle[0x69] = 1;
  memcpy(handle + 0x28, &thrd, sizeof(thrd));
  return handle;
}

void OS_ThreadWait_hook(void *thread) {
  if (!thread)
    return;

  pthread_t thrd;
  memset(&thrd, 0, sizeof(thrd));
  memcpy(&thrd, (char *)thread + 0x28, sizeof(thrd));

  if (!memcmp(&thrd, &(pthread_t){0}, sizeof(thrd)))
    return;

  pthread_join(thrd, NULL);
}

// The 64-bit game's NVThreadSpawnJNIThread worker dereferences
// pthread_getspecific(key) without null checks during startup, which crashes
// on Switch when the expected JNI TLS slot is unset. Bypass that wrapper.
int NVThreadSpawnJNIThread_hook(long *outThread, const pthread_attr_t *attr,
                                const char *name, void *(*entry)(void *),
                                void *arg) {
  (void)attr; // pthread_create_fake always uses default attributes.

  if (!entry)
    return -1;

  pthread_t thrd;
  int rc = pthread_create_fake(&thrd, NULL, (void *)entry, arg);
  if (rc == 0 && outThread)
    memcpy(outThread, &thrd, umin(sizeof(*outThread), sizeof(thrd)));

  if (rc != 0)
    debugPrintf("NVThreadSpawnJNIThread_hook: create failed rc=%d name=%s\n",
                rc, name ? name : "(null)");

  return rc;
}

int ReadDataFromPrivateStorage(const char *file, void **data, int *size) {
  debugPrintf("ReadDataFromPrivateStorage %s\n", file);

  FILE *f = fopen(file, "rb");
  if (!f)
    return 0;

  fseek(f, 0, SEEK_END);
  const int sz = ftell(f);
  fseek(f, 0, SEEK_SET);

  int ret = 0;

  if (sz > 0) {
    void *buf = malloc(sz);
    if (buf && fread(buf, sz, 1, f)) {
      ret = 1;
      *size = sz;
      *data = buf;
    } else {
      free(buf);
    }
  }

  fclose(f);

  return ret;
}

int WriteDataToPrivateStorage(const char *file, const void *data, int size) {
  debugPrintf("WriteDataToPrivateStorage %s\n", file);

  FILE *f = fopen(file, "wb");
  if (!f)
    return 0;

  const int ret = fwrite(data, size, 1, f);
  fclose(f);

  return ret;
}

// 0, 5, 6: XBOX 360
// 4: MogaPocket
// 7: MogaPro
// 8: PS3
// 9: IOSExtended
// 10: IOSSimple
int WarGamepad_GetGamepadType(int padnum) {
  (void)padnum;
  nx_input_ensure_snapshot();
  if (!g_nx_input.connected)
    return 0;
  return 8;
}

int WarGamepad_GetGamepadButtons(int padnum) {
  int mask = 0;

  (void)padnum;
  nx_input_ensure_snapshot();
  if (!g_nx_input.connected)
    return 0;

  if (nx_input_button_from_mask(g_nx_input.held, 0))
    mask |= 0x1;
  if (nx_input_button_from_mask(g_nx_input.held, 1))
    mask |= 0x2;
  if (nx_input_button_from_mask(g_nx_input.held, 2))
    mask |= 0x4;
  if (nx_input_button_from_mask(g_nx_input.held, 3))
    mask |= 0x8;
  if (nx_input_button_from_mask(g_nx_input.held, 4))
    mask |= 0x10;
  if (nx_input_button_from_mask(g_nx_input.held, 5))
    mask |= 0x20;
  if (nx_input_button_from_mask(g_nx_input.held, 16))
    mask |= 0x40;
  if (nx_input_button_from_mask(g_nx_input.held, 18))
    mask |= 0x80;
  if (nx_input_button_from_mask(g_nx_input.held, 8))
    mask |= 0x100;
  if (nx_input_button_from_mask(g_nx_input.held, 9))
    mask |= 0x200;
  if (nx_input_button_from_mask(g_nx_input.held, 10))
    mask |= 0x400;
  if (nx_input_button_from_mask(g_nx_input.held, 11))
    mask |= 0x800;
  if (nx_input_button_from_mask(g_nx_input.held, 12))
    mask |= 0x1000;
  if (nx_input_button_from_mask(g_nx_input.held, 13))
    mask |= 0x2000;

  return mask;
}

float WarGamepad_GetGamepadAxis(int padnum, int axis) {
  float val = 0.0f;

  (void)padnum;
  nx_input_ensure_snapshot();
  if (!g_nx_input.connected)
    return 0.0f;

  switch (axis) {
  case 0:
    val = g_nx_input.lx;
    break;
  case 1:
    val = g_nx_input.ly;
    break;
  case 2:
    val = g_nx_input.rx;
    break;
  case 3:
    val = g_nx_input.ry;
    break;
  case 4: // LT
    val = nx_input_button_from_mask(g_nx_input.held, 17) ? 1.0f : 0.0f;
    break;
  case 5: // RT
    val = nx_input_button_from_mask(g_nx_input.held, 19) ? 1.0f : 0.0f;
    break;
  }
  return val;
}

// Hook game's own debug output so we see what the engine prints
void OS_DebugOutFmt_hook(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  debugPrintf("OS_DebugOutFmt: %s\n", buf);
}

void OS_DebugOutFmtForce_hook(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  debugPrintf("OS_DebugOutFmtForce: %s\n", buf);
}

void OS_DebugOut_hook(const char *msg) {
  debugPrintf("OS_DebugOut: %s\n", msg ? msg : "(null)");
}

void LoadingScreenLoadingFile_hook(const char *path) {
  debugPrintf("LoadingScreenLoadingFile: %s\n", path ? path : "(null)");
}

// ============================================================================
// BullyApplication::OrigInitialize trace wrapper
// Hooks the original function to log entry and exit, helping diagnose where
// the init sequence crashes or hangs.
// ============================================================================
static void (*OrigInitialize_real)(void *this_ptr);

void OrigInitialize_hook(void *this_ptr) {
  debugPrintf(">>> OrigInitialize ENTERED (this=%p)\n", this_ptr);
  OrigInitialize_real(this_ptr);
  debugPrintf("<<< OrigInitialize RETURNED\n");
}

// ============================================================================
// GOT-based hooks — applied AFTER so_finalize() when data segment is RW.
// These intercept functions called through PLT/GOT without the 16-byte
// hook_arm64 trampoline, allowing us to call the original function.
// ============================================================================
#define ORIG_INITIALIZE_GOT 0x1233820
#define ORIG_PRETICK_GOT 0x1233838

static void (*OrigPretick_real)(void *this_ptr, int flag);

static void OrigPretick_hook(void *this_ptr, int flag) {
  g_nx_input.valid = 0;
  nx_input_refresh_snapshot();
  OrigPretick_real(this_ptr, flag);
}

void patch_game_post_finalize(void) {
  extern void *text_virtbase;

  // GOT-patch OrigInitialize
  uintptr_t *got_init =
      (uintptr_t *)((uintptr_t)text_virtbase + ORIG_INITIALIZE_GOT);
  OrigInitialize_real = (void (*)(void *))*got_init;
  *got_init = (uintptr_t)OrigInitialize_hook;
  debugPrintf("GOT-patched OrigInitialize: real=%p hook=%p\n",
              (void *)OrigInitialize_real, (void *)OrigInitialize_hook);

  // GOT-patch OrigPretick
  uintptr_t *got_pre =
      (uintptr_t *)((uintptr_t)text_virtbase + ORIG_PRETICK_GOT);
  OrigPretick_real = (void (*)(void *, int))*got_pre;
  *got_pre = (uintptr_t)OrigPretick_hook;
  debugPrintf("GOT-patched OrigPretick: real=%p hook=%p\n",
              (void *)OrigPretick_real, (void *)OrigPretick_hook);
}

void patch_game(void) {
  // NVThreadGetCurrentJNIEnv must return our fake JNI env
  extern void *NVThreadGetCurrentJNIEnv(void);
  hook_arm64(so_find_addr("_Z24NVThreadGetCurrentJNIEnvv"),
             (uintptr_t)NVThreadGetCurrentJNIEnv);

  // Bypass game's JNI-thread wrapper (crashes on null pthread_getspecific).
  hook_arm64(
      so_find_addr(
          "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_"),
      (uintptr_t)NVThreadSpawnJNIThread_hook);

  hook_arm64(so_find_addr("__cxa_throw"), (uintptr_t)&__cxa_throw_wrapper);
  hook_arm64(so_find_addr("__cxa_guard_acquire"),
             (uintptr_t)&__cxa_guard_acquire);
  hook_arm64(so_find_addr("__cxa_guard_release"),
             (uintptr_t)&__cxa_guard_release);

  // Use Switch-safe thread launcher/wait.
  // The Android path can fail to spawn engine workers cleanly on 64-bit emu.
  hook_arm64(so_find_addr("_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority"),
             (uintptr_t)OS_ThreadLaunch);
  hook_arm64(so_find_addr("_Z13OS_ThreadWaitPv"),
             (uintptr_t)OS_ThreadWait_hook);

  // Keep original EGL thread/swap functions so the engine's hidden state
  // transitions are preserved. jni_start now seeds their internal EGL globals.

  // Hook SetGameState to log game state transitions
  // Pre-resolve MainMenu::UpdateAccess NOW — so_find_addr_rx reads from
  // text_base which becomes Perm_None after so_finalize, so it cannot be called
  // at runtime.
  saved_MainMenu_UpdateAccess =
      (void (*)(void *))so_find_addr_rx("_ZN8MainMenu12UpdateAccessEv");
  hook_arm64(
      so_find_addr("_ZN16BullyApplication12SetGameStateE14BullyGameState"),
      (uintptr_t)BullyApplication_SetGameState_hook);

  // Hook game's own debug output to capture engine messages
  hook_arm64(so_find_addr("_Z14OS_DebugOutFmtPKcz"),
             (uintptr_t)OS_DebugOutFmt_hook);
  hook_arm64(so_find_addr("_Z19OS_DebugOutFmtForcePKcz"),
             (uintptr_t)OS_DebugOutFmtForce_hook);
  hook_arm64(so_find_addr("_Z11OS_DebugOutPKc"), (uintptr_t)OS_DebugOut_hook);
  hook_arm64(so_find_addr("_Z24LoadingScreenLoadingFilePKc"),
             (uintptr_t)LoadingScreenLoadingFile_hook);
  hook_arm64(so_find_addr("_ZN5name813setWithStringERK7string8"),
             (uintptr_t)name8_setWithString_hook);
  hook_arm64(so_find_addr("_ZN5name811setWithTextEPKc"),
             (uintptr_t)name8_setWithText_hook);
  hook_arm64((uintptr_t)text_base + NAME8_TOSTRING_IMPL_OFFSET,
             (uintptr_t)name8_toString_impl_hook);
  hook_arm64(so_find_addr("_ZN17ActionTreeDecoder15decodeStringRefEv"),
             (uintptr_t)ActionTreeDecoder_decodeStringRef_hook);
  hook_arm64((uintptr_t)text_base + BULLY_SETTINGS_RESET_DISPLAY_PLT_OFFSET,
             (uintptr_t)BullySettings_ResetDisplay_hook);
  hook_arm64((uintptr_t)text_base + BULLY_SETTINGS_LOAD_PLT_OFFSET,
             (uintptr_t)BullySettings_Load_hook);

  // --- SYMBOLS BELOW DO NOT EXIST IN 64-BIT libGame.so ---
  // hook_arm64(so_find_addr("_Z20OS_ServiceAppCommandPKcS0_"),
  // (uintptr_t)ret0);
  // hook_arm64(so_find_addr("_Z23OS_ServiceAppCommandIntPKci"),
  // (uintptr_t)ret0);
  // hook_arm64(so_find_addr("_Z25OS_ServiceIsWifiAvailablev"),
  // (uintptr_t)ret0);
  // hook_arm64(so_find_addr("_Z28OS_ServiceIsNetworkAvailablev"),
  // (uintptr_t)ret0); hook_arm64(so_find_addr("_Z18OS_ServiceOpenLinkPKc"),
  // (uintptr_t)ret0);

  // don't have movie playback yet
  hook_arm64(so_find_addr("_Z12OS_MoviePlayPKcbbf"), (uintptr_t)ret0);
  hook_arm64(so_find_addr("_Z12OS_MovieStopv"), (uintptr_t)ret0);
  hook_addr_ret0(
      so_find_addr("_Z20OS_MovieSetSkippableb")); // 4B func, inline ret0
  hook_addr_ret0(
      so_find_addr("_Z17OS_MovieTextScalei")); // 4B func, inline ret0
  hook_arm64(so_find_addr("_Z17OS_MovieIsPlayingPi"), (uintptr_t)ret0);
  hook_arm64(so_find_addr("_Z20OS_MoviePlayinWindowPKciiiibbf"),
             (uintptr_t)ret0);
  hook_arm64(so_find_addr("_Z15OS_MovieSetTextPKcbb"), (uintptr_t)ret0);
  hook_arm64(so_find_addr("_Z13OS_MoviePauseb"), (uintptr_t)ret0);
  hook_addr_ret0(
      so_find_addr("_Z19OS_MovieDisplayTextb")); // 4B func, inline ret0
  hook_arm64(so_find_addr("_Z17OS_MovieClearTextb"), (uintptr_t)ret0);

  hook_arm64(so_find_addr("_Z17OS_ScreenGetWidthv"),
             (uintptr_t)OS_ScreenGetWidth);
  hook_arm64(so_find_addr("_Z18OS_ScreenGetHeightv"),
             (uintptr_t)OS_ScreenGetHeight);
  hook_arm64(so_find_addr("_ZN8IOBuffer18BlockUntilCompleteEv"),
             (uintptr_t)IOBuffer_BlockUntilComplete_hook);
  hook_arm64(so_find_addr("_ZN14FileReadBuffer18BlockUntilCompleteEv"),
             (uintptr_t)FileReadBuffer_BlockUntilComplete_hook);
  hook_arm64(so_find_addr("_Z16OS_CanGameRenderv"),
             (uintptr_t)OS_CanGameRender);
  hook_arm64(so_find_addr("_Z18OS_IsGameSuspendedv"),
             (uintptr_t)OS_IsGameSuspended);
  // Leave the 64-bit build on the JNI callback path for digital buttons.
  // The direct native gamepad hooks below caused the game to lose button
  // events entirely while axes still worked.
  // hook_arm64(so_find_addr("_Z21OS_GamepadIsConnectedjP13OSGamepadType"),
  //            (uintptr_t)OS_GamepadIsConnected_hook);
  // hook_arm64(so_find_addr("_Z16OS_GamepadButtonjj"),
  //            (uintptr_t)OS_GamepadButton_hook);
  // hook_arm64(so_find_addr("_Z13PadUsesXInputi"),
  //            (uintptr_t)PadUsesXInput_hook);
  // hook_arm64(so_find_addr("_Z16LIB_GamepadStateii"),
  //            (uintptr_t)LIB_GamepadState_hook);
  // hook_arm64(so_find_addr("_ZN15InputController12GetGBPressedEj13GamepadButton"),
  //            (uintptr_t)InputController_GetGBPressed_hook);
  // hook_arm64(so_find_addr("_ZN15InputController9GetGBDownEj13GamepadButton"),
  //            (uintptr_t)InputController_GetGBDown_hook);
  // hook_arm64(so_find_addr("_ZN15InputController13GetGBReleasedEj13GamepadButton"),
  //            (uintptr_t)InputController_GetGBReleased_hook);

  // Hook AND_*Initialize functions — stub out subsystems not needed on Switch.
  // These are called from implOnInitialSetup and only register JNI method IDs
  // for Android features (splash screen, social club, playlists, keyboard,
  // etc.)
  hook_arm64(so_find_addr("_Z19AND_TouchInitializeP7_JNIEnvP8_jobject"),
             (uintptr_t)AND_TouchInitialize_hook);
  hook_arm64(so_find_addr("_Z19AND_MovieInitializeP7_JNIEnv"),
             (uintptr_t)AND_MovieInitialize_hook);
  hook_arm64(so_find_addr("_Z20AND_HapticInitializeP7_JNIEnv"),
             (uintptr_t)AND_HapticInitialize_hook);
  // Leave Rockstar init native for diagnostics (allows original login flow).
  // hook_arm64(so_find_addr("_Z22AND_RockstarInitializeP7_JNIEnv"),
  // (uintptr_t)AND_RockstarInitialize_hook);
  hook_arm64(so_find_addr("_Z21AND_DisplayInitializeP7_JNIEnv"),
             (uintptr_t)AND_DisplayInitialize_hook);
  hook_arm64(so_find_addr("_Z22AND_LanguageInitializeP7_JNIEnv"),
             (uintptr_t)ret0);
  hook_arm64(so_find_addr("_Z22AND_PlaylistInitializeP7_JNIEnv"),
             (uintptr_t)AND_PlaylistInitialize_hook);
  hook_arm64(so_find_addr("_Z18AND_MiscInitializeP7_JNIEnv"),
             (uintptr_t)AND_MiscInitialize_hook);
  hook_arm64(so_find_addr("_Z22AND_KeyboardInitializeP7_JNIEnv"),
             (uintptr_t)AND_KeyboardInitialize_hook);

  // AND_SystemInitialize — fully stubbed (original does JNI dispatch → infinite
  // recursion)
  hook_arm64(so_find_addr("_Z20AND_SystemInitializeP7_JNIEnvP8_jobject"),
             (uintptr_t)AND_SystemInitialize_hook);

  // Keep Rockstar/SC native, but bypass phase wait deadlocks that occur
  // when Android async callbacks don't line up 1:1 on Switch.
  hook_arm64(so_find_addr("_Z16WaitForNextPhase17MULTIPLAYER_PHASE"),
             (uintptr_t)WaitForNextPhase_hook);

  // OS_GetAppVersion — does JNI dispatch, stub it
  hook_arm64(so_find_addr("_Z16OS_GetAppVersionv"),
             (uintptr_t)OS_GetAppVersion_hook);

  hook_arm64(so_find_addr("_ZN15ControllerScene23ShowGamepadInstructionsEv"),
             (uintptr_t)ControllerScene_ShowGamepadInstructions_hook);
  hook_arm64(so_find_addr("_ZN8HUDScene17OnSwitchToGamepadEv"),
             (uintptr_t)HUDScene_OnSwitchToGamepad_hook);
  hook_arm64(so_find_addr("_ZN11MenuWrapper12CheckGamepadEv"),
             (uintptr_t)MenuWrapper_CheckGamepad_hook);

  // OS_Playlist* stubs — AND_PlaylistInitialize was stubbed so method IDs are
  // NULL
  hook_arm64(so_find_addr("_Z15OS_PlaylistOpenPKc"),
             (uintptr_t)OS_PlaylistOpen_hook);
  hook_arm64(so_find_addr("_Z15OS_PlaylistPlayv"),
             (uintptr_t)OS_PlaylistPlay_hook);
  hook_arm64(so_find_addr("_Z15OS_PlaylistStopv"),
             (uintptr_t)OS_PlaylistStop_hook);
  hook_arm64(so_find_addr("_Z16OS_PlaylistPausev"),
             (uintptr_t)OS_PlaylistPause_hook);
  hook_arm64(so_find_addr("_Z16OS_PlaylistCountv"),
             (uintptr_t)OS_PlaylistCount_hook);
  hook_arm64(so_find_addr("_Z20OS_PlaylistIsPlayingv"),
             (uintptr_t)OS_PlaylistIsPlaying_hook);
  hook_addr_ret0(
      so_find_addr("_Z20OS_PlaylistAvailablev")); // 12B func, inline ret0
  hook_arm64(so_find_addr("_Z20OS_PlaylistSetVolumef"),
             (uintptr_t)OS_PlaylistSetVolume_hook);

  // OS_Language* stub
  hook_arm64(so_find_addr("_Z23OS_LanguageUserSelectedv"), (uintptr_t)ret0);

  // NvAPK filesystem replacements — hook ALL NvAPK functions so the game
  // reads directly from assets/ instead of going through AAssetManager/JNI.
  hook_arm64(so_find_addr("_Z9NvAPKInitP8_jobjectP13_jobjectArrayS2_"),
             (uintptr_t)NvAPKInit_hook);
  hook_arm64(so_find_addr("_Z9NvAPKOpenPKc"), (uintptr_t)NvAPKOpen_hook);
  hook_arm64(so_find_addr("_Z17NvAPKOpenFromPackPKc"),
             (uintptr_t)NvAPKOpenFromPack_hook);
  hook_arm64(so_find_addr("_Z10NvAPKClosePv"), (uintptr_t)NvAPKClose_hook);
  hook_arm64(so_find_addr("_Z9NvAPKReadPvmmS_"), (uintptr_t)NvAPKRead_hook);
  hook_arm64(so_find_addr("_Z9NvAPKSeekPvli"), (uintptr_t)NvAPKSeek_hook);
  hook_arm64(so_find_addr("_Z9NvAPKTellPv"), (uintptr_t)NvAPKTell_hook);
  hook_arm64(so_find_addr("_Z9NvAPKSizePv"), (uintptr_t)NvAPKSize_hook);
  hook_arm64(so_find_addr("_Z8NvAPKEOFPv"), (uintptr_t)NvAPKEOF_hook);
  hook_arm64(so_find_addr("_Z9NvAPKGetcPv"), (uintptr_t)NvAPKGetc_hook);
  hook_arm64(so_find_addr("_Z9NvAPKGetsPciPv"), (uintptr_t)NvAPKGets_hook);

  // NvF* wrapper layer — intercept the game's file wrapper API that sits
  // between OS_File* and NvAPK*. This gives us full control and visibility
  // over ALL file I/O. NOTE: NvFInit is only 4 bytes, too small to hook.
  // hook_arm64(so_find_addr("_Z7NvFInitv"), (uintptr_t)NvFInit_hook);  // TOO
  // SMALL
  hook_arm64(so_find_addr("_Z7NvFOpenPKc"), (uintptr_t)NvFOpen_hook);
  hook_arm64(so_find_addr("_Z12NvFIsApkFilePv"), (uintptr_t)NvFIsApkFile_hook);
  hook_arm64(so_find_addr("_Z8NvFClosePv"), (uintptr_t)NvFClose_hook);
  hook_arm64(so_find_addr("_Z7NvFReadPvmmS_"), (uintptr_t)NvFRead_hook);
  hook_arm64(so_find_addr("_Z7NvFSizePv"), (uintptr_t)NvFSize_hook);
  hook_arm64(so_find_addr("_Z7NvFSeekPvli"), (uintptr_t)NvFSeek_hook);
  hook_arm64(so_find_addr("_Z7NvFTellPv"), (uintptr_t)NvFTell_hook);
  hook_arm64(so_find_addr("_Z6NvFEOFPv"), (uintptr_t)NvFEOF_hook);
  hook_arm64(so_find_addr("_Z7NvFGetcPv"), (uintptr_t)NvFGetc_hook);
  hook_arm64(so_find_addr("_Z7NvFGetsPciPv"), (uintptr_t)NvFGets_hook);

  // --- SYMBOLS BELOW DO NOT EXIST IN 64-BIT libGame.so ---
  // hook_arm64(so_find_addr("_Z13ProcessEventsb"), (uintptr_t)ProcessEvents);
  // hook_arm64(so_find_addr("_Z25GetAndroidCurrentLanguagev"),
  // (uintptr_t)GetAndroidCurrentLanguage);
  // hook_arm64(so_find_addr("_Z25SetAndroidCurrentLanguagei"),
  // (uintptr_t)SetAndroidCurrentLanguage);
  // hook_arm64(so_find_addr("_Z14AND_DeviceTypev"), (uintptr_t)AND_DeviceType);
  // hook_arm64(so_find_addr("_Z16AND_DeviceLocalev"),
  // (uintptr_t)AND_DeviceLocale);
  // hook_arm64(so_find_addr("_Z20AND_SystemInitializev"),
  // (uintptr_t)AND_SystemInitialize);
  // hook_arm64(so_find_addr("_Z21AND_ScreenSetWakeLockb"), (uintptr_t)ret0);
  // hook_arm64(so_find_addr("_Z25WarGamepad_GetGamepadTypei"),
  //            (uintptr_t)WarGamepad_GetGamepadType);
  // hook_arm64(so_find_addr("_Z28WarGamepad_GetGamepadButtonsi"),
  //            (uintptr_t)WarGamepad_GetGamepadButtons);
  // hook_arm64(so_find_addr("_Z25WarGamepad_GetGamepadAxisii"),
  //            (uintptr_t)WarGamepad_GetGamepadAxis);
  // hook_arm64(so_find_addr("_Z12VibratePhonei"), (uintptr_t)ret0);
  // hook_arm64(so_find_addr("_Z14Mobile_Vibratei"), (uintptr_t)ret0);
  // hook_arm64(so_find_addr("_Z15ExitAndroidGamev"),
  // (uintptr_t)ExitAndroidGame);

  // Stack canary fix: the binary uses TLS-based canaries (mrs tpidr_el0; ldr
  // [+0x28]) which fail on Switch because that TLS offset is the IPC buffer.
  // Disable all checks.
  patch_stack_canary();

  // Still set fake TLS so mrs tpidr_el0 doesn't return 0/garbage (would crash
  // on ldr)
  armSetTlsRw(fake_tls);
}
