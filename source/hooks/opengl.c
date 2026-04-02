/* opengl.c -- OpenGL and shader generator hooks and patches
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include "../config.h"
#include "../util.h"
#include "../so_util.h"

// Global EGL objects - accessible from imports.c for EGL wrapper functions
EGLDisplay g_egl_display = EGL_NO_DISPLAY;
EGLSurface g_egl_surface = EGL_NO_SURFACE;
EGLContext g_egl_context = EGL_NO_CONTEXT;

// Internal aliases for convenience
#define display g_egl_display
#define surface g_egl_surface
#define context g_egl_context

void NVEventEGLSwapBuffers(void)
{
  eglSwapBuffers(display, surface);
}

void NVEventEGLMakeCurrent(void)
{
}

void NVEventEGLUnmakeCurrent(void)
{
}

int NVEventEGLInit(void)
{
  EGLint numConfigs = 0;
  EGLConfig eglConfig;

  const EGLint contextAttribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE};

  const EGLint configAttribs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 24,
      EGL_STENCIL_SIZE, 8,
      EGL_NONE};

  NWindow *win = nwindowGetDefault();

  nwindowSetDimensions(win, screen_width, screen_height);

  display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!display)
  {
    debugPrintf("EGL: Could not connect to display: %08x\n", eglGetError());
    return 0;
  }

  eglInitialize(display, NULL, NULL);

  if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE)
  {
    debugPrintf("EGL: Could not set API: %08x\n", eglGetError());
    return 0;
  }

  eglChooseConfig(display, configAttribs, &eglConfig, 1, &numConfigs);
  if (numConfigs <= 0)
  {
    debugPrintf("EGL: No matching config: %08x\n", eglGetError());
    return 0;
  }

  surface = eglCreateWindowSurface(display, eglConfig, win, NULL);
  if (!surface)
  {
    debugPrintf("EGL: Could not create surface: %08x\n", eglGetError());
    return 0;
  }

  context = eglCreateContext(display, eglConfig, EGL_NO_CONTEXT, contextAttribs);
  if (!context)
  {
    debugPrintf("EGL: Could not create context: %08x\n", eglGetError());
    return 0;
  }

  eglMakeCurrent(display, surface, surface, context);
  if (eglSwapInterval(display, 1) == EGL_FALSE)
    debugPrintf("EGL: Could not set swap interval: %08x\n", eglGetError());

  debugPrintf("GL_EXTENSIONS: %s\n", glGetString(GL_EXTENSIONS));

  return 1; // success
}

void patch_opengl(void)
{
  // --- NVEventEGL* symbols DO NOT EXIST in 64-bit libGame.so ---
  // hook_arm64(so_find_addr("_Z14NVEventEGLInitv"), (uintptr_t)NVEventEGLInit);
  // hook_arm64(so_find_addr("_Z21NVEventEGLMakeCurrentv"), (uintptr_t)NVEventEGLMakeCurrent);
  // hook_arm64(so_find_addr("_Z23NVEventEGLUnmakeCurrentv"), (uintptr_t)NVEventEGLUnmakeCurrent);
  // hook_arm64(so_find_addr("_Z21NVEventEGLSwapBuffersv"), (uintptr_t)NVEventEGLSwapBuffers);
}

void deinit_opengl(void)
{
  if (display)
  {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context)
      eglDestroyContext(display, context);
    if (surface)
      eglDestroySurface(display, surface);
    eglTerminate(display);
  }
}
