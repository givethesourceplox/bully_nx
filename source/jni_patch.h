#ifndef __JNI_PATCH_H__
#define __JNI_PATCH_H__

#include <switch.h>

void *NVThreadGetCurrentJNIEnv(void);

void jni_load(void);
void jni_gamepad_get_state(int *connected_out, u64 *buttons_out, float axes_out[6]);
int jni_async_file_pending(void);
int jni_async_file_pump(double delta_hint);
void jni_async_file_kick(void);

#endif
