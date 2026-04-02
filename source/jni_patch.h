#ifndef __JNI_PATCH_H__
#define __JNI_PATCH_H__

#include <switch.h>

void *NVThreadGetCurrentJNIEnv(void);

void jni_load(void);
void jni_gamepad_get_state(int *connected_out, u64 *buttons_out, float axes_out[6]);

#endif
