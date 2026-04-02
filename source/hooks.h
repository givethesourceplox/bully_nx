#ifndef __HOOKS_H__
#define __HOOKS_H__

#include <EGL/egl.h>
#include <AL/al.h>
#include <AL/alc.h>

void patch_opengl(void);
void patch_openal(void);
void patch_game(void);
void patch_game_post_finalize(void);
void patch_io(void);
void openal_set_runtime_enabled(int enabled);

void deinit_opengl(void);
void deinit_openal(void);
ALenum alGetErrorHook(void);
void alBufferDataHook(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq);
void alDeleteBuffersHook(ALsizei n, const ALuint *buffers);
void alDeleteSourcesHook(ALsizei n, const ALuint *sources);
void alGenSourcesHook(ALsizei n, ALuint *sources);
void alGenBuffersHook(ALsizei n, ALuint *buffers);
void alGetBufferiHook(ALuint buffer, ALenum param, ALint *value);
void alGetSource3fHook(ALuint sid, ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3);
void alGetSourcefHook(ALuint sid, ALenum param, ALfloat *value);
void alGetSourceiHook(ALuint sid, ALenum param, ALint *value);
void alListener3fHook(ALenum param, ALfloat value1, ALfloat value2, ALfloat value3);
void alListenerfvHook(ALenum param, const ALfloat *values);
void alSource3fHook(ALuint sid, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3);
void alSourcePauseHook(ALuint sid);
void alSourcePlayHook(ALuint sid);
void alSourceQueueBuffersHook(ALuint sid, ALsizei numEntries, const ALuint *bids);
void alSourceRewindHook(ALuint sid);
void alSourceStopHook(ALuint sid);
void alSourceUnqueueBuffersHook(ALuint sid, ALsizei numEntries, ALuint *bids);
void alSourcefHook(ALuint sid, ALenum param, ALfloat value);
void alSourceiHook(ALuint sid, ALenum param, ALint value);
ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *attrlist);
ALCdevice *alcOpenDeviceHook(const char *name);
ALCboolean alcMakeContextCurrentHook(ALCcontext *ctx);
ALCboolean alcIsExtensionPresentHook(ALCdevice *dev, const ALCchar *extname);
void alcDestroyContextHook(ALCcontext *ctx);
ALCboolean alcCloseDeviceHook(ALCdevice *dev);
void alDeleteFiltersHook(ALsizei n, const ALuint *filters);
void alFilterfHook(ALuint filter, ALenum param, ALfloat value);
void alFilteriHook(ALuint filter, ALenum param, ALint value);
void alGenFiltersHook(ALsizei n, ALuint *filters);
ALboolean alIsFilterHook(ALuint filter);

// EGL initialization for Switch - must be called before game starts
int NVEventEGLInit(void);

// EGL globals (initialized by NVEventEGLInit)
extern EGLDisplay g_egl_display;
extern EGLSurface g_egl_surface;
extern EGLContext g_egl_context;

#endif
