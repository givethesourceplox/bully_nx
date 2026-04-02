/* openal.c -- OpenAL hooks and patches
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "../util.h"
#include "../so_util.h"

static ALCcontext *al_ctx = NULL;
static ALCdevice *al_dev = NULL;
static int al_guard_logs = 0;
static int al_disable_logs = 0;
static int al_runtime_logs = 0;
static pthread_mutex_t al_lock = PTHREAD_MUTEX_INITIALIZER;
static ALCdevice *const al_stub_dev = (ALCdevice *)(uintptr_t)1;
static ALCcontext *const al_stub_ctx = (ALCcontext *)(uintptr_t)1;
static int al_audio_disabled = 0;
static int al_playback_enabled = 0;
static ALuint al_pending_play_sources[256];
static size_t al_pending_play_count = 0;

#define OPENAL_GUARD_VOID(tag)            \
  do                                      \
  {                                       \
    pthread_mutex_lock(&al_lock);         \
    if (!openal_ensure_context_locked(tag)) \
    {                                     \
      pthread_mutex_unlock(&al_lock);     \
      return;                             \
    }                                     \
  } while (0)

#define OPENAL_GUARD_RET(tag, fallback)   \
  do                                      \
  {                                       \
    pthread_mutex_lock(&al_lock);         \
    if (!openal_ensure_context_locked(tag)) \
    {                                     \
      pthread_mutex_unlock(&al_lock);     \
      return (fallback);                  \
    }                                     \
  } while (0)

static int openal_is_stub_locked(void)
{
  return al_audio_disabled || al_dev == al_stub_dev || al_ctx == al_stub_ctx;
}

static void openal_pending_play_remove_locked(ALuint sid)
{
  size_t i;

  if (!sid)
    return;

  for (i = 0; i < al_pending_play_count; i++)
  {
    if (al_pending_play_sources[i] != sid)
      continue;

    memmove(&al_pending_play_sources[i],
            &al_pending_play_sources[i + 1],
            (al_pending_play_count - i - 1) * sizeof(al_pending_play_sources[0]));
    al_pending_play_count--;
    return;
  }
}

static void openal_pending_play_add_locked(ALuint sid)
{
  size_t i;

  if (!sid)
    return;

  for (i = 0; i < al_pending_play_count; i++)
  {
    if (al_pending_play_sources[i] == sid)
      return;
  }

  if (al_pending_play_count < sizeof(al_pending_play_sources) / sizeof(al_pending_play_sources[0]))
    al_pending_play_sources[al_pending_play_count++] = sid;
}

static int openal_ensure_context_locked(const char *tag)
{
  ALCcontext *cur = alcGetCurrentContext();

  if (openal_is_stub_locked())
  {
    if (al_disable_logs < 8)
    {
      debugPrintf("openal: %s skipped (audio disabled)\n", tag ? tag : "?");
      al_disable_logs++;
    }
    return 0;
  }

  if (cur)
  {
    al_ctx = cur;
    al_dev = alcGetContextsDevice(cur);
    return 1;
  }

  if (al_ctx)
  {
    ALCdevice *ctx_dev = alcGetContextsDevice(al_ctx);
    if (ctx_dev)
      al_dev = ctx_dev;

    if (al_dev && alcMakeContextCurrent(al_ctx) == ALC_TRUE)
      return 1;
  }

  if (al_guard_logs < 16)
  {
    debugPrintf("openal: %s skipped (dev=%p ctx=%p cur=%p)\n",
                tag ? tag : "?", al_dev, al_ctx, cur);
    al_guard_logs++;
  }
  return 0;
}

void openal_set_runtime_enabled(int enabled)
{
  pthread_mutex_lock(&al_lock);

  if (al_audio_disabled)
  {
    al_playback_enabled = 0;
    if (al_runtime_logs < 8)
    {
      debugPrintf("openal: runtime enable ignored (hard muted)\n");
      al_runtime_logs++;
    }
    pthread_mutex_unlock(&al_lock);
    return;
  }

  if (!enabled)
  {
    al_playback_enabled = 0;
    if (al_runtime_logs < 8)
    {
      debugPrintf("openal: playback gated\n");
      al_runtime_logs++;
    }
    pthread_mutex_unlock(&al_lock);
    return;
  }

  if (al_playback_enabled && al_dev && al_dev != al_stub_dev && al_ctx && al_ctx != al_stub_ctx)
  {
    pthread_mutex_unlock(&al_lock);
    return;
  }

  if (al_dev == al_stub_dev)
    al_dev = NULL;
  if (al_ctx == al_stub_ctx)
    al_ctx = NULL;

  if (!al_dev)
    al_dev = alcOpenDevice(NULL);

  if (!al_dev)
  {
    al_dev = NULL;
    al_ctx = NULL;
    if (al_runtime_logs < 8)
    {
      debugPrintf("openal: runtime enable failed (open device)\n");
      al_runtime_logs++;
    }
    pthread_mutex_unlock(&al_lock);
    return;
  }

  if (!al_ctx)
  {
    const ALCint attr[] = { ALC_FREQUENCY, 44100, 0 };
    al_ctx = alcCreateContext(al_dev, attr);
  }

  if (!al_ctx || alcMakeContextCurrent(al_ctx) != ALC_TRUE)
  {
    if (al_ctx)
      alcDestroyContext(al_ctx);
    if (al_dev)
      alcCloseDevice(al_dev);
    al_dev = NULL;
    al_ctx = NULL;
    if (al_runtime_logs < 8)
    {
      debugPrintf("openal: runtime enable failed (create/make current)\n");
      al_runtime_logs++;
    }
    pthread_mutex_unlock(&al_lock);
    return;
  }

  al_playback_enabled = 1;
  if (al_pending_play_count > 0)
  {
    size_t i;
    for (i = 0; i < al_pending_play_count; i++)
      alSourcePlay(al_pending_play_sources[i]);
    al_pending_play_count = 0;
  }

  if (al_runtime_logs < 8)
  {
    debugPrintf("openal: playback enabled dev=%p ctx=%p\n", al_dev, al_ctx);
    al_runtime_logs++;
  }
  pthread_mutex_unlock(&al_lock);
}

ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *unused) {
  pthread_mutex_lock(&al_lock);
  if (al_audio_disabled || dev == al_stub_dev)
  {
    al_dev = al_stub_dev;
    al_ctx = al_stub_ctx;
    if (al_disable_logs < 8)
    {
      debugPrintf("openal: create context -> stub\n");
      al_disable_logs++;
    }
    pthread_mutex_unlock(&al_lock);
    return al_ctx;
  }
  // override 22050hz with 44100hz in case someone wants high quality sounds
  const ALCint attr[] = { ALC_FREQUENCY, 44100, 0 };
  al_dev = dev;
  al_ctx = alcCreateContext(dev, attr); // capture context for later deinit
  pthread_mutex_unlock(&al_lock);
  return al_ctx;
}

ALCdevice *alcOpenDeviceHook(const char *name) {
  pthread_mutex_lock(&al_lock);
  if (al_audio_disabled)
  {
    al_dev = al_stub_dev;
    al_ctx = NULL;
    if (al_disable_logs < 8)
    {
      debugPrintf("openal: open device \"%s\" -> stub\n", name ? name : "(default)");
      al_disable_logs++;
    }
    pthread_mutex_unlock(&al_lock);
    return al_dev;
  }
  // capture device pointer for later deinit
  al_dev = alcOpenDevice(name);
  pthread_mutex_unlock(&al_lock);
  return al_dev;
}

ALCboolean alcMakeContextCurrentHook(ALCcontext *ctx) {
  pthread_mutex_lock(&al_lock);
  if (openal_is_stub_locked() || ctx == al_stub_ctx)
  {
    al_dev = al_stub_dev;
    al_ctx = ctx ? al_stub_ctx : NULL;
    pthread_mutex_unlock(&al_lock);
    return ALC_TRUE;
  }
  if (ctx)
    al_ctx = ctx;

  ALCboolean ret = alcMakeContextCurrent(ctx);
  if (ret == ALC_TRUE)
  {
    ALCcontext *cur = alcGetCurrentContext();
    if (cur)
    {
      al_ctx = cur;
      al_dev = alcGetContextsDevice(cur);
    }
  }
  pthread_mutex_unlock(&al_lock);
  return ret;
}

ALCboolean alcIsExtensionPresentHook(ALCdevice *dev, const ALCchar *extname) {
  (void)extname;
  pthread_mutex_lock(&al_lock);
  if (openal_is_stub_locked() || dev == al_stub_dev)
  {
    pthread_mutex_unlock(&al_lock);
    return ALC_FALSE;
  }
  pthread_mutex_unlock(&al_lock);
  return alcIsExtensionPresent(dev, extname);
}

void alcDestroyContextHook(ALCcontext *ctx) {
  pthread_mutex_lock(&al_lock);
  if (ctx == al_stub_ctx || openal_is_stub_locked())
  {
    al_ctx = NULL;
    pthread_mutex_unlock(&al_lock);
    return;
  }
  if (ctx && alcGetCurrentContext() == ctx)
    alcMakeContextCurrent(NULL);
  alcDestroyContext(ctx);
  if (ctx == al_ctx)
    al_ctx = NULL;
  pthread_mutex_unlock(&al_lock);
}

ALCboolean alcCloseDeviceHook(ALCdevice *dev) {
  pthread_mutex_lock(&al_lock);
  if (dev == al_stub_dev || openal_is_stub_locked())
  {
    al_dev = NULL;
    al_ctx = NULL;
    pthread_mutex_unlock(&al_lock);
    return ALC_TRUE;
  }
  if (alcGetCurrentContext() && (!al_ctx || alcGetCurrentContext() == al_ctx))
    alcMakeContextCurrent(NULL);
  ALCboolean ret = alcCloseDevice(dev);
  if (ret == ALC_TRUE && dev == al_dev)
  {
    al_dev = NULL;
    al_ctx = NULL;
  }
  pthread_mutex_unlock(&al_lock);
  return ret;
}

void alGenSourcesHook(ALsizei n, ALuint *sources) {
  if (sources && n > 0)
    memset(sources, 0, sizeof(*sources) * (size_t)n);

  pthread_mutex_lock(&al_lock);
  if (!openal_ensure_context_locked("alGenSources"))
  {
    pthread_mutex_unlock(&al_lock);
    return;
  }

  alGenSources(n, sources);
  pthread_mutex_unlock(&al_lock);
}

void alGenBuffersHook(ALsizei n, ALuint *buffers) {
  if (buffers && n > 0)
    memset(buffers, 0, sizeof(*buffers) * (size_t)n);

  OPENAL_GUARD_VOID("alGenBuffers");
  alGenBuffers(n, buffers);
  pthread_mutex_unlock(&al_lock);
}

ALenum alGetErrorHook(void) {
  OPENAL_GUARD_RET("alGetError", AL_NO_ERROR);
  ALenum ret = alGetError();
  pthread_mutex_unlock(&al_lock);
  return ret;
}

void alBufferDataHook(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq) {
  OPENAL_GUARD_VOID("alBufferData");
  alBufferData(buffer, format, data, size, freq);
  pthread_mutex_unlock(&al_lock);
}

void alDeleteBuffersHook(ALsizei n, const ALuint *buffers) {
  OPENAL_GUARD_VOID("alDeleteBuffers");
  alDeleteBuffers(n, buffers);
  pthread_mutex_unlock(&al_lock);
}

void alDeleteSourcesHook(ALsizei n, const ALuint *sources) {
  OPENAL_GUARD_VOID("alDeleteSources");
  if (sources && n > 0)
  {
    ALsizei i;
    for (i = 0; i < n; i++)
      openal_pending_play_remove_locked(sources[i]);
  }
  alDeleteSources(n, sources);
  pthread_mutex_unlock(&al_lock);
}

void alGetBufferiHook(ALuint buffer, ALenum param, ALint *value) {
  if (value)
    *value = 0;
  OPENAL_GUARD_VOID("alGetBufferi");
  alGetBufferi(buffer, param, value);
  pthread_mutex_unlock(&al_lock);
}

void alGetSource3fHook(ALuint sid, ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3) {
  if (value1)
    *value1 = 0.0f;
  if (value2)
    *value2 = 0.0f;
  if (value3)
    *value3 = 0.0f;
  OPENAL_GUARD_VOID("alGetSource3f");
  alGetSource3f(sid, param, value1, value2, value3);
  pthread_mutex_unlock(&al_lock);
}

void alGetSourcefHook(ALuint sid, ALenum param, ALfloat *value) {
  if (value)
    *value = 0.0f;
  OPENAL_GUARD_VOID("alGetSourcef");
  alGetSourcef(sid, param, value);
  pthread_mutex_unlock(&al_lock);
}

void alGetSourceiHook(ALuint sid, ALenum param, ALint *value) {
  if (value)
    *value = 0;
  OPENAL_GUARD_VOID("alGetSourcei");
  alGetSourcei(sid, param, value);
  pthread_mutex_unlock(&al_lock);
}

void alListener3fHook(ALenum param, ALfloat value1, ALfloat value2, ALfloat value3) {
  OPENAL_GUARD_VOID("alListener3f");
  alListener3f(param, value1, value2, value3);
  pthread_mutex_unlock(&al_lock);
}

void alListenerfvHook(ALenum param, const ALfloat *values) {
  OPENAL_GUARD_VOID("alListenerfv");
  alListenerfv(param, values);
  pthread_mutex_unlock(&al_lock);
}

void alSource3fHook(ALuint sid, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3) {
  OPENAL_GUARD_VOID("alSource3f");
  alSource3f(sid, param, value1, value2, value3);
  pthread_mutex_unlock(&al_lock);
}

void alSourcePauseHook(ALuint sid) {
  OPENAL_GUARD_VOID("alSourcePause");
  openal_pending_play_remove_locked(sid);
  alSourcePause(sid);
  pthread_mutex_unlock(&al_lock);
}

void alSourcePlayHook(ALuint sid) {
  OPENAL_GUARD_VOID("alSourcePlay");
  if (!al_playback_enabled)
  {
    openal_pending_play_add_locked(sid);
    if (al_disable_logs < 16)
    {
      debugPrintf("openal: deferred play sid=%u\n", sid);
      al_disable_logs++;
    }
    pthread_mutex_unlock(&al_lock);
    return;
  }
  alSourcePlay(sid);
  pthread_mutex_unlock(&al_lock);
}

void alSourceQueueBuffersHook(ALuint sid, ALsizei numEntries, const ALuint *bids) {
  OPENAL_GUARD_VOID("alSourceQueueBuffers");
  alSourceQueueBuffers(sid, numEntries, bids);
  pthread_mutex_unlock(&al_lock);
}

void alSourceRewindHook(ALuint sid) {
  OPENAL_GUARD_VOID("alSourceRewind");
  openal_pending_play_remove_locked(sid);
  alSourceRewind(sid);
  pthread_mutex_unlock(&al_lock);
}

void alSourceStopHook(ALuint sid) {
  OPENAL_GUARD_VOID("alSourceStop");
  openal_pending_play_remove_locked(sid);
  alSourceStop(sid);
  pthread_mutex_unlock(&al_lock);
}

void alSourceUnqueueBuffersHook(ALuint sid, ALsizei numEntries, ALuint *bids) {
  if (bids && numEntries > 0)
    memset(bids, 0, sizeof(*bids) * (size_t)numEntries);
  OPENAL_GUARD_VOID("alSourceUnqueueBuffers");
  alSourceUnqueueBuffers(sid, numEntries, bids);
  pthread_mutex_unlock(&al_lock);
}

void alSourcefHook(ALuint sid, ALenum param, ALfloat value) {
  OPENAL_GUARD_VOID("alSourcef");
  alSourcef(sid, param, value);
  pthread_mutex_unlock(&al_lock);
}

void alSourceiHook(ALuint sid, ALenum param, ALint value) {
  OPENAL_GUARD_VOID("alSourcei");
  alSourcei(sid, param, value);
  pthread_mutex_unlock(&al_lock);
}

void alDeleteFiltersHook(ALsizei n, const ALuint *filters) {
  OPENAL_GUARD_VOID("alDeleteFilters");
  alDeleteFilters(n, filters);
  pthread_mutex_unlock(&al_lock);
}

void alFilterfHook(ALuint filter, ALenum param, ALfloat value) {
  OPENAL_GUARD_VOID("alFilterf");
  alFilterf(filter, param, value);
  pthread_mutex_unlock(&al_lock);
}

void alFilteriHook(ALuint filter, ALenum param, ALint value) {
  OPENAL_GUARD_VOID("alFilteri");
  alFilteri(filter, param, value);
  pthread_mutex_unlock(&al_lock);
}

void alGenFiltersHook(ALsizei n, ALuint *filters) {
  if (filters && n > 0)
    memset(filters, 0, sizeof(*filters) * (size_t)n);
  OPENAL_GUARD_VOID("alGenFilters");
  alGenFilters(n, filters);
  pthread_mutex_unlock(&al_lock);
}

ALboolean alIsFilterHook(ALuint filter) {
  OPENAL_GUARD_RET("alIsFilter", AL_FALSE);
  ALboolean ret = alIsFilter(filter);
  pthread_mutex_unlock(&al_lock);
  return ret;
}

void patch_openal(void) {
  // used for openal
  // hook_arm64(so_find_addr("InitializeCriticalSection"), (uintptr_t)ret0);
  
  // The 64-bit libGame.so for Bully does not export al/alc functions statically.
  // We will let the game load its own audio system or fail gracefully.
}

void deinit_openal(void) {
  pthread_mutex_lock(&al_lock);
  if (al_dev && al_dev != al_stub_dev) {
    if (al_ctx && al_ctx != al_stub_ctx) {
      alcMakeContextCurrent(NULL);
      alcDestroyContext(al_ctx);
    }
    alcCloseDevice(al_dev);
  }
  al_ctx = NULL;
  al_dev = NULL;
  al_audio_disabled = 1;
  al_playback_enabled = 0;
  al_pending_play_count = 0;
  pthread_mutex_unlock(&al_lock);
}
