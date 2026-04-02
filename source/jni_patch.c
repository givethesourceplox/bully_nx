/* jni_patch.c -- Fake Java Native Interface
 *
 * Copyright (C) 2026 givethesourceplox, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#include "main.h"
#include "config.h"
#include "so_util.h"
#include "hooks.h"
#include "util.h"

enum MethodIDs
{
  UNKNOWN = 0,
  INIT_EGL_AND_GLES2,
  SWAP_BUFFERS,
  MAKE_CURRENT,
  UN_MAKE_CURRENT,
  SHARE_TEXT,
  SHARE_IMAGE,
  HAS_APP_LOCAL_VALUE,
  GET_APP_LOCAL_VALUE,
  SET_APP_LOCAL_VALUE,
  GET_PARAMETER,
  FILE_GET_ARCHIVE_NAME,
  DELETE_FILE,
  GET_DEVICE_INFO,
  GET_DEVICE_TYPE,
  GET_DEVICE_LOCALE,
  GET_GAMEPAD_TYPE,
  GET_GAMEPAD_BUTTONS,
  GET_GAMEPAD_AXIS,
  ROCKSTAR_SHOW_GATE,
  ROCKSTAR_SHOW_INITIAL,
  ROCKSTAR_SIGN_IN,
  ROCKSTAR_SIGN_OUT,
  ROCKSTAR_DELETE_ACCOUNT,
  ROCKSTAR_SET_LOCALE_PRIORITY,
  ROCKSTAR_IN_TRIAL,
  ROCKSTAR_SHOW_CLOUD_DISABLED,
  ROCKSTAR_REQUEST_REVIEW,
  ROCKSTAR_LOG_ERROR,
  ROCKSTAR_LOG_BREADCRUMB,
  HTTP_HEAD,
  HTTP_GET,
  HTTP_POST,
  HTTP_CANCEL,
  QUIT,
} MethodIDs;

typedef struct
{
  char *name;
  enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
    {"InitEGLAndGLES2", INIT_EGL_AND_GLES2},
    {"swapBuffers", SWAP_BUFFERS},
    {"makeCurrent", MAKE_CURRENT},
    {"unMakeCurrent", UN_MAKE_CURRENT},

    {"ShareText", SHARE_TEXT},
    {"ShareImage", SHARE_IMAGE},

    {"hasAppLocalValue", HAS_APP_LOCAL_VALUE},
    {"getAppLocalValue", GET_APP_LOCAL_VALUE},
    {"setAppLocalValue", SET_APP_LOCAL_VALUE},
    {"getParameter", GET_PARAMETER},

    {"FileGetArchiveName", FILE_GET_ARCHIVE_NAME},
    {"DeleteFile", DELETE_FILE},

    {"GetDeviceInfo", GET_DEVICE_INFO},
    {"GetDeviceType", GET_DEVICE_TYPE},
    {"GetDeviceLocale", GET_DEVICE_LOCALE},

    {"GetGamepadType", GET_GAMEPAD_TYPE},
    {"GetGamepadButtons", GET_GAMEPAD_BUTTONS},
    {"GetGamepadAxis", GET_GAMEPAD_AXIS},

    {"rockstarShowGate", ROCKSTAR_SHOW_GATE},
    {"rockstarShowInitial", ROCKSTAR_SHOW_INITIAL},
    {"rockstarSignIn", ROCKSTAR_SIGN_IN},
    {"rockstarSignOut", ROCKSTAR_SIGN_OUT},
    {"rockstarDeleteAccount", ROCKSTAR_DELETE_ACCOUNT},
    {"rockstarSetLocalePriority", ROCKSTAR_SET_LOCALE_PRIORITY},
    {"rockstarInTrial", ROCKSTAR_IN_TRIAL},
    {"rockstarShowCloudDisabled", ROCKSTAR_SHOW_CLOUD_DISABLED},
    {"rockstarRequestReview", ROCKSTAR_REQUEST_REVIEW},
    {"rockstarLogError", ROCKSTAR_LOG_ERROR},
    {"rockstarLogBreadcrumb", ROCKSTAR_LOG_BREADCRUMB},

    {"httpHead", HTTP_HEAD},
    {"httpGet", HTTP_GET},
    {"httpPost", HTTP_POST},
    {"httpCancel", HTTP_CANCEL},

    {"quit", QUIT},
};

static volatile int g_rockstar_pending_initial = 0;
static volatile int g_rockstar_pending_gate = 0;
static volatile int g_rockstar_pending_gate_type = 0;
static volatile int g_rockstar_pending_signin = 0;

typedef struct
{
  u64 nx_mask;
  int mask_bit;
  int game_button;
  const char *name;
} JniGamepadButtonMap;

static char fake_vm[0x1000];
static char fake_env[0x1000];
static void *natives;
static PadState g_jni_pad;
static int g_jni_pad_initialized = 0;
static int g_jni_pad_connected = -1;
static u64 g_jni_pad_buttons = 0;
static float g_jni_pad_axes[6];
static int g_jni_pad_axes_valid = 0;
static int g_jni_gamepad_log_count = 0;
static volatile uint8_t *g_jni_gamepad_slots = NULL;
static volatile int *g_jni_lib_gamepad_state = NULL;
static int g_sdl_gamepad_initialized = 0;
static int g_sdl_gamepad_failed = 0;
static SDL_GameController *g_sdl_gamepad = NULL;

static const JniGamepadButtonMap g_jni_gamepad_buttons[] = {
    // GamepadButton enum from libGame:
    // 0=A, 1=B, 2=X, 3=Y, 4=START, 5=BACK, 6=L3, 7=R3,
    // 8=NAV_UP, 9=NAV_DOWN, 10=NAV_LEFT, 11=NAV_RIGHT,
    // 12=DPAD_UP, 13=DPAD_DOWN, 14=DPAD_LEFT, 15=DPAD_RIGHT,
    // 16=LEFT_SHOULDER, 17=LEFT_TRIGGER, 18=RIGHT_SHOULDER, 19=RIGHT_TRIGGER.
    // On Switch, we want the physical D-pad to drive the NAV_* actions
    // (zoom/tasks/objectives) while L3/R3 take over the older DPAD_UP/DOWN
    // gameplay actions (look back / crouch).
    {HidNpadButton_B, 0x1, 0, "south"},
    {HidNpadButton_A, 0x2, 1, "east"},
    {HidNpadButton_Y, 0x4, 2, "west"},
    {HidNpadButton_X, 0x8, 3, "north"},
    {HidNpadButton_Plus, 0x10, 4, "plus"},
    {HidNpadButton_Minus, 0x20, 5, "minus"},
    // L3/R3 need held-state semantics for look back / crouch, so keep them in
    // both the callback path and the held-button mask path.
    {HidNpadButton_StickL, 0x1000, 12, "l3"},
    {HidNpadButton_StickR, 0x2000, 13, "r3"},
    // Physical D-pad drives the active Android callback NAV_* actions.
    // Keep these off the legacy GetGamepadButtons mask path to avoid colliding
    // with the older Android D-pad semantics in this build.
    {HidNpadButton_Up, 0, 8, "up"},
    {HidNpadButton_Down, 0, 9, "down"},
    {HidNpadButton_Left, 0x400, 10, "left"},
    {HidNpadButton_Right, 0x800, 11, "right"},
    {HidNpadButton_L, 0x40, 16, "L"},
    {HidNpadButton_ZL, 0, 17, "zl"},
    {HidNpadButton_R, 0x80, 18, "R"},
    {HidNpadButton_ZR, 0, 19, "zr"},
};

static float jni_gamepad_deadzone(float val)
{
  if (fabsf(val) < 0.2f)
    return 0.0f;
  return val;
}

static void jni_gamepad_update(void)
{
  if (!g_jni_pad_initialized)
  {
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_jni_pad);
    g_jni_pad_initialized = 1;
    debugPrintf("jni_gamepad: initialized\n");
  }

  padUpdate(&g_jni_pad);
}

static void jni_gamepad_sample(int *connected_out, u64 *buttons_out, float axes_out[6])
{
  const float scale = 1.f / (float)0x7fff;
  int i;

  jni_gamepad_update();

  if (connected_out)
    *connected_out = padIsConnected(&g_jni_pad);

  if (!padIsConnected(&g_jni_pad))
  {
    if (buttons_out)
      *buttons_out = 0;
    if (axes_out)
    {
      for (i = 0; i < 6; i++)
        axes_out[i] = 0.0f;
    }
    return;
  }

  u64 buttons = padGetButtons(&g_jni_pad);
  HidAnalogStickState left = padGetStickPos(&g_jni_pad, 0);
  HidAnalogStickState right = padGetStickPos(&g_jni_pad, 1);

  if (buttons_out)
    *buttons_out = buttons;

  if (axes_out)
  {
    axes_out[0] = jni_gamepad_deadzone((float)left.x * scale);
    axes_out[1] = jni_gamepad_deadzone((float)left.y * -scale);
    axes_out[2] = jni_gamepad_deadzone((float)right.x * scale);
    axes_out[3] = jni_gamepad_deadzone((float)right.y * -scale);
    axes_out[4] = (buttons & HidNpadButton_ZL) ? 1.0f : 0.0f;
    axes_out[5] = (buttons & HidNpadButton_ZR) ? 1.0f : 0.0f;
  }
}

void jni_gamepad_get_state(int *connected_out, u64 *buttons_out, float axes_out[6])
{
  jni_gamepad_sample(connected_out, buttons_out, axes_out);
}

static int jni_gamepad_button_mask(u64 buttons)
{
  int mask = 0;
  unsigned i;

  for (i = 0; i < sizeof(g_jni_gamepad_buttons) / sizeof(g_jni_gamepad_buttons[0]); i++)
  {
    if (g_jni_gamepad_buttons[i].mask_bit && (buttons & g_jni_gamepad_buttons[i].nx_mask))
      mask |= g_jni_gamepad_buttons[i].mask_bit;
  }

  return mask;
}

static void sdl_gamepad_log(const char *fmt, ...)
{
  char buf[256];
  va_list ap;

  if (g_jni_gamepad_log_count >= 24)
    return;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  debugPrintf("sdl_gamepad: %s\n", buf);
  g_jni_gamepad_log_count++;
}

static int sdl_gamepad_open_first(void)
{
  int i;
  int num;

  num = SDL_NumJoysticks();
  for (i = 0; i < num; i++)
  {
    if (!SDL_IsGameController(i))
      continue;

    g_sdl_gamepad = SDL_GameControllerOpen(i);
    if (g_sdl_gamepad)
    {
      sdl_gamepad_log("opened index=%d name=%s", i,
                      SDL_GameControllerName(g_sdl_gamepad));
      return 1;
    }
  }

  sdl_gamepad_log("no controller opened (num=%d err=%s)", num, SDL_GetError());
  return 0;
}

static int sdl_gamepad_init(void)
{
  if (g_sdl_gamepad_initialized)
    return 1;
  if (g_sdl_gamepad_failed)
    return 0;

  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0)
  {
    debugPrintf("sdl_gamepad: SDL_InitSubSystem failed: %s\n", SDL_GetError());
    g_sdl_gamepad_failed = 1;
    return 0;
  }

  g_sdl_gamepad_initialized = 1;
  sdl_gamepad_log("initialized");
  sdl_gamepad_open_first();
  return 1;
}

static void sdl_gamepad_sample(int *connected_out, int *buttons_out, float axes_out[6])
{
  const float axis_scale = 1.0f / 32767.0f;
  int connected = 0;
  int buttons = 0;

  if (connected_out)
    *connected_out = 0;
  if (buttons_out)
    *buttons_out = 0;
  if (axes_out)
    memset(axes_out, 0, sizeof(float) * 6);

  if (!sdl_gamepad_init())
    return;

  if (g_sdl_gamepad && !SDL_GameControllerGetAttached(g_sdl_gamepad))
  {
    sdl_gamepad_log("detached");
    SDL_GameControllerClose(g_sdl_gamepad);
    g_sdl_gamepad = NULL;
  }

  if (!g_sdl_gamepad)
    sdl_gamepad_open_first();

  SDL_PumpEvents();
  SDL_GameControllerUpdate();

  if (g_sdl_gamepad && SDL_GameControllerGetAttached(g_sdl_gamepad))
  {
    connected = 1;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_A))
      buttons |= 0x1;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_B))
      buttons |= 0x2;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_X))
      buttons |= 0x4;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_Y))
      buttons |= 0x8;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_START))
      buttons |= 0x10;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_BACK))
      buttons |= 0x20;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
      buttons |= 0x40;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
      buttons |= 0x80;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP))
      buttons |= 0x100;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
      buttons |= 0x200;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
      buttons |= 0x400;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
      buttons |= 0x800;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_LEFTSTICK))
      buttons |= 0x1000;
    if (SDL_GameControllerGetButton(g_sdl_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSTICK))
      buttons |= 0x2000;

    if (axes_out)
    {
      axes_out[0] = jni_gamepad_deadzone((float)SDL_GameControllerGetAxis(g_sdl_gamepad, SDL_CONTROLLER_AXIS_LEFTX) * axis_scale);
      axes_out[1] = jni_gamepad_deadzone((float)-SDL_GameControllerGetAxis(g_sdl_gamepad, SDL_CONTROLLER_AXIS_LEFTY) * axis_scale);
      axes_out[2] = jni_gamepad_deadzone((float)SDL_GameControllerGetAxis(g_sdl_gamepad, SDL_CONTROLLER_AXIS_RIGHTX) * axis_scale);
      axes_out[3] = jni_gamepad_deadzone((float)-SDL_GameControllerGetAxis(g_sdl_gamepad, SDL_CONTROLLER_AXIS_RIGHTY) * axis_scale);
      axes_out[4] = (float)SDL_GameControllerGetAxis(g_sdl_gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
      axes_out[5] = (float)SDL_GameControllerGetAxis(g_sdl_gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
      if (axes_out[4] < 0.0f)
        axes_out[4] = 0.0f;
      if (axes_out[5] < 0.0f)
        axes_out[5] = 0.0f;
    }
  }

  if (connected_out)
    *connected_out = connected;
  if (buttons_out)
    *buttons_out = buttons;
}

int GetDeviceInfo(void)
{
  return 0;
}

int GetDeviceType(void)
{
  // 0x1: phone
  // 0x2: tegra
  // low memory is < 256
  return (MEMORY_MB << 6) | (3 << 2) | 0x1;
}

int GetDeviceLocale(void)
{
  return 0; // english
}

int GetGamepadType(int port)
{
  int connected = 0;

  (void)port;
  jni_gamepad_sample(&connected, NULL, NULL);
  if (!connected)
    return 0;
  return 8;
}

int GetGamepadButtons(int port)
{
  int connected = 0;
  u64 buttons = 0;

  (void)port;
  jni_gamepad_sample(&connected, &buttons, NULL);
  if (!connected)
    return 0;
  return jni_gamepad_button_mask(buttons);
}

float GetGamepadAxis(int port, int axis)
{
  int connected = 0;
  float axes[6];

  (void)port;
  jni_gamepad_sample(&connected, NULL, axes);
  if (!connected || axis < 0 || axis >= 6)
    return 0.0f;
  return axes[axis];
}

int swapBuffers(void)
{
  extern void NVEventEGLSwapBuffers(void);
  NVEventEGLSwapBuffers();
  return 1;
}

int InitEGLAndGLES2(void)
{
  // The game calls this via JNI to set up EGL.
  // We already initialized in jni_start(), so just confirm success.
  if (g_egl_display != EGL_NO_DISPLAY)
  {
    debugPrintf("InitEGLAndGLES2: EGL already initialized, returning success\n");
    return 1;
  }
  debugPrintf("InitEGLAndGLES2: initializing EGL for Switch...\n");
  int result = NVEventEGLInit();
  if (!result)
  {
    debugPrintf("InitEGLAndGLES2: EGL initialization FAILED!\n");
    return 0;
  }
  debugPrintf("InitEGLAndGLES2: EGL initialized successfully\n");
  return 1;
}

int makeCurrent(void)
{
  // The game calls this from its render thread to acquire the GL context.
  debugPrintf("makeCurrent: acquiring EGL context on current thread\n");
  EGLBoolean ret = eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context);
  if (ret == EGL_FALSE)
  {
    debugPrintf("makeCurrent: FAILED! error=0x%x\n", eglGetError());
    return 0;
  }
  return 1;
}

int unMakeCurrent(void)
{
  // The game calls this to release the GL context from the current thread,
  // allowing another thread (render thread) to claim it.
  debugPrintf("unMakeCurrent: releasing EGL context from current thread\n");
  eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return 1;
}

int hasAppLocalValue(char *key)
{
  if (strcmp(key, "STORAGE_ROOT") == 0)
    return 1;
  return 0;
}

char *getAppLocalValue(char *key)
{
  if (strcmp(key, "STORAGE_ROOT") == 0)
    return ".";
  return NULL;
}

void setAppLocalValue(char *key, char *value)
{
  debugPrintf("setAppLocalValue: %s = %s\n", key ? key : "(null)", value ? value : "(null)");
}

char *getParameter(char *key)
{
  debugPrintf("getParameter: %s\n", key ? key : "(null)");
  return NULL;
}

char *FileGetArchiveName(int type)
{
  // If we return NULL, the game engine will bypass OBB checks
  // and load files directly from the working directory / STORAGE_ROOT.
  return NULL;
}

#include <unistd.h>
int DeleteFile(char *file)
{
  char path[128];
  snprintf(path, sizeof(path), "%s/%s", ".", file);
  if (unlink(path) < 0)
    return 0;
  return 1;
}

int CallBooleanMethodV(void *env, void *obj, int methodID, va_list args)
{
  switch (methodID)
  {
  case INIT_EGL_AND_GLES2:
    return InitEGLAndGLES2();
  case SWAP_BUFFERS:
    return swapBuffers();
  case MAKE_CURRENT:
    return makeCurrent();
  case UN_MAKE_CURRENT:
    return unMakeCurrent();
  case HAS_APP_LOCAL_VALUE:
    return hasAppLocalValue(va_arg(args, char *));
  case DELETE_FILE:
    return DeleteFile(va_arg(args, char *));
  case ROCKSTAR_IN_TRIAL:
    // No trial mode on Switch; behave as full game.
    return 0;
  default:
    debugPrintf("CallBooleanMethodV: unknown methodID %d\n", methodID);
    break;
  }

  return 0;
}

int CallBooleanMethod(void *env, void *obj, int methodID, ...)
{
  va_list args;
  va_start(args, methodID);
  int ret = CallBooleanMethodV(env, obj, methodID, args);
  va_end(args);
  return ret;
}

float CallFloatMethodV(void *env, void *obj, int methodID, va_list args)
{
  switch (methodID)
  {
  case GET_GAMEPAD_AXIS:
  {
    int port = va_arg(args, int);
    int axis = va_arg(args, int);
    return GetGamepadAxis(port, axis);
  }
  default:
    debugPrintf("CallFloatMethodV: unknown methodID %d\n", methodID);
    break;
  }

  return 0.0f;
}

float CallFloatMethod(void *env, void *obj, int methodID, ...)
{
  va_list args;
  va_start(args, methodID);
  float ret = CallFloatMethodV(env, obj, methodID, args);
  va_end(args);
  return ret;
}

int CallIntMethodV(void *env, void *obj, int methodID, va_list args)
{
  switch (methodID)
  {
  case GET_GAMEPAD_TYPE:
    return GetGamepadType(va_arg(args, int));
  case GET_GAMEPAD_BUTTONS:
    return GetGamepadButtons(va_arg(args, int));
  case GET_DEVICE_INFO:
    return GetDeviceInfo();
  case GET_DEVICE_TYPE:
    return GetDeviceType();
  case GET_DEVICE_LOCALE:
    return GetDeviceLocale();
  default:
    debugPrintf("CallIntMethodV: unknown methodID %d\n", methodID);
    break;
  }

  return 0;
}

int CallIntMethod(void *env, void *obj, int methodID, ...)
{
  va_list args;
  va_start(args, methodID);
  int ret = CallIntMethodV(env, obj, methodID, args);
  va_end(args);
  return ret;
}

void *CallObjectMethodV(void *env, void *obj, int methodID, va_list args)
{
  switch (methodID)
  {
  case GET_APP_LOCAL_VALUE:
    return getAppLocalValue(va_arg(args, char *));
  case GET_PARAMETER:
    return getParameter(va_arg(args, char *));
  case FILE_GET_ARCHIVE_NAME:
    return FileGetArchiveName(va_arg(args, int));
  }

  return NULL;
}

void *CallObjectMethod(void *env, void *obj, int methodID, ...)
{
  va_list args;
  va_start(args, methodID);
  void *ret = CallObjectMethodV(env, obj, methodID, args);
  va_end(args);
  return ret;
}

void CallVoidMethodV(void *env, void *obj, int methodID, va_list args)
{
  switch (methodID)
  {
  case SET_APP_LOCAL_VALUE:
  {
    char *key = va_arg(args, char *);
    char *value = va_arg(args, char *);
    setAppLocalValue(key, value);
    return;
  }
  case ROCKSTAR_SHOW_GATE:
  {
    int type = va_arg(args, int);
    g_rockstar_pending_gate_type = type;
    g_rockstar_pending_gate = 1;
    debugPrintf("JNI rockstarShowGate(type=%d)\n", type);
    return;
  }
  case ROCKSTAR_SHOW_INITIAL:
    g_rockstar_pending_initial = 1;
    debugPrintf("JNI rockstarShowInitial()\n");
    return;
  case ROCKSTAR_SIGN_IN:
    debugPrintf("JNI rockstarSignIn()\n");
    g_rockstar_pending_signin = 1;
    return;
  case ROCKSTAR_SIGN_OUT:
    debugPrintf("JNI rockstarSignOut()\n");
    g_rockstar_pending_signin = 0;
    return;
  case ROCKSTAR_DELETE_ACCOUNT:
    debugPrintf("JNI rockstarDeleteAccount()\n");
    return;
  case ROCKSTAR_SET_LOCALE_PRIORITY:
  {
    char *locale = va_arg(args, char *);
    debugPrintf("JNI rockstarSetLocalePriority(%s)\n", locale ? locale : "(null)");
    return;
  }
  case ROCKSTAR_SHOW_CLOUD_DISABLED:
    debugPrintf("JNI rockstarShowCloudDisabled()\n");
    return;
  case ROCKSTAR_REQUEST_REVIEW:
    debugPrintf("JNI rockstarRequestReview()\n");
    return;
  case ROCKSTAR_LOG_ERROR:
  {
    char *msg = va_arg(args, char *);
    debugPrintf("JNI rockstarLogError(%s)\n", msg ? msg : "(null)");
    return;
  }
  case ROCKSTAR_LOG_BREADCRUMB:
  {
    char *cat = va_arg(args, char *);
    char *msg = va_arg(args, char *);
    (void)va_arg(args, void *); // java.util.Map (ignored)
    debugPrintf("JNI rockstarLogBreadcrumb(%s, %s)\n",
                cat ? cat : "(null)", msg ? msg : "(null)");
    return;
  }
  case HTTP_HEAD:
  {
    int request = va_arg(args, int);
    char *url = va_arg(args, char *);
    (void)va_arg(args, void *); // header names
    (void)va_arg(args, void *); // header values
    debugPrintf("JNI httpHead(id=%d, url=%s)\n", request, url ? url : "(null)");
    return;
  }
  case HTTP_GET:
  {
    int request = va_arg(args, int);
    char *url = va_arg(args, char *);
    (void)va_arg(args, void *); // header names
    (void)va_arg(args, void *); // header values
    debugPrintf("JNI httpGet(id=%d, url=%s)\n", request, url ? url : "(null)");
    return;
  }
  case HTTP_POST:
  {
    int request = va_arg(args, int);
    char *url = va_arg(args, char *);
    (void)va_arg(args, void *); // header names
    (void)va_arg(args, void *); // header values
    (void)va_arg(args, void *); // body bytes
    debugPrintf("JNI httpPost(id=%d, url=%s)\n", request, url ? url : "(null)");
    return;
  }
  case HTTP_CANCEL:
  {
    int request = va_arg(args, int);
    debugPrintf("JNI httpCancel(id=%d)\n", request);
    return;
  }
  case QUIT:
    debugPrintf("JNI quit()\n");
    return;
  default:
    debugPrintf("CallVoidMethodV: unknown methodID %d\n", methodID);
    return;
  }
}

void CallVoidMethod(void *env, void *obj, int methodID, ...)
{
  va_list args;
  va_start(args, methodID);
  CallVoidMethodV(env, obj, methodID, args);
  va_end(args);
}

int GetMethodID(void *env, void *class, const char *name, const char *sig)
{
  debugPrintf("GetMethodID: %s %s\n", name, sig);

  for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++)
  {
    if (strcmp(name, name_to_method_ids[i].name) == 0)
    {
      // debugPrintf("Return ID: %d\n", name_to_method_ids[i].id);
      return name_to_method_ids[i].id;
    }
  }

  debugPrintf("GetMethodID: unresolved -> UNKNOWN (%s %s)\n", name, sig);
  return UNKNOWN;
}

void RegisterNatives(void *env, int r1, void *r2)
{
  natives = r2;
}

void *FindClass(void *env, const char *name)
{
  debugPrintf("FindClass: %s\n", name ? name : "(null)");
  return (void *)0x41414141;
}

void *NewGlobalRef(void)
{
  return (void *)0x42424242;
}

char *NewStringUTF(void *env, char *bytes)
{
  return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy)
{
  return string;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig)
{
  debugPrintf("GetStaticMethodID: %s %s\n", name, sig);
  return 0;
}

void *NVThreadGetCurrentJNIEnv(void)
{
  return fake_env;
}

int GetEnv(void *vm, void **env, int r2)
{
  // Fill ALL entries with ret0 pointers so any unimplemented JNI call safely returns 0
  // instead of crashing on garbage 'A' fill.
  for (int i = 0; i < (int)(sizeof(fake_env) / sizeof(uintptr_t)); i++)
    ((uintptr_t *)fake_env)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
  // 64-bit JNI offsets: each function pointer is 8 bytes, so offset = index * 8
  *(uintptr_t *)(fake_env + 0x30) = (uintptr_t)FindClass;           // index 6
  *(uintptr_t *)(fake_env + 0x88) = (uintptr_t)ret0;                // ExceptionClear (index 17)
  *(uintptr_t *)(fake_env + 0xA8) = (uintptr_t)NewGlobalRef;        // NewGlobalRef (index 21)
  *(uintptr_t *)(fake_env + 0xB8) = (uintptr_t)ret0;                // DeleteLocalRef (index 23)
  *(uintptr_t *)(fake_env + 0x108) = (uintptr_t)GetMethodID;        // GetMethodID (index 33)
  *(uintptr_t *)(fake_env + 0x110) = (uintptr_t)CallObjectMethod;   // CallObjectMethod (index 34)
  *(uintptr_t *)(fake_env + 0x118) = (uintptr_t)CallObjectMethodV;  // CallObjectMethodV (index 35)
  *(uintptr_t *)(fake_env + 0x128) = (uintptr_t)CallBooleanMethod;  // CallBooleanMethod (index 37)
  *(uintptr_t *)(fake_env + 0x130) = (uintptr_t)CallBooleanMethodV; // CallBooleanMethodV (index 38)
  *(uintptr_t *)(fake_env + 0x188) = (uintptr_t)CallIntMethod;      // CallIntMethod (index 49)
  *(uintptr_t *)(fake_env + 0x190) = (uintptr_t)CallIntMethodV;     // CallIntMethodV (index 50)
  *(uintptr_t *)(fake_env + 0x1B8) = (uintptr_t)CallFloatMethod;    // CallFloatMethod (index 55)
  *(uintptr_t *)(fake_env + 0x1C0) = (uintptr_t)CallFloatMethodV;   // CallFloatMethodV (index 56)
  *(uintptr_t *)(fake_env + 0x1E8) = (uintptr_t)CallVoidMethod;     // CallVoidMethod (index 61)
  *(uintptr_t *)(fake_env + 0x1F0) = (uintptr_t)CallVoidMethodV;    // CallVoidMethodV (index 62)
  *(uintptr_t *)(fake_env + 0x2F0) = (uintptr_t)ret0;               // GetFieldID (index 94)
  *(uintptr_t *)(fake_env + 0x388) = (uintptr_t)GetStaticMethodID;  // GetStaticMethodID (index 113)
  *(uintptr_t *)(fake_env + 0x398) = (uintptr_t)ret0;               // CallStaticObjectMethodV (index 116)
  *(uintptr_t *)(fake_env + 0x480) = (uintptr_t)ret0;               // keyboard stuff
  *(uintptr_t *)(fake_env + 0x538) = (uintptr_t)NewStringUTF;       // NewStringUTF (index 167)
  *(uintptr_t *)(fake_env + 0x548) = (uintptr_t)GetStringUTFChars;  // GetStringUTFChars (index 169)
  *(uintptr_t *)(fake_env + 0x550) = (uintptr_t)ret0;               // ReleaseStringUTFChars (index 170)
  *(uintptr_t *)(fake_env + 0x6B8) = (uintptr_t)RegisterNatives;    // RegisterNatives (index 215)
  *env = fake_env;
  return 0;
}

// Saved function pointers - resolved before so_finalize, called after
static int (*saved_JNI_OnLoad)(void *vm, void *reserved) = NULL;
static void (*saved_implOnInitialSetup)(void *env, void *clazz, void *deviceInfo, void *assetManager, void *paths1, void *paths2) = NULL;
static void (*saved_implOnActivityCreated)(void *env, void *clazz, void *services, int isReady) = NULL;
static void (*saved_implOnSurfaceCreated)(void *env, void *clazz) = NULL;
static void (*saved_implOnSurfaceChanged)(void *env, void *clazz, void *surface, int width, int height) = NULL;
static void (*saved_implOnDrawFrame)(void *env, void *clazz, float delta) = NULL;
static void (*saved_implOnResume)(void *env, void *clazz) = NULL;
static void (*saved_implOnGamepadButtonDown)(void *env, void *clazz, int pad, int button) = NULL;
static void (*saved_implOnGamepadButtonUp)(void *env, void *clazz, int pad, int button) = NULL;
static void (*saved_implOnGamepadAxesChanged)(void *env, void *clazz, int pad, float lx, float ly, float rx, float ry, float l2, float r2) = NULL;
static void (*saved_implOnGamepadCountChanged)(void *env, void *clazz, int count) = NULL;
static void (*saved_implOnRockstarGateComplete)(void *env, void *clazz, int type, int success) = NULL;
static void (*saved_implOnRockstarSetup)(void *env, void *clazz, void *email, void *ticket) = NULL;
static void (*saved_implOnRockstarIdChanged)(void *env, void *clazz, void *id) = NULL;
static void (*saved_implOnRockstarTicketChanged)(void *env, void *clazz, void *ticket) = NULL;
static void (*saved_implOnRockstarInitialComplete)(void *env, void *clazz) = NULL;
static void (*saved_implOnRockstarStateChanged)(void *env, void *clazz, int paused) = NULL;
static void (*saved_implOnRockstarSignInComplete)(void *env, void *clazz) = NULL;
static void (*saved_OS_OnRockstarInitialComplete)(void) = NULL;
static void (*saved_OS_OnRockstarGateComplete)(int type, int success) = NULL;
static void (*saved_OS_ApplicationEvent)(int eventType, void *data) = NULL;
static void (*saved_OS_OnRockstarStateChanged)(int paused) = NULL;
static void (*saved_OS_OnRockstarSignInComplete)(void) = NULL;
static void (*saved_AND_FileUpdated)(double delta) = NULL;

// Addresses of critical gate flags in the data segment (resolved before so_finalize)
static volatile uint8_t *saved_implIsInitialized_flag = NULL;  // 0x158aad4
static volatile uint8_t *saved_OS_IsGameSuspended_flag = NULL; // 0x158aacc
static volatile uint8_t *saved_OS_CanGameRender_flag = NULL;   // 0x158a960
static volatile uintptr_t *saved_OS_EGLDisplay_ptr = NULL;      // 0x158a978
static volatile uintptr_t *saved_OS_EGLSurface_ptr = NULL;      // 0x158a980
static volatile uintptr_t *saved_OS_EGLContext_ptr = NULL;      // 0x158a988
static volatile uintptr_t *g_async_first_file_ptr = NULL;
static int g_async_file_thread_started = 0;
static pthread_t g_async_file_thread;
static pthread_mutex_t g_async_update_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_async_update_cond = PTHREAD_COND_INITIALIZER;
static uint64_t g_async_last_pump_ns = 0;

int jni_async_file_pending(void)
{
  return (saved_AND_FileUpdated && g_async_first_file_ptr &&
          __atomic_load_n(g_async_first_file_ptr, __ATOMIC_ACQUIRE) != 0);
}

int jni_async_file_pump(double delta_hint)
{
  uint64_t now_ns;
  double delta;

  if (!jni_async_file_pending())
    return 0;

  pthread_mutex_lock(&g_async_update_lock);

  if (!saved_AND_FileUpdated || !g_async_first_file_ptr ||
      !__atomic_load_n(g_async_first_file_ptr, __ATOMIC_ACQUIRE))
  {
    pthread_mutex_unlock(&g_async_update_lock);
    return 0;
  }

  now_ns = armGetSystemTick() * 1000000000ULL / armGetSystemTickFreq();
  delta = delta_hint;
  if (g_async_last_pump_ns == 0)
    g_async_last_pump_ns = now_ns;
  if (delta <= 0.0 || delta > 0.1)
    delta = (double)(now_ns - g_async_last_pump_ns) / 1000000000.0;
  if (delta <= 0.0 || delta > 0.1)
    delta = 0.002;
  g_async_last_pump_ns = now_ns;

  saved_AND_FileUpdated(delta);
  pthread_mutex_unlock(&g_async_update_lock);
  return 1;
}

void jni_async_file_kick(void)
{
  pthread_mutex_lock(&g_async_update_lock);
  pthread_cond_signal(&g_async_update_cond);
  pthread_mutex_unlock(&g_async_update_lock);
}

static void *async_file_worker_main(void *arg)
{
  (void)arg;

  while (1)
  {
    while (jni_async_file_pending())
      jni_async_file_pump(0.0);

    pthread_mutex_lock(&g_async_update_lock);
    if (!jni_async_file_pending())
    {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 2000000L; // 2 ms fallback wake in case we miss a signal
      if (ts.tv_nsec >= 1000000000L)
      {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
      }
      pthread_cond_timedwait(&g_async_update_cond, &g_async_update_lock, &ts);
    }
    pthread_mutex_unlock(&g_async_update_lock);
  }

  return NULL;
}

static void jni_gamepad_log(const char *fmt, ...)
{
  char buf[256];
  va_list ap;

  if (g_jni_gamepad_log_count >= 24)
    return;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  debugPrintf("gamepad: %s\n", buf);
  g_jni_gamepad_log_count++;
}

typedef struct
{
  u64 nx_mask;
  int lib_index;
  const char *name;
} JniLibGamepadStateMap;

static const JniLibGamepadStateMap g_jni_lib_gamepad_buttons[] = {
    {HidNpadButton_L, 6, "l"},
    {HidNpadButton_R, 7, "r"},
};

static void jni_gamepad_sync_lib_state(int connected, u64 buttons)
{
  static u64 prev_buttons = 0;
  unsigned i;

  if (!g_jni_lib_gamepad_state)
    return;

  for (i = 0; i < sizeof(g_jni_lib_gamepad_buttons) / sizeof(g_jni_lib_gamepad_buttons[0]); i++)
  {
    u64 bit = g_jni_lib_gamepad_buttons[i].nx_mask;
    int value = 0;

    if (connected && (buttons & bit))
      value = (prev_buttons & bit) ? 3 : 2;

    g_jni_lib_gamepad_state[g_jni_lib_gamepad_buttons[i].lib_index] = value;
  }

  g_jni_lib_gamepad_state[12] = 0;
  g_jni_lib_gamepad_state[13] = 0;

  prev_buttons = connected ? buttons : 0;
}

static void jni_gamepad_dispatch(int gs)
{
  int connected = 0;
  u64 buttons = 0;
  float axes[6];
  unsigned i;
  int axes_changed = !g_jni_pad_axes_valid;

  (void)gs;
  jni_gamepad_sample(&connected, &buttons, axes);
  jni_gamepad_sync_lib_state(connected, buttons);

  if (g_jni_pad_connected != connected)
  {
    if (saved_implOnGamepadCountChanged)
      saved_implOnGamepadCountChanged(fake_env, NULL, connected ? 1 : 0);
    jni_gamepad_log("count=%d connected=%d", connected ? 1 : 0, connected);
    g_jni_pad_connected = connected;
  }

  if (!connected)
  {
    if (g_jni_pad_axes_valid)
      memset(g_jni_pad_axes, 0, sizeof(g_jni_pad_axes));
    g_jni_pad_axes_valid = 0;
    g_jni_pad_buttons = 0;
    return;
  }

  for (i = 0; i < 6; i++)
  {
    if (fabsf(axes[i] - g_jni_pad_axes[i]) > 0.01f)
    {
      axes_changed = 1;
      break;
    }
  }

  if (axes_changed && saved_implOnGamepadAxesChanged)
  {
    saved_implOnGamepadAxesChanged(fake_env, NULL, 0,
                                   axes[0], axes[1], axes[2], axes[3],
                                   axes[4], axes[5]);
    memcpy(g_jni_pad_axes, axes, sizeof(g_jni_pad_axes));
    g_jni_pad_axes_valid = 1;
    if (fabsf(axes[0]) > 0.0f || fabsf(axes[1]) > 0.0f ||
        fabsf(axes[2]) > 0.0f || fabsf(axes[3]) > 0.0f ||
        axes[4] > 0.0f || axes[5] > 0.0f)
    {
      jni_gamepad_log("axes lx=%.2f ly=%.2f rx=%.2f ry=%.2f l2=%.2f r2=%.2f",
                      axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
    }
  }

  if (buttons != g_jni_pad_buttons)
  {
    u64 changed = buttons ^ g_jni_pad_buttons;

    for (i = 0; i < sizeof(g_jni_gamepad_buttons) / sizeof(g_jni_gamepad_buttons[0]); i++)
    {
      u64 bit = g_jni_gamepad_buttons[i].nx_mask;
      if (!bit || !(changed & bit))
        continue;

      if (buttons & bit)
      {
        if (saved_implOnGamepadButtonDown)
          saved_implOnGamepadButtonDown(fake_env, NULL, 0,
                                        g_jni_gamepad_buttons[i].game_button);
        jni_gamepad_log("down %s -> %d", g_jni_gamepad_buttons[i].name,
                        g_jni_gamepad_buttons[i].game_button);
      }
      else
      {
        if (saved_implOnGamepadButtonUp)
          saved_implOnGamepadButtonUp(fake_env, NULL, 0,
                                      g_jni_gamepad_buttons[i].game_button);
        jni_gamepad_log("up %s -> %d", g_jni_gamepad_buttons[i].name,
                        g_jni_gamepad_buttons[i].game_button);
      }
    }

    g_jni_pad_buttons = buttons;
  }
}

static void sync_engine_egl_globals(const char *tag)
{
  if (!saved_OS_EGLDisplay_ptr || !saved_OS_EGLSurface_ptr || !saved_OS_EGLContext_ptr)
    return;

  uintptr_t want_display = (uintptr_t)g_egl_display;
  uintptr_t want_surface = (uintptr_t)g_egl_surface;
  uintptr_t want_context = (uintptr_t)g_egl_context;

  uintptr_t old_display = *saved_OS_EGLDisplay_ptr;
  uintptr_t old_surface = *saved_OS_EGLSurface_ptr;
  uintptr_t old_context = *saved_OS_EGLContext_ptr;

  *saved_OS_EGLDisplay_ptr = want_display;
  *saved_OS_EGLSurface_ptr = want_surface;
  *saved_OS_EGLContext_ptr = want_context;

  if (old_display != want_display || old_surface != want_surface || old_context != want_context)
  {
    debugPrintf("sync_engine_egl_globals(%s): dpy %p->%p surf %p->%p ctx %p->%p\n",
                tag ? tag : "?", (void *)old_display, (void *)want_display,
                (void *)old_surface, (void *)want_surface,
                (void *)old_context, (void *)want_context);
  }
}

// Called BEFORE so_finalize() - resolves all symbol addresses while text_base is still readable
void jni_init(void)
{
  extern void *text_virtbase;

  // Fill with ret0 pointers for safety (any unhandled JVM call returns 0)
  for (int i = 0; i < (int)(sizeof(fake_vm) / sizeof(uintptr_t)); i++)
    ((uintptr_t *)fake_vm)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
  // 64-bit JNIInvokeInterface: GetEnv is at index 6, offset = 6 * 8 = 0x30
  *(uintptr_t *)(fake_vm + 0x30) = (uintptr_t)GetEnv;

  // Bully uses standard JNI naming (Java_com_rockstargames_*), NOT RegisterNatives.
  // Resolve all native function pointers using so_find_addr_rx (text_virtbase addresses).
  saved_JNI_OnLoad = (void *)so_find_addr_rx("JNI_OnLoad");
  saved_implOnInitialSetup = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnInitialSetup");
  saved_implOnActivityCreated = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnActivityCreated");
  saved_implOnSurfaceCreated = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnSurfaceCreated");
  saved_implOnSurfaceChanged = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnSurfaceChanged");
  saved_implOnDrawFrame = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnDrawFrame");
  saved_implOnResume = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnResume");
  saved_implOnGamepadButtonDown = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnGamepadButtonDown");
  saved_implOnGamepadButtonUp = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnGamepadButtonUp");
  saved_implOnGamepadAxesChanged = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnGamepadAxesChanged");
  saved_implOnGamepadCountChanged = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnGamepadCountChanged");
  saved_implOnRockstarGateComplete = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnRockstarGateComplete");
  saved_implOnRockstarSetup = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnRockstarSetup");
  saved_implOnRockstarIdChanged = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnRockstarIdChanged");
  saved_implOnRockstarTicketChanged = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnRockstarTicketChanged");
  saved_implOnRockstarInitialComplete = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnRockstarInitialComplete");
  saved_implOnRockstarStateChanged = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnRockstarStateChanged");
  saved_implOnRockstarSignInComplete = (void *)so_find_addr_rx("Java_com_rockstargames_oswrapper_GameNative_implOnRockstarSignInComplete");
  saved_OS_OnRockstarInitialComplete = (void *)so_find_addr_rx("_Z28OS_OnRockstarInitialCompletev");
  saved_OS_OnRockstarGateComplete = (void *)so_find_addr_rx("_Z25OS_OnRockstarGateCompleteib");
  saved_OS_ApplicationEvent = (void *)so_find_addr_rx("_Z19OS_ApplicationEvent11OSEventTypePv");
  saved_OS_OnRockstarStateChanged = (void *)so_find_addr_rx("_Z25OS_OnRockstarStateChangedb");
  saved_OS_OnRockstarSignInComplete = (void *)so_find_addr_rx("_Z27OS_OnRockstarSignInCompletev");
  saved_AND_FileUpdated = (void *)so_find_addr_rx("_Z14AND_FileUpdated");

  // Resolve all critical gate flag addresses.
  // These are relative to StorageRootPath (ELF vaddr 0x158ac48).
  uintptr_t srp = so_find_addr_rx("StorageRootPath");
  saved_implIsInitialized_flag = (volatile uint8_t *)(srp - 0x174);  // 0x158aad4
  saved_OS_IsGameSuspended_flag = (volatile uint8_t *)(srp - 0x17c); // 0x158aacc
  saved_OS_CanGameRender_flag = (volatile uint8_t *)(srp - 0x2e8);   // 0x158a960
  saved_OS_EGLDisplay_ptr = (volatile uintptr_t *)(srp - 0x2d0);     // 0x158a978
  saved_OS_EGLSurface_ptr = (volatile uintptr_t *)(srp - 0x2c8);     // 0x158a980
  saved_OS_EGLContext_ptr = (volatile uintptr_t *)(srp - 0x2c0);     // 0x158a988
  g_jni_gamepad_slots = (volatile uint8_t *)(srp - 0x2a0);           // 0x158a9a8
  g_jni_lib_gamepad_state = (volatile int *)so_find_addr_rx("gamepads");
  g_async_first_file_ptr = (volatile uintptr_t *)((uintptr_t)text_virtbase + 0x158a9a0);

  debugPrintf("jni_init: srp=%p implIsInit=%p suspended=%p canRender=%p egl(d/s/c)=%p/%p/%p\n",
              (void *)srp, saved_implIsInitialized_flag,
              saved_OS_IsGameSuspended_flag, saved_OS_CanGameRender_flag,
              saved_OS_EGLDisplay_ptr, saved_OS_EGLSurface_ptr, saved_OS_EGLContext_ptr);
  debugPrintf("jni_init: gamepad cb down=%p up=%p axes=%p count=%p\n",
              saved_implOnGamepadButtonDown, saved_implOnGamepadButtonUp,
              saved_implOnGamepadAxesChanged, saved_implOnGamepadCountChanged);
  debugPrintf("jni_init: gamepad cache slots=%p lib_state=%p\n",
              g_jni_gamepad_slots, g_jni_lib_gamepad_state);
  debugPrintf("jni_init: AND_FileUpdated=%p async_head=%p\n",
              saved_AND_FileUpdated, g_async_first_file_ptr);
}

// Called AFTER so_finalize() - code is now executable at text_virtbase
void jni_start(void)
{
  extern int OS_ScreenGetWidth(void);
  extern int OS_ScreenGetHeight(void);

  debugPrintf("=== BULLY NX BUILD v11 (stack canary fix — disable TLS-based __stack_chk) ===\n");
  debugPrintf_setMainThread();

  // Initialize EGL for Switch BEFORE any game code runs.
  // The game calls EGL functions directly through the import table,
  // so our display/surface/context must be ready before JNI_OnLoad.
  debugPrintf("jni_start: initializing EGL for Switch...\n");
  if (!NVEventEGLInit())
  {
    debugPrintf("jni_start: EGL initialization FAILED!\n");
    return;
  }
  debugPrintf("jni_start: EGL initialized (display=%p, surface=%p, context=%p)\n",
              g_egl_display, g_egl_surface, g_egl_context);
  sync_engine_egl_globals("post-egl-init");

  saved_JNI_OnLoad(fake_vm, NULL);
  debugPrintf("jni_start: JNI_OnLoad done\n");
  g_rockstar_pending_initial = 0;
  g_rockstar_pending_gate = 0;
  g_rockstar_pending_gate_type = 0;
  g_rockstar_pending_signin = 0;

  // Force OS_IsGameSuspended = 0 (not paused) BEFORE calling init.
  // On Android this defaults to 1 (paused) and is cleared by implOnResume.
  if (saved_OS_IsGameSuspended_flag)
  {
    *saved_OS_IsGameSuspended_flag = 0;
    debugPrintf("jni_start: forced OS_IsGameSuspended = 0\n");
  }

  // Initialize all AND_* subsystems — this sets the critical flag at 0x158aad4
  // that implOnDrawFrame checks before processing any frames.
  // On Android this is called from Java: implOnInitialSetup(env, this, activity, apkFd, paths1, paths2)
  // NvAPKInit is hooked so apk/obb params are ignored. Other params can be NULL.
  debugPrintf("jni_start: calling implOnInitialSetup...\n");
  saved_implOnInitialSetup(fake_env, NULL, NULL, NULL, NULL, NULL);
  debugPrintf("jni_start: implOnInitialSetup done\n");

  // Force ALL three gate flags required by implOnDrawFrame:
  // Gate 1: OS_IsGameSuspended (0x158aacc) = 0  (not suspended)
  // Gate 2: OS_CanGameRender (0x158a960) = 1     (can render)
  // Gate 3: implIsInitialized (0x158aad4) = 1    (initialized)
  if (saved_implIsInitialized_flag && *saved_implIsInitialized_flag != 1)
  {
    debugPrintf("jni_start: WARNING - implIsInitialized not set, forcing to 1\n");
    *saved_implIsInitialized_flag = 1;
  }
  if (saved_OS_IsGameSuspended_flag)
  {
    *saved_OS_IsGameSuspended_flag = 0;
    debugPrintf("jni_start: OS_IsGameSuspended = %d\n", *saved_OS_IsGameSuspended_flag);
  }
  if (saved_OS_CanGameRender_flag)
  {
    *saved_OS_CanGameRender_flag = 1;
    debugPrintf("jni_start: OS_CanGameRender forced = %d\n", *saved_OS_CanGameRender_flag);
  }
  debugPrintf("jni_start: gate flags: suspended=%d canRender=%d isInit=%d\n",
              saved_OS_IsGameSuspended_flag ? *saved_OS_IsGameSuspended_flag : -1,
              saved_OS_CanGameRender_flag ? *saved_OS_CanGameRender_flag : -1,
              saved_implIsInitialized_flag ? *saved_implIsInitialized_flag : -1);

  // Initialize the game - Bully's Rockstar framework init sequence
  debugPrintf("jni_start: calling implOnActivityCreated...\n");
  saved_implOnActivityCreated(fake_env, NULL, (void *)0x42424242, 1);
  debugPrintf("jni_start: implOnActivityCreated done\n");

  // Release EGL context from main thread BEFORE surface callbacks.
  // The game's render thread (spawned by implOnSurfaceCreated) needs to
  // acquire the context via makeCurrent JNI call or eglMakeCurrent import.
  // If main thread holds it, the render thread's eglMakeCurrent will fail.
  debugPrintf("jni_start: releasing EGL context from main thread before surface callbacks\n");
  eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  debugPrintf("jni_start: calling implOnSurfaceCreated...\n");
  saved_implOnSurfaceCreated(fake_env, NULL);
  debugPrintf("jni_start: implOnSurfaceCreated done\n");

  int width = OS_ScreenGetWidth();
  int height = OS_ScreenGetHeight();
  debugPrintf("jni_start: calling implOnSurfaceChanged(NULL, %d, %d)...\n", width, height);
  saved_implOnSurfaceChanged(fake_env, NULL, NULL, width, height);
  debugPrintf("jni_start: implOnSurfaceChanged done\n");
  sync_engine_egl_globals("post-surface-changed");

  debugPrintf("jni_start: calling implOnResume...\n");
  saved_implOnResume(fake_env, NULL);
  debugPrintf("jni_start: implOnResume done\n");

  if (!g_async_file_thread_started && saved_AND_FileUpdated && g_async_first_file_ptr)
  {
    if (pthread_create(&g_async_file_thread, NULL, async_file_worker_main, NULL) == 0)
    {
      pthread_detach(g_async_file_thread);
      g_async_file_thread_started = 1;
      debugPrintf("jni_start: async file worker started (AND_FileUpdated=%p head=%p)\n",
                  saved_AND_FileUpdated, g_async_first_file_ptr);
    }
    else
    {
      debugPrintf("jni_start: async file worker start FAILED\n");
    }
  }

  // NOTE: Rockstar gate completion is now DEFERRED to the frame loop.
  // On Android, these callbacks fire asynchronously AFTER GameMain finishes Startup.
  // Firing them here (before GameMain even launches) causes them to be ignored.

  // Main game loop — simulate Android vsync cadence.
  // On Android, implOnDrawFrame is called by GLSurfaceView at ~60Hz.
  // Without a sleep, the main thread busy-spins and starves the GameMain
  // thread (which does all the actual rendering and game logic).
  extern volatile int g_gamemain_alive;
  extern int diag_get_cond_wait_count(void);
  extern int diag_get_cond_signal_count(void);
  extern int diag_get_sem_wait_count(void);
  extern int diag_get_sem_post_count(void);
  extern int diag_get_syscall_count(void);
  extern int diag_get_swap_count(void);

  // Monitor AndroidFile::firstAsyncFile to see if the async file I/O queue has work
  // Address 0x158a9a0 in the binary's data section
  extern void *text_virtbase;
  volatile uintptr_t *firstAsyncFile_ptr = (volatile uintptr_t *)((uintptr_t)text_virtbase + 0x158a9a0);

  // BullyApplication game state at this->0x68
  // Global pointer to GameRenderer at offset 0x12146b0 (from appRender disasm)
  // Global pointer to Application at offset 0x12146f0 (from Application::Tick disasm)
  // Game states: 0=init, 1=loading?, 2=running, 4=special
  volatile uint32_t *app_gamestate_ptr = NULL;

  // GameMain tick gate flags — both must be 1 for Application::Tick to be called
  // Located at 0x126bb70 (notSuspended?) and 0x126bb74 (canRender?)
  volatile uint8_t *tick_flag_a = (volatile uint8_t *)((uintptr_t)text_virtbase + 0x126bb70);
  volatile uint8_t *tick_flag_b = (volatile uint8_t *)((uintptr_t)text_virtbase + 0x126bb74);

  const uint64_t target_frame_ns = 1000000000ULL / 60ULL;
  int frame = 0;
  int rockstar_gate_fired = 0;
  int forced_gamestate = 0;
  int audio_runtime_enabled = 0;
  int compat_auto = (config.timing_workaround_ms <= 0);
  int compat_delay_ms = compat_auto ? 0 : config.timing_workaround_ms;
  int compat_max_delay_ms = 4;
  int last_swap_count = 0;
  uint64_t last_swap_progress_ns = 0;
  uint64_t last_frame_ns = 0;
  // Use armGetSystemTick for timing: ticks at 19.2 MHz on Switch
  last_frame_ns = armGetSystemTick() * 1000000000ULL / armGetSystemTickFreq();
  last_swap_count = diag_get_swap_count();
  last_swap_progress_ns = last_frame_ns;

  while (1)
  {
    uintptr_t asyncHead = *firstAsyncFile_ptr;

    // Try to read game state from BullyApplication object
    // The Application* holder is at global 0x12146a8 (confirmed from GameMain + CreateApplication)
    // Chain: *(*(0x12146a8)) + 0x68 = gameState
    if (!app_gamestate_ptr)
    {
      volatile uintptr_t *app_ptr_ptr = (volatile uintptr_t *)((uintptr_t)text_virtbase + 0x12146a8);
      uintptr_t app_obj = *app_ptr_ptr;
      if (app_obj)
      {
        // Dereference once more — the global holds a pointer to a pointer
        uintptr_t app_real = *(volatile uintptr_t *)app_obj;
        if (app_real)
          app_gamestate_ptr = (volatile uint32_t *)(app_real + 0x68);
      }
    }
    int gs = app_gamestate_ptr ? (int)*app_gamestate_ptr : -1;
    if (!forced_gamestate && g_gamemain_alive == 1 && app_gamestate_ptr && gs == 0 && frame > 240)
    {
      *app_gamestate_ptr = 2;
      gs = 2;
      forced_gamestate = 1;
      debugPrintf("FORCE GameState 0->2 (frame=%d)\n", frame);
    }

    // Compute delta time in seconds (like Android's ExecutorThread.guardedRun)
    uint64_t now_ns = armGetSystemTick() * 1000000000ULL / armGetSystemTickFreq();
    float delta = (float)(now_ns - last_frame_ns) / 1000000000.0f;
    if (delta <= 0.0f || delta > 1.0f)
      delta = 0.016f; // clamp to sane range
    last_frame_ns = now_ns;
    int swap_count = diag_get_swap_count();
    if (swap_count != last_swap_count)
    {
      last_swap_count = swap_count;
      last_swap_progress_ns = now_ns;
    }

    if (frame < 10 || (frame % 300 == 0))
      debugPrintf("frame %d gm=%d gs=%d tA=%d tB=%d swp=%d cw=%d cs=%d sem=%d/%d sc=%d async=%p dt=%.3f\n",
                  frame, g_gamemain_alive, gs,
                  (int)*tick_flag_a, (int)*tick_flag_b,
                  swap_count,
                  diag_get_cond_wait_count(), diag_get_cond_signal_count(),
                  diag_get_sem_wait_count(), diag_get_sem_post_count(),
                  diag_get_syscall_count(),
                  (void *)asyncHead, delta);

    if (compat_auto &&
        compat_delay_ms < compat_max_delay_ms &&
        frame > 120 &&
        g_gamemain_alive == 1 &&
        gs >= 0 &&
        (now_ns - last_swap_progress_ns) > 1000000000ULL)
    {
      compat_delay_ms++;
      debugPrintf_setCompatDelayMs(compat_delay_ms);
      last_swap_progress_ns = now_ns;
      debugPrintf("compat auto: swap stall detected (frame=%d gs=%d async=%p swp=%d) -> %d ms\n",
                  frame, gs, (void *)asyncHead, swap_count, compat_delay_ms);
    }

    if (!audio_runtime_enabled &&
        frame > 240 &&
        g_gamemain_alive == 1 &&
        gs >= 2 &&
        saved_implIsInitialized_flag &&
        *saved_implIsInitialized_flag)
    {
      openal_set_runtime_enabled(1);
      audio_runtime_enabled = 1;
      debugPrintf("openal: enabling playback at frame=%d gs=%d async=%p\n",
                  frame, gs, (void *)asyncHead);
    }

    // Rockstar callback bridge: the native game asks Java to show social/login UI.
    // On Switch we don't have that Java UI, so when those JNI methods are requested,
    // deliver the expected native completion callbacks asynchronously from here.
    if (!rockstar_gate_fired && (g_rockstar_pending_initial || g_rockstar_pending_gate) && frame > 30)
    {
      rockstar_gate_fired = 1;
      int gate_type = g_rockstar_pending_gate ? g_rockstar_pending_gate_type : 0;
      debugPrintf("=== ROCKSTAR JNI COMPLETE (frame=%d type=%d) ===\n", frame, gate_type);

      if (saved_implOnRockstarStateChanged)
      {
        saved_implOnRockstarStateChanged(fake_env, NULL, 0); // not paused
        debugPrintf("implOnRockstarStateChanged(0) done\n");
      }
      else if (saved_OS_OnRockstarStateChanged)
      {
        saved_OS_OnRockstarStateChanged(0); // not paused
        debugPrintf("OS_OnRockstarStateChanged(0) done\n");
      }
      if (saved_implOnRockstarInitialComplete)
      {
        saved_implOnRockstarInitialComplete(fake_env, NULL);
        debugPrintf("implOnRockstarInitialComplete done\n");
      }
      else if (saved_OS_OnRockstarInitialComplete)
      {
        saved_OS_OnRockstarInitialComplete();
        debugPrintf("OS_OnRockstarInitialComplete done\n");
      }
      if (saved_implOnRockstarGateComplete)
      {
        saved_implOnRockstarGateComplete(fake_env, NULL, gate_type, 1); // success=true
        debugPrintf("implOnRockstarGateComplete(%d,1) done\n", gate_type);
      }
      else if (saved_OS_OnRockstarGateComplete)
      {
        saved_OS_OnRockstarGateComplete(gate_type, 1); // success=true
        debugPrintf("OS_OnRockstarGateComplete(%d,1) done\n", gate_type);
      }
      if (saved_OS_ApplicationEvent)
      {
        saved_OS_ApplicationEvent(9, NULL); // OSET_Resume
        debugPrintf("OS_ApplicationEvent(Resume) done\n");
      }

      // Populate Rockstar native identity/ticket state so OS_RockstarSignedIn()
      // sees a non-empty credential and startup can advance.
      if (saved_implOnRockstarSetup)
      {
        saved_implOnRockstarSetup(fake_env, NULL, (void *)"switch_user", (void *)"switch_ticket");
        debugPrintf("implOnRockstarSetup(switch_user,switch_ticket) done\n");
      }
      else
      {
        if (saved_implOnRockstarIdChanged)
        {
          saved_implOnRockstarIdChanged(fake_env, NULL, (void *)"switch_user");
          debugPrintf("implOnRockstarIdChanged(switch_user) done\n");
        }
        if (saved_implOnRockstarTicketChanged)
        {
          saved_implOnRockstarTicketChanged(fake_env, NULL, (void *)"switch_ticket");
          debugPrintf("implOnRockstarTicketChanged(switch_ticket) done\n");
        }
      }

      // Re-force gate flags
      if (saved_OS_CanGameRender_flag)
        *saved_OS_CanGameRender_flag = 1;
      if (saved_OS_IsGameSuspended_flag)
        *saved_OS_IsGameSuspended_flag = 0;
      if (saved_implIsInitialized_flag)
        *saved_implIsInitialized_flag = 1;

      g_rockstar_pending_initial = 0;
      g_rockstar_pending_gate = 0;
      g_rockstar_pending_gate_type = 0;
      g_rockstar_pending_signin = 1; // advance past login UI on platforms without SC UI

      debugPrintf("=== ROCKSTAR JNI COMPLETE DONE ===\n");
    }

    if (g_rockstar_pending_signin && frame > 45)
    {
      g_rockstar_pending_signin = 0;
      if (saved_implOnRockstarSignInComplete)
      {
        saved_implOnRockstarSignInComplete(fake_env, NULL);
        debugPrintf("implOnRockstarSignInComplete done\n");
      }
      else if (saved_OS_OnRockstarSignInComplete)
      {
        saved_OS_OnRockstarSignInComplete();
        debugPrintf("OS_OnRockstarSignInComplete done\n");
      }
    }

    jni_gamepad_dispatch(gs);
    if (g_async_file_thread_started && asyncHead)
      jni_async_file_kick();
    if (!g_async_file_thread_started && asyncHead)
      jni_async_file_pump((double)delta);
    saved_implOnDrawFrame(fake_env, NULL, delta);
    frame++;
    {
      uint64_t frame_end_ns = armGetSystemTick() * 1000000000ULL / armGetSystemTickFreq();
      uint64_t next_frame_ns = last_frame_ns + target_frame_ns;
      if (frame_end_ns < next_frame_ns)
        svcSleepThread((int64_t)(next_frame_ns - frame_end_ns));
    }
  }
}
