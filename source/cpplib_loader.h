/* cpplib_loader.h -- load libc++_shared.so for __ndk1 symbol resolution
 *
 * Copyright (C) 2026 givethesourceplox
 *
 * This is a minimal ELF loader specifically designed to load
 * libc++_shared.so from the Bully APK. It maps the library into
 * heap memory, resolves its imports against our existing libc/libm,
 * applies relocations, and exports a symbol lookup function.
 */

#ifndef __CPPLIB_LOADER_H__
#define __CPPLIB_LOADER_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int cpplib_load(const char *filename);
uintptr_t cpplib_find_symbol(const char *name);
int cpplib_resolve_symbol(const char *name, uintptr_t *out_addr);

#ifdef __cplusplus
}
#endif

#endif
