/* imports.c -- .so import resolution
 *
 * Copyright (C) 2026 givethesourceplox, fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <setjmp.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <AL/al.h>
#include <AL/alc.h>
#define AL_ALEXT_PROTOTYPES
#include <AL/efx.h>
#include <switch.h>
#include <malloc.h>

#include "config.h"
#include "cpplib_loader.h"
#include "so_util.h"
#include "util.h"
#include "hooks.h"
#include "zip_fs.h"

extern uintptr_t __cxa_atexit;

// __stack_chk_fail: make it a no-op so surviving canary checks don't abort
static void __stack_chk_fail_nop(void) { /* disabled for Switch port */ }

// C++ ABI runtime - these are provided by libstdc++/libsupc++
extern void *__cxa_allocate_exception(size_t);
extern void __cxa_throw(void *, void *, void (*)(void *));
extern void *__cxa_begin_catch(void *);
extern void __cxa_end_catch(void);
extern void __cxa_free_exception(void *);
extern void __cxa_throw_wrapper(void *, void *, void (*)(void *));
extern int __cxa_guard_acquire(int64_t *);
extern void __cxa_guard_release(int64_t *);
extern void __cxa_guard_abort(int64_t *);
extern void __cxa_pure_virtual(void);
extern void __cxa_rethrow(void);
extern int __cxa_thread_atexit(void (*)(void *), void *, void *);
extern void *__dynamic_cast(const void *, const void *, const void *, long);
extern void *__gxx_personality_v0;
extern void terminate_wrapper(void);
extern void _ZNSt9exceptionD2Ev(void *);

typedef void *(*ndk_istream_read_fn)(void *, char *, long);
typedef struct
{
  unsigned long long lo;
  unsigned long long hi;
} ndk_streampos_t;
typedef ndk_streampos_t (*ndk_istream_tellg_fn)(void *);
typedef void *(*ndk_istream_seekg_fn)(void *, unsigned long long, long long);
typedef void (*ndk_ios_base_clear_fn)(void *, unsigned int);
typedef void (*ndk_basic_ios_dtor_fn)(void *);

typedef struct
{
  uintptr_t vtable;
  uintptr_t unk8;
  char *eback;
  char *gptr;
  char *egptr;
  char *pbase;
  char *pptr;
  char *epptr;
  unsigned int mode;
  unsigned int pad0;
  char *component_begin;
  char *component_end;
  unsigned char component_open;
  char pad1[7];
  char *input_begin;
  char *input_end;
  char *output_begin;
  char *output_end;
  unsigned char auto_close;
} ndk_direct_streambuf_t;

typedef struct
{
  void *prev;
  void *next;
  void *obj;
} ndk_chain_node_t;

static ndk_istream_read_fn get_ndk_istream_read(void)
{
  static ndk_istream_read_fn fn;
  if (!fn)
    fn = (ndk_istream_read_fn)cpplib_find_symbol("_ZNSt6__ndk113basic_istreamIcNS_11char_traitsIcEEE4readEPcl");
  return fn;
}

static ndk_istream_tellg_fn get_ndk_istream_tellg(void)
{
  static ndk_istream_tellg_fn fn;
  if (!fn)
    fn = (ndk_istream_tellg_fn)cpplib_find_symbol("_ZNSt6__ndk113basic_istreamIcNS_11char_traitsIcEEE5tellgEv");
  return fn;
}

static ndk_istream_seekg_fn get_ndk_istream_seekg(void)
{
  static ndk_istream_seekg_fn fn;
  if (!fn)
    fn = (ndk_istream_seekg_fn)cpplib_find_symbol("_ZNSt6__ndk113basic_istreamIcNS_11char_traitsIcEEE5seekgENS_4fposI9mbstate_tEE");
  return fn;
}

static ndk_ios_base_clear_fn get_ndk_ios_base_clear(void)
{
  static ndk_ios_base_clear_fn fn;
  if (!fn)
    fn = (ndk_ios_base_clear_fn)cpplib_find_symbol("_ZNSt6__ndk18ios_base5clearEj");
  return fn;
}

static ndk_basic_ios_dtor_fn get_ndk_basic_ios_dtor(void)
{
  static ndk_basic_ios_dtor_fn fn;
  if (!fn)
    fn = (ndk_basic_ios_dtor_fn)cpplib_find_symbol("_ZNSt6__ndk19basic_iosIcNS_11char_traitsIcEEED2Ev");
  return fn;
}

static unsigned long long ndk_rel_caller(const void *caller)
{
  uintptr_t base = (uintptr_t)text_virtbase;
  uintptr_t addr = (uintptr_t)caller;
  if (!base || addr < base)
    return 0ull;
  return (unsigned long long)(addr - base);
}

static int g_ndk_istream_read_logs;
static int g_ndk_istream_key_logs;
static int g_ndk_istream_tellg_logs;
static int g_ndk_istream_seekg_logs;
static int g_ndk_ios_clear_logs;
static int g_ndk_stream_probe_logs;
static int g_ndk_basic_ios_dtor_logs;
static int g_strlen_null_logs;
static int g_free_guard_logs;
static int g_memcmp_guard_logs;
static int g_strcmp_guard_logs;
static int g_memalign_guard_logs;

typedef struct
{
  void *key;
  pthread_mutex_t host;
  int in_use;
} ndk_mutex_slot_t;

static pthread_mutex_t g_ndk_mutex_slots_lock = PTHREAD_MUTEX_INITIALIZER;
static ndk_mutex_slot_t g_ndk_mutex_slots[128];

static pthread_mutex_t *ndk_mutex_resolve(void *key, int create)
{
  pthread_mutex_t *ret = NULL;
  int i;

  if (!key)
    return NULL;

  pthread_mutex_lock(&g_ndk_mutex_slots_lock);

  for (i = 0; i < (int)(sizeof(g_ndk_mutex_slots) / sizeof(g_ndk_mutex_slots[0])); i++)
  {
    if (!g_ndk_mutex_slots[i].in_use || g_ndk_mutex_slots[i].key != key)
      continue;
    ret = &g_ndk_mutex_slots[i].host;
    break;
  }

  if (!ret && create)
  {
    for (i = 0; i < (int)(sizeof(g_ndk_mutex_slots) / sizeof(g_ndk_mutex_slots[0])); i++)
    {
      if (g_ndk_mutex_slots[i].in_use)
        continue;
      pthread_mutex_init(&g_ndk_mutex_slots[i].host, NULL);
      g_ndk_mutex_slots[i].key = key;
      g_ndk_mutex_slots[i].in_use = 1;
      ret = &g_ndk_mutex_slots[i].host;
      break;
    }
  }

  pthread_mutex_unlock(&g_ndk_mutex_slots_lock);
  return ret;
}

int ndk_mutex_lock_wrapper(void *mutex_obj)
{
  pthread_mutex_t *m = ndk_mutex_resolve(mutex_obj, 1);
  if (!m)
    return 0;
  return pthread_mutex_lock(m);
}

int ndk_mutex_unlock_wrapper(void *mutex_obj)
{
  pthread_mutex_t *m = ndk_mutex_resolve(mutex_obj, 0);
  if (!m)
    return 0;
  return pthread_mutex_unlock(m);
}

int ndk_mutex_trylock_wrapper(void *mutex_obj)
{
  pthread_mutex_t *m = ndk_mutex_resolve(mutex_obj, 1);
  if (!m)
    return 0;
  return pthread_mutex_trylock(m);
}

void ndk_mutex_dtor_wrapper(void *mutex_obj)
{
  int i;

  if (!mutex_obj)
    return;

  pthread_mutex_lock(&g_ndk_mutex_slots_lock);
  for (i = 0; i < (int)(sizeof(g_ndk_mutex_slots) / sizeof(g_ndk_mutex_slots[0])); i++)
  {
    if (!g_ndk_mutex_slots[i].in_use || g_ndk_mutex_slots[i].key != mutex_obj)
      continue;
    pthread_mutex_destroy(&g_ndk_mutex_slots[i].host);
    memset(&g_ndk_mutex_slots[i], 0, sizeof(g_ndk_mutex_slots[i]));
    break;
  }
  pthread_mutex_unlock(&g_ndk_mutex_slots_lock);
}

void *memalign_wrapper(size_t alignment, size_t size)
{
  void *ptr = NULL;

  if (alignment <= sizeof(void *) || alignment <= 16)
    ptr = malloc(size);
  else
    ptr = memalign(alignment, size);

  if (!ptr && alignment > sizeof(void *))
    ptr = malloc(size);

  if (!ptr && g_memalign_guard_logs < 32)
  {
    debugPrintf("memalign_wrapper: alloc failed align=%zu size=%zu\n", alignment, size);
    g_memalign_guard_logs++;
  }

  return ptr;
}

static int free_wrapper_looks_like_path(const void *ptr, char preview[65])
{
  const unsigned char *s = (const unsigned char *)ptr;
  int i;
  int sep = 0;
  int alpha = 0;
  int digits = 0;
  int dots = 0;
  int underscores = 0;
  int printable = 0;
  static const char *const resource_tokens[] = {
      "Act", "Anim", "Props", "Door", "IDLE", "ESC", "Hud", "UI", ".img", ".act", ".wad", ".txd"};

  if (preview)
    preview[0] = '\0';

  if (!ptr)
    return 0;

  for (i = 0; i < 64; i++)
  {
    unsigned char c = s[i];
    if (preview)
      preview[i] = (char)c;

    if (c == '\0')
      break;

    if (c < 0x20 || c > 0x7e)
    {
      if (preview)
        preview[i] = '\0';
      return 0;
    }

    printable++;

    if (c == '/' || c == '\\')
      sep = 1;
    else if (isalpha(c))
      alpha = 1;
    else if (isdigit(c))
      digits = 1;
    else if (c == '.')
      dots = 1;
    else if (c == '_')
      underscores = 1;
  }

  if (preview)
    preview[i < 64 ? i : 64] = '\0';

  if (printable < 4 || !alpha)
    return 0;

  if (preview && (strncmp(preview, "/Act/", 5) == 0 ||
                  strncmp(preview, "Act/", 4) == 0 ||
                  strncmp(preview, "Anim/", 5) == 0 ||
                  strncmp(preview, "/Anim/", 6) == 0 ||
                  strncmp(preview, "Props/", 6) == 0 ||
                  strncmp(preview, "/Props/", 7) == 0 ||
                  strncmp(preview, "ESC", 3) == 0))
    return 1;

  if (preview && (preview[0] == '/' || strchr(preview, '\\') != NULL))
    return 1;

  if (preview && printable >= 8 && (dots || underscores || digits))
  {
    unsigned i;
    for (i = 0; i < sizeof(resource_tokens) / sizeof(resource_tokens[0]); i++)
    {
      if (strstr(preview, resource_tokens[i]) != NULL)
        return 1;
    }
  }

  return 0;
}

void free_wrapper(void *ptr)
{
  uintptr_t p = (uintptr_t)ptr;
  char preview[65];

  if (!ptr)
    return;

  // Switch user-space pointers stay below 48 bits. If upper bits are set,
  // the caller handed us garbage, often an inline ASCII path blob rather than
  // a real heap pointer. Skip the free so we don't explode in _free_r.
  if ((p >> 48) != 0)
  {
    if (g_free_guard_logs < 32)
    {
      char text[9];
      memcpy(text, &p, 8);
      text[8] = '\0';
      debugPrintf("free_wrapper: ignoring bogus ptr=%p text=\"%s\"\n", ptr, text);
      g_free_guard_logs++;
    }
    return;
  }

  // Resource streaming occasionally hands delete/free a pointer to a
  // filename buffer instead of heap memory. Treat obvious path strings as
  // non-owning references and ignore the free to keep the worker thread alive.
  if (free_wrapper_looks_like_path(ptr, preview))
  {
    if (g_free_guard_logs < 32)
    {
      debugPrintf("free_wrapper: ignoring path-like ptr=%p text=\"%s\"\n", ptr, preview);
      g_free_guard_logs++;
    }
    return;
  }

  free(ptr);
}

void *realloc_wrapper(void *ptr, size_t size)
{
  uintptr_t p = (uintptr_t)ptr;
  char preview[65];

  if (!ptr)
    return realloc(NULL, size);

  if (size == 0)
  {
    free_wrapper(ptr);
    return NULL;
  }

  if ((p >> 48) != 0)
  {
    void *fresh = calloc(1, size);
    if (g_free_guard_logs < 32)
    {
      char text[9];
      memcpy(text, &p, 8);
      text[8] = '\0';
      debugPrintf("realloc_wrapper: replaced bogus ptr=%p text=\"%s\" size=%zu -> %p\n",
                  ptr, text, size, fresh);
      g_free_guard_logs++;
    }
    return fresh;
  }

  if (free_wrapper_looks_like_path(ptr, preview))
  {
    size_t copy_len = strnlen((const char *)ptr, 64);
    void *fresh = calloc(1, size);
    if (fresh && copy_len > 0)
    {
      if (copy_len >= size)
        copy_len = size - 1;
      memcpy(fresh, ptr, copy_len);
      ((char *)fresh)[copy_len] = '\0';
    }
    if (g_free_guard_logs < 32)
    {
      debugPrintf("realloc_wrapper: detached path-like ptr=%p text=\"%s\" size=%zu -> %p\n",
                  ptr, preview, size, fresh);
      g_free_guard_logs++;
    }
    return fresh;
  }

  return realloc(ptr, size);
}

static void *ndk_stream_ios_base(void *stream)
{
  uintptr_t vtable;
  intptr_t adjust;

  if (!stream)
    return NULL;

  vtable = *(uintptr_t *)stream;
  if (!vtable)
    return NULL;

  adjust = *(const intptr_t *)(vtable - 0x18);
  return (char *)stream + adjust;
}

static int ndk_action_tree_stream_caller(unsigned long long rel)
{
  return rel >= 0xa1fc64ull && rel < 0xa20f00ull;
}

static int ndk_action_tree_basic_ios_dtor_caller(unsigned long long rel)
{
  return rel == 0xa21888ull || rel == 0xa21178ull || rel == 0xa21394ull;
}

static int ndk_action_tree_streambuf_valid(ndk_direct_streambuf_t *buf)
{
  size_t size;

  if (!buf)
    return 0;

  if (buf->input_begin && buf->input_end && buf->input_end >= buf->input_begin)
  {
    size = (size_t)(buf->input_end - buf->input_begin);
    if (size > 0 && size <= (64u * 1024u * 1024u))
      return 1;
  }

  if (buf->component_begin && buf->component_end && buf->component_end >= buf->component_begin)
  {
    size = (size_t)(buf->component_end - buf->component_begin);
    if (size > 0 && size <= (64u * 1024u * 1024u))
      return 1;
  }

  if (buf->eback && buf->egptr && buf->egptr >= buf->eback)
  {
    size = (size_t)(buf->egptr - buf->eback);
    if (size > 0 && size <= (64u * 1024u * 1024u))
      return 1;
  }

  return 0;
}

static ndk_direct_streambuf_t *ndk_action_tree_streambuf(void *stream, void **out_ios)
{
  void *ios = ndk_stream_ios_base(stream);
  ndk_direct_streambuf_t *buf = NULL;

  if (out_ios)
    *out_ios = ios;

  if (ios)
  {
    buf = (ndk_direct_streambuf_t *)*(void **)((char *)ios + 0x28);
    if (ndk_action_tree_streambuf_valid(buf))
      return buf;
  }

  // Fallback: the vtable-based ios_base lookup fails because the istream
  // subobject's vtable pointer points into remapped code memory rather than
  // valid vtable data (the game calls tellg/read via PLT, not virtual
  // dispatch, so the vtable is never set correctly by so_relocate).
  // Probe known vbase offsets from the istream this-pointer to ios_base.
  // In the game's boost stream layout, basic_ios is at istream - 0x10.
  if (stream)
  {
    static const int vbase_offsets[] = {-0x10, 0x10, 0x20};
    for (unsigned i = 0; i < sizeof(vbase_offsets) / sizeof(vbase_offsets[0]); i++)
    {
      void *candidate = (char *)stream + vbase_offsets[i];
      buf = (ndk_direct_streambuf_t *)*(void **)((char *)candidate + 0x28);
      if (ndk_action_tree_streambuf_valid(buf))
      {
        if (out_ios)
          *out_ios = candidate;
        return buf;
      }
    }
  }

  return NULL;
}

static void ndk_action_tree_stream_probe(void *stream, void *ios, void *rdbuf, unsigned long long rel)
{
  uintptr_t stream_q0 = 0, stream_q1 = 0, stream_q2 = 0;
  uintptr_t ios_q0 = 0, ios_q1 = 0, ios_q2 = 0;
  uintptr_t buf_q0 = 0, buf_q1 = 0, buf_q2 = 0, buf_q3 = 0, buf_q4 = 0;

  if (g_ndk_stream_probe_logs >= 8)
    return;

  if (stream)
  {
    stream_q0 = *(uintptr_t *)((char *)stream + 0x0);
    stream_q1 = *(uintptr_t *)((char *)stream + 0x8);
    stream_q2 = *(uintptr_t *)((char *)stream + 0x10);
  }

  if (ios)
  {
    ios_q0 = *(uintptr_t *)((char *)ios + 0x20);
    ios_q1 = *(uintptr_t *)((char *)ios + 0x28);
    ios_q2 = *(uintptr_t *)((char *)ios + 0x30);
  }

  if (rdbuf)
  {
    buf_q0 = *(uintptr_t *)((char *)rdbuf + 0x0);
    buf_q1 = *(uintptr_t *)((char *)rdbuf + 0x10);
    buf_q2 = *(uintptr_t *)((char *)rdbuf + 0x18);
    buf_q3 = *(uintptr_t *)((char *)rdbuf + 0x20);
    buf_q4 = *(uintptr_t *)((char *)rdbuf + 0x60);
  }

  debugPrintf("ndk_stream_probe: rel=0x%llx stream={%p,%p,%p} ios={0x%llx,%p,0x%llx} rdbuf={0x%llx,0x%llx,0x%llx,0x%llx,0x%llx}\n",
              rel,
              (void *)stream_q0, (void *)stream_q1, (void *)stream_q2,
              (unsigned long long)ios_q0, (void *)ios_q1, (unsigned long long)ios_q2,
              (unsigned long long)buf_q0, (unsigned long long)buf_q1,
              (unsigned long long)buf_q2, (unsigned long long)buf_q3,
              (unsigned long long)buf_q4);
  g_ndk_stream_probe_logs++;
}

static size_t ndk_action_tree_stream_pos(ndk_direct_streambuf_t *buf)
{
  char *cur;
  char *begin = NULL;
  char *end = NULL;

  if (!buf)
    return 0;

  if (buf->input_begin && buf->input_end && buf->input_end >= buf->input_begin)
  {
    begin = buf->input_begin;
    end = buf->input_end;
  }
  else if (buf->component_begin && buf->component_end && buf->component_end >= buf->component_begin)
  {
    begin = buf->component_begin;
    end = buf->component_end;
  }
  else if (buf->eback && buf->egptr && buf->egptr >= buf->eback)
  {
    begin = buf->eback;
    end = buf->egptr;
  }

  if (!begin || !end)
    return 0;

  if (!buf->gptr)
  {
    buf->eback = begin;
    buf->gptr = begin;
    buf->egptr = end;
  }

  cur = buf->gptr ? buf->gptr : begin;
  if (cur < begin)
    cur = begin;
  if (cur > end)
    cur = end;
  return (size_t)(cur - begin);
}

static size_t ndk_action_tree_stream_size(ndk_direct_streambuf_t *buf)
{
  if (!buf)
    return 0;
  if (buf->input_begin && buf->input_end && buf->input_end >= buf->input_begin)
    return (size_t)(buf->input_end - buf->input_begin);
  if (buf->component_begin && buf->component_end && buf->component_end >= buf->component_begin)
    return (size_t)(buf->component_end - buf->component_begin);
  if (buf->eback && buf->egptr && buf->egptr >= buf->eback)
    return (size_t)(buf->egptr - buf->eback);
  return 0;
}

static char *ndk_action_tree_stream_begin(ndk_direct_streambuf_t *buf)
{
  if (!buf)
    return NULL;
  if (buf->input_begin && buf->input_end && buf->input_end >= buf->input_begin)
    return buf->input_begin;
  if (buf->component_begin && buf->component_end && buf->component_end >= buf->component_begin)
    return buf->component_begin;
  if (buf->eback && buf->egptr && buf->egptr >= buf->eback)
    return buf->eback;
  return NULL;
}

static char *ndk_action_tree_stream_end(ndk_direct_streambuf_t *buf)
{
  if (!buf)
    return NULL;
  if (buf->input_begin && buf->input_end && buf->input_end >= buf->input_begin)
    return buf->input_end;
  if (buf->component_begin && buf->component_end && buf->component_end >= buf->component_begin)
    return buf->component_end;
  if (buf->eback && buf->egptr && buf->egptr >= buf->eback)
    return buf->egptr;
  return NULL;
}

static void *ndk_istream_read_wrapper(void *stream, char *dst, long count)
{
  ndk_istream_read_fn fn = get_ndk_istream_read();
  const void *caller = __builtin_return_address(0);
  void *ios = NULL;
  ndk_direct_streambuf_t *direct_buf = ndk_action_tree_streambuf(stream, &ios);
  unsigned int state = ios ? *(unsigned int *)((char *)ios + 0x20) : 0;
  unsigned int exceptions = ios ? *(unsigned int *)((char *)ios + 0x24) : 0;
  void *rdbuf = ios ? *(void **)((char *)ios + 0x28) : NULL;
  unsigned long long rel = ndk_rel_caller(caller);
  int should_log = g_ndk_istream_read_logs < 64 || (count == 4 && g_ndk_istream_key_logs < 64);
  char *base = ndk_action_tree_stream_begin(direct_buf);
  char *end = ndk_action_tree_stream_end(direct_buf);

  ndk_action_tree_stream_probe(stream, ios, rdbuf, rel);

  if (should_log)
  {
    debugPrintf("ndk_istream::read: caller=%p rel=0x%llx stream=%p ios=%p count=%ld state=0x%x exc=0x%x rdbuf=%p direct=%p pos=%zu/%zu\n",
                caller, rel, stream, ios, count, state, exceptions, rdbuf, direct_buf,
                ndk_action_tree_stream_pos(direct_buf), ndk_action_tree_stream_size(direct_buf));
    g_ndk_istream_read_logs++;
    if (count == 4)
      g_ndk_istream_key_logs++;
  }

  if (stream && direct_buf)
  {
    size_t pos = ndk_action_tree_stream_pos(direct_buf);
    size_t size = ndk_action_tree_stream_size(direct_buf);
    size_t avail = pos <= size ? (size - pos) : 0;
    size_t to_copy = avail < (size_t)count ? avail : (size_t)count;

    if (to_copy && dst && base)
      memcpy(dst, base + pos, to_copy);

    if (base && end)
    {
      direct_buf->eback = base;
      direct_buf->gptr = base + pos + to_copy;
      direct_buf->egptr = end;
    }
    *(unsigned long long *)((char *)stream + 0x8) = (unsigned long long)to_copy;

    if (should_log && dst && count > 0 && count <= 8)
    {
      unsigned int value = 0;
      memcpy(&value, dst, to_copy < sizeof(value) ? to_copy : sizeof(value));
      debugPrintf("ndk_istream::read direct: rel=0x%llx bytes=%zu/%ld value=0x%08x pos=%zu->%zu\n",
                  rel, to_copy, count, value, pos, pos + to_copy);
    }

    return stream;
  }

  if (!fn)
  {
    debugPrintf("ndk_istream::read: cpplib symbol missing\n");
    return stream;
  }

  fn(stream, dst, count);

  if (should_log && dst && count > 0 && count <= 8)
  {
    unsigned int value = 0;
    memcpy(&value, dst, count < (long)sizeof(value) ? (size_t)count : sizeof(value));
    debugPrintf("ndk_istream::read done: rel=0x%llx bytes=%ld value=0x%08x state=0x%x\n",
                ndk_rel_caller(caller), count, value,
                stream ? *(unsigned int *)((char *)stream + 0x20) : 0);
  }

  return stream;
}

static ndk_streampos_t ndk_istream_tellg_wrapper(void *stream)
{
  ndk_istream_tellg_fn fn = get_ndk_istream_tellg();
  const void *caller = __builtin_return_address(0);
  unsigned long long rel = ndk_rel_caller(caller);
  void *ios = NULL;
  ndk_direct_streambuf_t *direct_buf = ndk_action_tree_streambuf(stream, &ios);
  unsigned int old_state = ios ? *(unsigned int *)((char *)ios + 0x20) : 0;
  unsigned int old_exceptions = ios ? *(unsigned int *)((char *)ios + 0x24) : 0;
  void *rdbuf = ios ? *(void **)((char *)ios + 0x28) : NULL;
  ndk_streampos_t ret = {0, 0};
  int direct = 0;

  ndk_action_tree_stream_probe(stream, ios, rdbuf, rel);

  if (g_ndk_istream_tellg_logs < 64)
  {
    debugPrintf("ndk_istream::tellg: caller=%p rel=0x%llx stream=%p ios=%p state=0x%x exc=0x%x rdbuf=%p direct=%p pos=%zu/%zu\n",
                caller, rel, stream, ios, old_state, old_exceptions, rdbuf, direct_buf,
                ndk_action_tree_stream_pos(direct_buf), ndk_action_tree_stream_size(direct_buf));
    g_ndk_istream_tellg_logs++;
  }

  if (stream && direct_buf)
  {
    ret.lo = 0;
    ret.hi = ndk_action_tree_stream_pos(direct_buf);
    direct = 1;
  }

  if (direct)
  {
    if (g_ndk_istream_tellg_logs < 96)
    {
      debugPrintf("ndk_istream::tellg direct: rel=0x%llx ret={0x%llx,0x%llx} pos=%zu/%zu\n",
                  rel, ret.lo, ret.hi,
                  ndk_action_tree_stream_pos(direct_buf), ndk_action_tree_stream_size(direct_buf));
      g_ndk_istream_tellg_logs++;
    }
    return ret;
  }

  if (!fn)
  {
    debugPrintf("ndk_istream::tellg: cpplib symbol missing\n");
    return ret;
  }

  ret = fn(stream);

  if (g_ndk_istream_tellg_logs < 96)
  {
    debugPrintf("ndk_istream::tellg done: rel=0x%llx ret={0x%llx,0x%llx} state=0x%x exc=0x%x direct=%d\n",
                rel, ret.lo, ret.hi,
                ios ? *(unsigned int *)((char *)ios + 0x20) : 0,
                ios ? *(unsigned int *)((char *)ios + 0x24) : 0,
                direct);
    g_ndk_istream_tellg_logs++;
  }

  return ret;
}

static void *ndk_istream_seekg_wrapper(void *stream, unsigned long long state, long long off)
{
  ndk_istream_seekg_fn fn = get_ndk_istream_seekg();
  const void *caller = __builtin_return_address(0);
  unsigned long long rel = ndk_rel_caller(caller);
  void *ios = NULL;
  ndk_direct_streambuf_t *direct_buf = ndk_action_tree_streambuf(stream, &ios);
  void *rdbuf = ios ? *(void **)((char *)ios + 0x28) : NULL;
  char *base = ndk_action_tree_stream_begin(direct_buf);
  char *end = ndk_action_tree_stream_end(direct_buf);

  ndk_action_tree_stream_probe(stream, ios, rdbuf, rel);

  if (g_ndk_istream_seekg_logs < 64)
  {
    debugPrintf("ndk_istream::seekg: caller=%p rel=0x%llx stream=%p ios=%p direct=%p fpos={0x%llx,0x%llx} pos=%zu/%zu\n",
                caller, rel, stream, ios, direct_buf, state, (unsigned long long)off,
                ndk_action_tree_stream_pos(direct_buf), ndk_action_tree_stream_size(direct_buf));
    g_ndk_istream_seekg_logs++;
  }

  if (stream && direct_buf)
  {
    size_t size = ndk_action_tree_stream_size(direct_buf);
    size_t pos = off < 0 ? size + 1 : (size_t)off;

    if (pos <= size && base && end)
    {
      direct_buf->eback = base;
      direct_buf->gptr = base + pos;
      direct_buf->egptr = end;
    }

    if (g_ndk_istream_seekg_logs < 96)
    {
      debugPrintf("ndk_istream::seekg direct: rel=0x%llx pos=%zu/%zu ok=%d\n",
                  rel, pos, size, pos <= size);
      g_ndk_istream_seekg_logs++;
    }

    return stream;
  }

  if (!fn)
  {
    debugPrintf("ndk_istream::seekg: cpplib symbol missing\n");
    return stream;
  }

  return fn(stream, state, off);
}

static void ndk_ios_base_clear_wrapper(void *ios, unsigned int state)
{
  ndk_ios_base_clear_fn fn = get_ndk_ios_base_clear();
  const void *caller = __builtin_return_address(0);
  unsigned long long rel = ndk_rel_caller(caller);
  void *buf = ios ? *(void **)((char *)ios + 0x28) : NULL;

  ndk_action_tree_stream_probe(NULL, ios, buf, rel);

  if (g_ndk_ios_clear_logs < 64)
  {
    debugPrintf("ndk_ios_base::clear: caller=%p rel=0x%llx ios=%p arg=0x%x cur=0x%x exc=0x%x buf=%p\n",
                caller, rel, ios, state,
                ios ? *(unsigned int *)((char *)ios + 0x20) : 0,
                ios ? *(unsigned int *)((char *)ios + 0x24) : 0,
                buf);
    g_ndk_ios_clear_logs++;
  }

  // ActionTreeReader construction calls ios_base::clear(0) on a stream-relative
  // object, not a real libc++ ios_base. Forwarding that into libc++ corrupts the
  // filtering_stream's shared_ptr at +0x20, which later crashes in its destructor.
  if (rel == 0xa21348ull)
  {
    if (g_ndk_ios_clear_logs < 96)
    {
      debugPrintf("ndk_ios_base::clear: skipped ctor clear rel=0x%llx ios=%p state=0x%x\n",
                  rel, ios, state);
      g_ndk_ios_clear_logs++;
    }
    return;
  }

  if (!fn)
  {
    debugPrintf("ndk_ios_base::clear: cpplib symbol missing\n");
    return;
  }

  fn(ios, state);
}

static void ndk_basic_ios_dtor_wrapper(void *ios)
{
  ndk_basic_ios_dtor_fn fn = get_ndk_basic_ios_dtor();
  const void *caller = __builtin_return_address(0);
  unsigned long long rel = ndk_rel_caller(caller);
  void *locale_impl = ios ? *(void **)((char *)ios + 0x30) : NULL;

  if (g_ndk_basic_ios_dtor_logs < 32)
  {
    debugPrintf("ndk_basic_ios_dtor: caller=%p rel=0x%llx ios=%p locale=%p\n",
                caller, rel, ios, locale_impl);
    g_ndk_basic_ios_dtor_logs++;
  }

  if (ndk_action_tree_basic_ios_dtor_caller(rel) && !locale_impl)
  {
    if (g_ndk_basic_ios_dtor_logs < 64)
    {
      debugPrintf("ndk_basic_ios_dtor: skipped ActionTreeReader teardown rel=0x%llx ios=%p\n",
                  rel, ios);
      g_ndk_basic_ios_dtor_logs++;
    }
    return;
  }

  if (!fn)
  {
    debugPrintf("ndk_basic_ios_dtor: cpplib symbol missing\n");
    return;
  }

  fn(ios);
}

static size_t strlen_wrapper(const char *s)
{
  if (!s)
  {
    void *caller = __builtin_return_address(0);
    unsigned long long rel = ndk_rel_caller(caller);
    if (g_strlen_null_logs < 64)
    {
      debugPrintf("strlen_wrapper: caller=%p rel=0x%llx s=NULL -> 0\n",
                  caller, rel);
      g_strlen_null_logs++;
    }
    return 0;
  }

  return strlen(s);
}

static int memcmp_wrapper(const void *a, const void *b, size_t n)
{
  if (n == 0 || a == b)
    return 0;

  if (!a || !b)
  {
    void *caller = __builtin_return_address(0);
    unsigned long long rel = ndk_rel_caller(caller);

    if (g_memcmp_guard_logs < 64)
    {
      debugPrintf("memcmp_wrapper: caller=%p rel=0x%llx a=%p b=%p n=%zu -> guard\n",
                  caller, rel, a, b, n);
      g_memcmp_guard_logs++;
    }

    if (!a && !b)
      return 0;
    return a ? 1 : -1;
  }

  return memcmp(a, b, n);
}

static int strcmp_wrapper(const char *a, const char *b)
{
  if (a == b)
    return 0;

  if (!a || !b)
  {
    void *caller = __builtin_return_address(0);
    unsigned long long rel = ndk_rel_caller(caller);

    if (g_strcmp_guard_logs < 64)
    {
      debugPrintf("strcmp_wrapper: caller=%p rel=0x%llx a=%p b=%p -> guard\n",
                  caller, rel, a, b);
      g_strcmp_guard_logs++;
    }

    if (!a && !b)
      return 0;
    return a ? 1 : -1;
  }

  return strcmp(a, b);
}

static int strncmp_wrapper(const char *a, const char *b, size_t n)
{
  if (n == 0 || a == b)
    return 0;

  if (!a || !b)
  {
    void *caller = __builtin_return_address(0);
    unsigned long long rel = ndk_rel_caller(caller);

    if (g_strcmp_guard_logs < 64)
    {
      debugPrintf("strncmp_wrapper: caller=%p rel=0x%llx a=%p b=%p n=%zu -> guard\n",
                  caller, rel, a, b, n);
      g_strcmp_guard_logs++;
    }

    if (!a && !b)
      return 0;
    return a ? 1 : -1;
  }

  return strncmp(a, b, n);
}

static size_t __strlen_chk_wrapper(const char *s, size_t maxlen)
{
  (void)maxlen;
  return strlen_wrapper(s);
}

// ============================================================================
// AAsset (Android Asset Manager) Wrappers
// The game uses AAsset API internally (e.g. NvAPK calls AAssetManager_open).
// We back these with regular filesystem I/O from the assets/ directory.
// ============================================================================

typedef struct
{
  FILE *fp;
  long size;
} FakeAAsset;

static int fake_asset_manager_storage; // just need a non-NULL address

void *AAssetManager_fromJava_wrapper(void *env, void *assetManager)
{
  debugPrintf("AAssetManager_fromJava_wrapper: returning fake manager\n");
  return &fake_asset_manager_storage;
}

void *AAssetManager_open_wrapper(void *mgr, const char *filename, int mode)
{
  char path[512];
  snprintf(path, sizeof(path), "assets/%s", filename);
  debugPrintf("AAssetManager_open_wrapper: opening %s\n", path);

  FILE *fp = fopen(path, "rb");
  if (!fp)
  {
    debugPrintf("AAssetManager_open_wrapper: FAILED to open %s\n", path);
    return NULL;
  }

  FakeAAsset *asset = calloc(1, sizeof(FakeAAsset));
  asset->fp = fp;

  fseek(fp, 0, SEEK_END);
  asset->size = ftell(fp);
  fseek(fp, 0, SEEK_SET);


  debugPrintf("AAssetManager_open_wrapper: opened %s (size=%ld)\n", path, asset->size);
  return asset;
}

int AAsset_read_wrapper(void *asset_ptr, void *buf, size_t count)
{
  FakeAAsset *asset = (FakeAAsset *)asset_ptr;
  if (!asset || !asset->fp)
    return -1;
  return (int)fread(buf, 1, count, asset->fp);
}

long AAsset_getLength_wrapper(void *asset_ptr)
{
  FakeAAsset *asset = (FakeAAsset *)asset_ptr;
  if (!asset)
    return 0;
  return asset->size;
}

long long AAsset_getLength64_wrapper(void *asset_ptr)
{
  FakeAAsset *asset = (FakeAAsset *)asset_ptr;
  if (!asset)
    return 0;
  return (long long)asset->size;
}

long AAsset_getRemainingLength_wrapper(void *asset_ptr)
{
  FakeAAsset *asset = (FakeAAsset *)asset_ptr;
  if (!asset || !asset->fp)
    return 0;
  long cur = ftell(asset->fp);
  return asset->size - cur;
}

long long AAsset_getRemainingLength64_wrapper(void *asset_ptr)
{
  FakeAAsset *asset = (FakeAAsset *)asset_ptr;
  if (!asset || !asset->fp)
    return 0;
  long cur = ftell(asset->fp);
  return (long long)(asset->size - cur);
}

long AAsset_seek_wrapper(void *asset_ptr, long offset, int whence)
{
  FakeAAsset *asset = (FakeAAsset *)asset_ptr;
  if (!asset || !asset->fp)
    return -1;
  if (fseek(asset->fp, offset, whence) != 0)
    return -1;
  return ftell(asset->fp);
}

long long AAsset_seek64_wrapper(void *asset_ptr, long long offset, int whence)
{
  FakeAAsset *asset = (FakeAAsset *)asset_ptr;
  if (!asset || !asset->fp)
    return -1;
  if (fseek(asset->fp, (long)offset, whence) != 0)
    return -1;
  return (long long)ftell(asset->fp);
}

void AAsset_close_wrapper(void *asset_ptr)
{
  FakeAAsset *asset = (FakeAAsset *)asset_ptr;
  if (!asset)
    return;
  debugPrintf("AAsset_close_wrapper: closing asset\n");
  if (asset->fp)
    fclose(asset->fp);
  free(asset);
}

// ============================================================================
// EGL Wrapper Functions for Switch
// These intercept EGL calls from the Android .so and return our pre-initialized
// EGL objects instead of letting the game try to create them (which would fail
// because Android native windows don't exist on Switch).
// ============================================================================

static EGLBoolean egl_initialized = EGL_FALSE;

// Lazy-init helper: if NVEventEGLInit wasn't called yet, do it now
static void ensure_egl_init(void)
{
  if (g_egl_display == EGL_NO_DISPLAY)
  {
    debugPrintf("ensure_egl_init: lazy EGL init triggered\n");
    NVEventEGLInit();
  }
}

EGLDisplay eglGetDisplay_wrapper(EGLNativeDisplayType display_id)
{
  ensure_egl_init();
  debugPrintf("eglGetDisplay_wrapper: returning pre-initialized display %p\n", g_egl_display);
  return g_egl_display;
}

EGLBoolean eglInitialize_wrapper(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
  ensure_egl_init();
  debugPrintf("eglInitialize_wrapper: EGL already initialized (display=%p)\n", g_egl_display);
  if (major)
    *major = 1;
  if (minor)
    *minor = 4;
  egl_initialized = EGL_TRUE;
  return EGL_TRUE;
}

EGLBoolean eglChooseConfig_wrapper(EGLDisplay dpy, const EGLint *attrib_list,
                                   EGLConfig *configs, EGLint config_size,
                                   EGLint *num_config)
{
  debugPrintf("eglChooseConfig_wrapper: calling real eglChooseConfig\n");
  return eglChooseConfig(g_egl_display, attrib_list, configs, config_size, num_config);
}

EGLSurface eglCreateWindowSurface_wrapper(EGLDisplay dpy, EGLConfig config,
                                          EGLNativeWindowType win,
                                          const EGLint *attrib_list)
{
  ensure_egl_init();
  debugPrintf("eglCreateWindowSurface_wrapper: returning pre-initialized surface %p (ignoring window %p)\n", g_egl_surface, win);
  return g_egl_surface;
}

EGLContext eglCreateContext_wrapper(EGLDisplay dpy, EGLConfig config,
                                    EGLContext share_context,
                                    const EGLint *attrib_list)
{
  ensure_egl_init();
  debugPrintf("eglCreateContext_wrapper: returning pre-initialized context %p\n", g_egl_context);
  return g_egl_context;
}

static pthread_mutex_t s_egl_call_mutex;
static pthread_once_t s_egl_call_once = PTHREAD_ONCE_INIT;

static void egl_call_mutex_init(void)
{
  pthread_mutexattr_t attr;

  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&s_egl_call_mutex, &attr);
  pthread_mutexattr_destroy(&attr);
}

static void egl_call_lock(void)
{
  pthread_once(&s_egl_call_once, egl_call_mutex_init);
  pthread_mutex_lock(&s_egl_call_mutex);
}

static void egl_call_unlock(void)
{
  pthread_mutex_unlock(&s_egl_call_mutex);
}

EGLBoolean eglMakeCurrent_wrapper(EGLDisplay dpy, EGLSurface draw,
                                  EGLSurface read, EGLContext ctx)
{
  EGLBoolean ok;

  ensure_egl_init();
  egl_call_lock();
  if (ctx == EGL_NO_CONTEXT || draw == EGL_NO_SURFACE)
  {
    debugPrintf("eglMakeCurrent_wrapper: RELEASING context on current thread\n");
    ok = eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ok == EGL_FALSE)
      debugPrintf("eglMakeCurrent_wrapper: release FAILED err=0x%x\n", eglGetError());
    egl_call_unlock();
    return ok;
  }
  debugPrintf("eglMakeCurrent_wrapper: ACQUIRING context on current thread\n");
  ok = eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context);
  if (ok == EGL_FALSE)
    debugPrintf("eglMakeCurrent_wrapper: acquire FAILED err=0x%x\n", eglGetError());
  egl_call_unlock();
  return ok;
}

// GL debug wrappers — trace key calls to see if game ever reaches rendering
static int gl_clear_count = 0;
static int gl_draw_count = 0;
static int gl_viewport_count = 0;
static int gl_shader_count = 0;
static int g_egl_swap_count = 0;

void glClear_wrapper(GLbitfield mask)
{
  gl_clear_count++;
  if (gl_clear_count <= 5 || (gl_clear_count % 500 == 0))
    debugPrintf("glClear: mask=0x%x (call #%d)\n", mask, gl_clear_count);
  glClear(mask);
}

void glViewport_wrapper(GLint x, GLint y, GLsizei w, GLsizei h)
{
  gl_viewport_count++;
  if (gl_viewport_count <= 5 || (gl_viewport_count % 500 == 0))
    debugPrintf("glViewport: %d,%d %dx%d (call #%d)\n", x, y, w, h, gl_viewport_count);
  glViewport(x, y, w, h);
}

void glDrawElements_wrapper(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
  gl_draw_count++;
  if (gl_draw_count <= 5 || (gl_draw_count % 500 == 0))
    debugPrintf("glDrawElements: mode=0x%x count=%d (call #%d)\n", mode, count, gl_draw_count);
  glDrawElements(mode, count, type, indices);
}

void glDrawArrays_wrapper(GLenum mode, GLint first, GLsizei count)
{
  gl_draw_count++;
  if (gl_draw_count <= 5 || (gl_draw_count % 500 == 0))
    debugPrintf("glDrawArrays: mode=0x%x first=%d count=%d (call #%d)\n", mode, first, count, gl_draw_count);
  glDrawArrays(mode, first, count);
}

GLuint glCreateShader_wrapper(GLenum type)
{
  gl_shader_count++;
  GLuint s = glCreateShader(type);
  debugPrintf("glCreateShader: type=0x%x -> %u (call #%d)\n", type, s, gl_shader_count);
  return s;
}

void glShaderSource_wrapper(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
{
  if (count > 0 && string && string[0])
  {
    int len = length ? length[0] : (int)strlen(string[0]);
    debugPrintf("glShaderSource: shader=%u len=%d first80=\"%.80s\"\n", shader, len, string[0]);
  }
  glShaderSource(shader, count, string, length);
}

EGLBoolean eglSwapBuffers_wrapper(EGLDisplay dpy, EGLSurface surface)
{
  EGLBoolean ok;

  egl_call_lock();
  if (g_egl_swap_count < 5 || (g_egl_swap_count % 300 == 0))
    debugPrintf("eglSwapBuffers_wrapper: swap #%d\n", g_egl_swap_count);
  g_egl_swap_count++;
  ok = eglSwapBuffers(g_egl_display, g_egl_surface);
  egl_call_unlock();
  return ok;
}

int diag_get_egl_swap_count(void)
{
  return g_egl_swap_count;
}

EGLBoolean eglQuerySurface_wrapper(EGLDisplay dpy, EGLSurface surface,
                                   EGLint attribute, EGLint *value)
{
  // Workaround for devkitPro mesa bug: eglQuerySurface returns 0 for
  // EGL_WIDTH/EGL_HEIGHT on Switch.  Return our known screen dimensions.
  if (attribute == EGL_WIDTH)
  {
    if (value)
      *value = screen_width;
    debugPrintf("eglQuerySurface_wrapper: EGL_WIDTH -> %d\n", screen_width);
    return EGL_TRUE;
  }
  if (attribute == EGL_HEIGHT)
  {
    if (value)
      *value = screen_height;
    debugPrintf("eglQuerySurface_wrapper: EGL_HEIGHT -> %d\n", screen_height);
    return EGL_TRUE;
  }
  // For other attributes, fall through to the real implementation
  return eglQuerySurface(g_egl_display, g_egl_surface, attribute, value);
}

EGLBoolean eglDestroySurface_wrapper(EGLDisplay dpy, EGLSurface surface)
{
  // Don't actually destroy our surface — the game may "destroy" and "recreate"
  // the surface during lifecycle events. We keep the single surface alive.
  debugPrintf("eglDestroySurface_wrapper: ignoring (keeping pre-initialized surface)\n");
  return EGL_TRUE;
}

// eglGetProcAddress wrapper: intercept known EGL/GL function names so that the
// game cannot bypass our wrappers by calling eglGetProcAddress at runtime.
void *eglGetProcAddress_wrapper(const char *procname)
{
  if (!procname)
    return NULL;

  // EGL functions that we wrap
  if (strcmp(procname, "eglGetDisplay") == 0)
    return (void *)eglGetDisplay_wrapper;
  if (strcmp(procname, "eglInitialize") == 0)
    return (void *)eglInitialize_wrapper;
  if (strcmp(procname, "eglChooseConfig") == 0)
    return (void *)eglChooseConfig_wrapper;
  if (strcmp(procname, "eglCreateWindowSurface") == 0)
    return (void *)eglCreateWindowSurface_wrapper;
  if (strcmp(procname, "eglCreateContext") == 0)
    return (void *)eglCreateContext_wrapper;
  if (strcmp(procname, "eglMakeCurrent") == 0)
    return (void *)eglMakeCurrent_wrapper;
  if (strcmp(procname, "eglSwapBuffers") == 0)
    return (void *)eglSwapBuffers_wrapper;
  if (strcmp(procname, "eglDestroySurface") == 0)
    return (void *)eglDestroySurface_wrapper;
  if (strcmp(procname, "eglQuerySurface") == 0)
    return (void *)eglQuerySurface_wrapper;

  // Fall through to real eglGetProcAddress for GL extensions etc.
  void *ptr = (void *)eglGetProcAddress(procname);
  debugPrintf("eglGetProcAddress_wrapper(\"%s\") -> %p\n", procname, ptr);
  return ptr;
}

// ============================================================================
// ANativeWindow wrappers for Switch
// Return the Switch NWindow* so the game's renderer thread sees a valid window.
// ============================================================================

void *ANativeWindow_fromSurface_wrapper(void *env, void *surface)
{
  NWindow *win = nwindowGetDefault();
  debugPrintf("ANativeWindow_fromSurface_wrapper: returning NWindow %p\n", win);
  return (void *)win;
}

int ANativeWindow_getWidth_wrapper(void *window)
{
  return screen_width;
}

int ANativeWindow_getHeight_wrapper(void *window)
{
  return screen_height;
}

int ANativeWindow_setBuffersGeometry_wrapper(void *window, int width, int height, int format)
{
  debugPrintf("ANativeWindow_setBuffersGeometry_wrapper: %dx%d fmt=%d (ignored)\n", width, height, format);
  return 0; // success
}

// Emulated TLS (__emutls) - GCC ABI for thread_local variables.
// Control block layout (GCC ABI): { uint32 size, uint32 align, uint64 index, void *templ }
// Each thread gets a dynamically-growing array of TLS variable pointers,
// indexed by the control block's pre-assigned index field.
#include <pthread.h>

typedef struct
{
  unsigned int size;  // offset 0: size of the TLS variable
  unsigned int align; // offset 4: alignment (0 = default)
  uintptr_t index;    // offset 8: 1-based index into per-thread array
  void *templ;        // offset 16: initial value template (NULL = zero-init)
} __emutls_control;

typedef struct
{
  uintptr_t capacity;
  void **slots;
} emutls_thread_array;

static pthread_key_t emutls_pthread_key;
static int emutls_key_created = 0;
static pthread_mutex_t emutls_mutex = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t emutls_max_index = 0;

static void emutls_thread_destructor(void *ptr)
{
  emutls_thread_array *arr = (emutls_thread_array *)ptr;
  if (arr)
  {
    for (uintptr_t i = 0; i < arr->capacity; i++)
      free(arr->slots[i]);
    free(arr->slots);
    free(arr);
  }
}

static void *__emutls_get_address_stub(void *control_raw)
{
  __emutls_control *ctrl = (__emutls_control *)control_raw;

  // Create the global pthread key on first call
  if (!emutls_key_created)
  {
    pthread_mutex_lock(&emutls_mutex);
    if (!emutls_key_created)
    {
      pthread_key_create(&emutls_pthread_key, emutls_thread_destructor);
      emutls_key_created = 1;
    }
    pthread_mutex_unlock(&emutls_mutex);
  }

  // Assign index if not yet set (should already be set in the binary)
  uintptr_t index = ctrl->index;
  if (index == 0)
  {
    pthread_mutex_lock(&emutls_mutex);
    index = ctrl->index;
    if (index == 0)
    {
      index = ++emutls_max_index;
      ctrl->index = index;
    }
    pthread_mutex_unlock(&emutls_mutex);
  }
  if (index > emutls_max_index)
  {
    pthread_mutex_lock(&emutls_mutex);
    if (index > emutls_max_index)
      emutls_max_index = index;
    pthread_mutex_unlock(&emutls_mutex);
  }

  // Get or create this thread's TLS array
  emutls_thread_array *arr = (emutls_thread_array *)pthread_getspecific(emutls_pthread_key);
  if (!arr || arr->capacity < index)
  {
    uintptr_t new_cap = index + 32;
    if (!arr)
    {
      arr = (emutls_thread_array *)calloc(1, sizeof(emutls_thread_array));
    }
    void **new_slots = (void **)calloc(new_cap, sizeof(void *));
    if (arr->slots)
    {
      memcpy(new_slots, arr->slots, arr->capacity * sizeof(void *));
      free(arr->slots);
    }
    arr->slots = new_slots;
    arr->capacity = new_cap;
    pthread_setspecific(emutls_pthread_key, arr);
  }

  // Get or allocate the TLS variable (index is 1-based)
  void *ptr = arr->slots[index - 1];
  if (!ptr)
  {
    size_t size = ctrl->size;
    if (size == 0)
      size = sizeof(void *);
    ptr = calloc(1, size);
    if (ctrl->templ && size > 0)
      memcpy(ptr, ctrl->templ, size);
    arr->slots[index - 1] = ptr;
  }
  return ptr;
}

static char *__ctype_ = (char *)&_ctype_;

// this is supposed to be an array of FILEs, which have a different size in libGame
// instead use it to determine whether it's trying to print to stdout/stderr
uint8_t fake_sF[3][0x100]; // stdout, stderr, stdin

static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

FILE *stderr_fake = (FILE *)0x1337;
FILE *stdin_fake = (FILE *)0x1338;

// Math stubs
void sincosf(float x, float *s, float *c)
{
  *s = sinf(x);
  *c = cosf(x);
}

void sincos(double x, double *s, double *c)
{
  *s = sin(x);
  *c = cos(x);
}

// Sched stub
int sched_get_priority_max(int policy)
{
  return 0;
}

// Bully-specific global symbols that libGame.so imports
static int ARCHIVE_SOUND_BANK_val = 0;
static int ARCHIVE_SOUND_NAME_val = 0;
static int MI_ITEM_FLOWERBUND_val = 0;

// Android fortified libc wrappers (extra flag/size params ignored)
int __vsprintf_chk_wrapper(char *s, int flag, size_t slen, const char *fmt, va_list ap)
{
  debugPrintf("__vsprintf_chk: %s\n", fmt);
  return vsprintf(s, fmt, ap);
}
int __vsnprintf_chk_wrapper(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list ap)
{
  debugPrintf("__vsnprintf_chk: %s\n", fmt);
  return vsnprintf(s, maxlen, fmt, ap);
}

void __assert2(const char *file, int line, const char *func, const char *expr)
{
  debugPrintf("assertion failed:\n%s:%d (%s): %s\n", file, line, func, expr);
  assert(0);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
#ifdef DEBUG_LOG
  va_list list;
  static char string[0x1000];

  va_start(list, fmt);
  debugPrintf("__android_log_print: tag=%s, fmt=%s\n", tag, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);

  debugPrintf("%s: %s\n", tag, string);
#endif
  return 0;
}

int fake_fprintf(FILE *stream, const char *fmt, ...)
{
  int ret = 0;
#ifdef DEBUG_LOG
  va_list list;
  static char string[0x1000];

  va_start(list, fmt);
  debugPrintf("fake_fprintf: %s\n", fmt);
  ret = vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);

  debugPrintf("%s", string);
#endif
  return ret;
}

// pthread stuff
// have to wrap it since struct sizes are different

int pthread_mutexattr_init_fake(int *attr)
{
  if (!attr)
    return -1;
  *attr = 0;
  return 0;
}

int pthread_mutexattr_settype_fake(int *attr, int type)
{
  if (!attr)
    return -1;

  // Android bionic encodes recursive mutex type as 1.
  if (type == 1)
    *attr = 1;
#ifdef PTHREAD_MUTEX_RECURSIVE
  else if (type == PTHREAD_MUTEX_RECURSIVE)
    *attr = 1;
#endif
  else
    *attr = 0;

  return 0;
}

int pthread_mutexattr_destroy_fake(int *attr)
{
  (void)attr;
  return 0;
}

static int pthread_mutex_attr_is_recursive(const int *mutexattr)
{
  int attr_val = mutexattr ? *mutexattr : 0;
  int recursive = (attr_val == 1 || attr_val == 0x4000);
#ifdef PTHREAD_MUTEX_RECURSIVE
  if (attr_val == PTHREAD_MUTEX_RECURSIVE)
    recursive = 1;
#endif
  return recursive;
}

static pthread_mutex_t *pthread_mutex_create_host(const int *mutexattr)
{
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m)
    return NULL;

  int recursive = pthread_mutex_attr_is_recursive(mutexattr);

  pthread_mutexattr_t host_attr;
  pthread_mutexattr_init(&host_attr);
  if (recursive)
    pthread_mutexattr_settype(&host_attr, PTHREAD_MUTEX_RECURSIVE);

  int ret = pthread_mutex_init(m, &host_attr);
  pthread_mutexattr_destroy(&host_attr);
  if (ret < 0)
  {
    free(m);
    return NULL;
  }

  return m;
}

static pthread_mutex_t *pthread_mutex_ensure_fake(pthread_mutex_t **uid, const int *mutexattr)
{
  int recursive = pthread_mutex_attr_is_recursive(mutexattr);

  if (!uid)
    return NULL;

  while (1)
  {
    pthread_mutex_t *cur = __atomic_load_n(uid, __ATOMIC_ACQUIRE);
    if (cur && (uintptr_t)cur != 0x4000)
      return cur;

    int init_attr = (recursive || (uintptr_t)cur == 0x4000) ? 1 : 0;
    pthread_mutex_t *created = pthread_mutex_create_host(init_attr ? &init_attr : NULL);
    if (!created)
      return NULL;

    pthread_mutex_t *expected = cur;
    if (__atomic_compare_exchange_n(uid, &expected, created, 0,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
      return created;

    pthread_mutex_destroy(created);
    free(created);

    if (expected && (uintptr_t)expected != 0x4000)
      return expected;
  }
}

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr)
{
  if (!pthread_mutex_ensure_fake(uid, mutexattr))
    return -1;
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid)
{
  if (uid)
  {
    pthread_mutex_t *m = __atomic_exchange_n(uid, NULL, __ATOMIC_ACQ_REL);
    if (m && (uintptr_t)m > 0x8000)
    {
      pthread_mutex_destroy(m);
      free(m);
    }
  }
  return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid)
{
  pthread_mutex_t *m = pthread_mutex_ensure_fake(uid, NULL);
  if (!m)
    return -1;
  return pthread_mutex_lock(m);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid)
{
  pthread_mutex_t *m = pthread_mutex_ensure_fake(uid, NULL);
  if (!m)
    return -1;
  return pthread_mutex_trylock(m);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid)
{
  pthread_mutex_t *m = pthread_mutex_ensure_fake(uid, NULL);
  if (!m)
    return -1;
  return pthread_mutex_unlock(m);
}

static pthread_cond_t *pthread_cond_create_host(void)
{
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c)
    return NULL;

  *c = PTHREAD_COND_INITIALIZER;

  int ret = pthread_cond_init(c, NULL);
  if (ret < 0)
  {
    free(c);
    return NULL;
  }

  return c;
}

static pthread_cond_t *pthread_cond_ensure_fake(pthread_cond_t **cnd)
{
  if (!cnd)
    return NULL;

  while (1)
  {
    pthread_cond_t *cur = __atomic_load_n(cnd, __ATOMIC_ACQUIRE);
    if (cur)
      return cur;

    pthread_cond_t *created = pthread_cond_create_host();
    if (!created)
      return NULL;

    pthread_cond_t *expected = NULL;
    if (__atomic_compare_exchange_n(cnd, &expected, created, 0,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
      return created;

    pthread_cond_destroy(created);
    free(created);

    if (expected)
      return expected;
  }
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr)
{
  (void)condattr;
  if (!pthread_cond_ensure_fake(cnd))
    return -1;
  return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd)
{
  pthread_cond_t *c = pthread_cond_ensure_fake(cnd);
  if (!c)
    return -1;
  return pthread_cond_broadcast(c);
}

static volatile int g_cond_signal_count = 0;
int pthread_cond_signal_fake(pthread_cond_t **cnd)
{
  pthread_cond_t *c = pthread_cond_ensure_fake(cnd);
  if (!c)
    return -1;
  int n = __atomic_add_fetch(&g_cond_signal_count, 1, __ATOMIC_RELAXED);
  if (n <= 20 || (n % 200 == 0))
    debugPrintf("pthread_cond_signal: cnd=%p (call #%d)\n", (void *)c, n);
  return pthread_cond_signal(c);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd)
{
  if (cnd)
  {
    pthread_cond_t *c = __atomic_exchange_n(cnd, NULL, __ATOMIC_ACQ_REL);
    if (c)
    {
      pthread_cond_destroy(c);
      free(c);
    }
  }
  return 0;
}

static volatile int g_cond_wait_count = 0;
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx)
{
  pthread_cond_t *c = pthread_cond_ensure_fake(cnd);
  pthread_mutex_t *m = pthread_mutex_ensure_fake(mtx, NULL);
  if (!c || !m)
    return -1;
  int n = __atomic_add_fetch(&g_cond_wait_count, 1, __ATOMIC_RELAXED);
  if (n <= 20 || (n % 200 == 0))
    debugPrintf("pthread_cond_wait: cnd=%p mtx=%p (call #%d)\n", (void *)c, (void *)m, n);
  return pthread_cond_wait(c, m);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t)
{
  pthread_cond_t *c = pthread_cond_ensure_fake(cnd);
  pthread_mutex_t *m = pthread_mutex_ensure_fake(mtx, NULL);
  if (!c || !m)
    return -1;
  debugPrintf("pthread_cond_timedwait: cnd=%p mtx=%p\n", (void *)c, (void *)m);
  return pthread_cond_timedwait(c, m, t);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void))
{
  if (!once_control || !init_routine)
    return -1;

  while (1)
  {
    int state = __atomic_load_n((int *)once_control, __ATOMIC_ACQUIRE);
    if (state == 2)
      return 0;

    if (state == 0)
    {
      int expected = 0;
      if (__atomic_compare_exchange_n((int *)once_control, &expected, 1, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      {
        (*init_routine)();
        __atomic_store_n((int *)once_control, 2, __ATOMIC_RELEASE);
        return 0;
      }
      continue;
    }

    sched_yield();
  }

  return 0;
}

// Thread wrapper: each game thread needs its own TPIDR_EL0 buffer for stack guard canary
typedef struct
{
  void *(*func)(void *);
  void *arg;
} PthreadWrapperData;

void *thread_guard_run_ptr(void *(*func)(void *), void *arg);

static void *pthread_entry_wrapper(void *data)
{
  PthreadWrapperData *pd = (PthreadWrapperData *)data;
  void *(*func)(void *) = pd->func;
  void *arg = pd->arg;
  free(pd);

  uint8_t *tls = calloc(1, 0x100);
  armSetTlsRw(tls);

  return thread_guard_run_ptr(func, arg);
}

// pthread_t is an unsigned int, so it should be fine
// TODO: probably shouldn't assume default attributes
int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg)
{
  PthreadWrapperData *pd = malloc(sizeof(PthreadWrapperData));
  pd->func = (void *(*)(void *))entry;
  pd->arg = arg;
  return pthread_create(thread, NULL, pthread_entry_wrapper, pd);
}

// GL stuff

void glGetShaderInfoLogHook(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog)
{
  glGetShaderInfoLog(shader, maxLength, length, infoLog);
  debugPrintf("shader info log:\n%s\n", infoLog);
}

void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data)
{
  // don't upload mips
  if (level == 0)
    glCompressedTexImage2D(target, level, format, width, height, border, imageSize, data);
}

void glTexParameteriHook(GLenum target, GLenum param, GLint val)
{
  // force trilinear filtering instead of bilinear+nearest mipmap
  if (val == GL_LINEAR_MIPMAP_NEAREST)
    val = GL_LINEAR_MIPMAP_LINEAR;
  glTexParameteri(target, param, val);
}

// fopen wrapper to trace all file opens from the game
static int fopen_count = 0;
FILE *fopen_wrapper(const char *path, const char *mode)
{
  FILE *f = fopen(path, mode);
  if (!f && path && mode && mode[0] == 'r')
    f = zip_fs_fopen(path);
  fopen_count++;
  debugPrintf("fopen: \"%s\" mode=%s -> %p (#%d)\n", path ? path : "(null)", mode ? mode : "?", f, fopen_count);
  return f;
}

static int open_count = 0;
int open_wrapper(const char *path, int flags, ...)
{
  mode_t mode = 0;
  int fd;

  if (flags & O_CREAT)
  {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
    fd = open(path, flags, mode);
  }
  else
  {
    fd = open(path, flags);
  }

  if (fd < 0 && !(flags & (O_WRONLY | O_RDWR)))
    fd = zip_fs_open(path, flags);

  open_count++;
  if (fd < 0)
  {
    debugPrintf("open FAILED: \"%s\" flags=0x%x err=%d (#%d)\n",
                path ? path : "(null)", flags, errno, open_count);
  }
  else if (open_count <= 30 || (open_count % 200 == 0))
  {
    debugPrintf("open: \"%s\" flags=0x%x -> fd=%d (#%d)\n",
                path ? path : "(null)", flags, fd, open_count);
  }
  return fd;
}

// sem_wait/sem_post wrappers for tracing
static volatile int g_sem_wait_count = 0;
static volatile int g_sem_post_count = 0;

int sem_wait_wrapper(sem_t *sem)
{
  int n = __atomic_add_fetch(&g_sem_wait_count, 1, __ATOMIC_RELAXED);
  if (n <= 20 || (n % 200 == 0))
    debugPrintf("sem_wait: sem=%p (call #%d)\n", (void *)sem, n);
  return sem_wait(sem);
}

int sem_post_wrapper(sem_t *sem)
{
  int n = __atomic_add_fetch(&g_sem_post_count, 1, __ATOMIC_RELAXED);
  if (n <= 20 || (n % 200 == 0))
    debugPrintf("sem_post: sem=%p (call #%d)\n", (void *)sem, n);
  return sem_post(sem);
}

// syscall wrapper — the game imports syscall() for futex and other Linux syscalls.
// Returning 0 for futex(FUTEX_WAIT) breaks thread synchronization because it
// succeeds immediately instead of blocking. Log all syscall numbers to diagnose.
static volatile int g_syscall_count = 0;
long syscall_wrapper(long number, ...)
{
  int n = __atomic_add_fetch(&g_syscall_count, 1, __ATOMIC_RELAXED);
  if (n <= 50 || (n % 500 == 0))
    debugPrintf("syscall: nr=%ld (call #%d)\n", number, n);
  errno = ENOSYS;
  return -1;
}

// Diagnostic counters accessible from other files
int diag_get_cond_wait_count(void) { return g_cond_wait_count; }
int diag_get_cond_signal_count(void) { return g_cond_signal_count; }
int diag_get_sem_wait_count(void) { return g_sem_wait_count; }
int diag_get_sem_post_count(void) { return g_sem_post_count; }
int diag_get_syscall_count(void) { return g_syscall_count; }

// import table

DynLibFunction dynlib_functions[] = {
    {"__sF", (uintptr_t)&fake_sF},
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},

    {"stderr", (uintptr_t)&stderr_fake},

    // C++ ABI runtime
    {"__cxa_allocate_exception", (uintptr_t)&__cxa_allocate_exception},
    {"__cxa_begin_catch", (uintptr_t)&__cxa_begin_catch},
    {"__cxa_end_catch", (uintptr_t)&__cxa_end_catch},
    {"__cxa_free_exception", (uintptr_t)&__cxa_free_exception},
    {"__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire},
    {"__cxa_guard_release", (uintptr_t)&__cxa_guard_release},
    {"__cxa_guard_abort", (uintptr_t)&__cxa_guard_abort},
    {"__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual},
    {"__cxa_rethrow", (uintptr_t)&__cxa_rethrow},
    {"__cxa_throw", (uintptr_t)&__cxa_throw_wrapper},
    {"__cxa_thread_atexit", (uintptr_t)&__cxa_thread_atexit},
    {"__dynamic_cast", (uintptr_t)&__dynamic_cast},
    {"__gxx_personality_v0", (uintptr_t)&__gxx_personality_v0},
    {"__emutls_get_address", (uintptr_t)&__emutls_get_address_stub},

    // operator new / delete
    {"_Znwm", (uintptr_t)&malloc},
    {"_Znam", (uintptr_t)&malloc},
    {"_ZdlPv", (uintptr_t)&free_wrapper},
    {"_ZdaPv", (uintptr_t)&free_wrapper},

    // C++ terminate
    {"_ZSt9terminatev", (uintptr_t)&terminate_wrapper},
    {"_ZNSt9exceptionD2Ev", (uintptr_t)&_ZNSt9exceptionD2Ev},
    {"_ZNSt6__ndk113basic_istreamIcNS_11char_traitsIcEEE4readEPcl", (uintptr_t)&ndk_istream_read_wrapper},
    {"_ZNSt6__ndk113basic_istreamIcNS_11char_traitsIcEEE5seekgENS_4fposI9mbstate_tEE", (uintptr_t)&ndk_istream_seekg_wrapper},
    {"_ZNSt6__ndk113basic_istreamIcNS_11char_traitsIcEEE5tellgEv", (uintptr_t)&ndk_istream_tellg_wrapper},
    {"_ZNSt6__ndk18ios_base5clearEj", (uintptr_t)&ndk_ios_base_clear_wrapper},
    {"_ZNSt6__ndk19basic_iosIcNS_11char_traitsIcEEED2Ev", (uintptr_t)&ndk_basic_ios_dtor_wrapper},
    {"_ZNSt6__ndk15mutex4lockEv", (uintptr_t)&ndk_mutex_lock_wrapper},
    {"_ZNSt6__ndk15mutex6unlockEv", (uintptr_t)&ndk_mutex_unlock_wrapper},
    {"_ZNSt6__ndk15mutex8try_lockEv", (uintptr_t)&ndk_mutex_trylock_wrapper},
    {"_ZNSt6__ndk15mutexD1Ev", (uintptr_t)&ndk_mutex_dtor_wrapper},
    {"_ZNSt6__ndk15mutexD2Ev", (uintptr_t)&ndk_mutex_dtor_wrapper},

    // Bully-specific global symbols
    {"ARCHIVE_SOUND_BANK", (uintptr_t)&ARCHIVE_SOUND_BANK_val},
    {"ARCHIVE_SOUND_NAME", (uintptr_t)&ARCHIVE_SOUND_NAME_val},
    {"MI_ITEM_FLOWERBUND", (uintptr_t)&MI_ITEM_FLOWERBUND_val},

    // AAsset (Android asset API) — filesystem-backed wrappers reading from assets/
    {"AAssetManager_open", (uintptr_t)&AAssetManager_open_wrapper},
    {"AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_wrapper},
    {"AAsset_close", (uintptr_t)&AAsset_close_wrapper},
    {"AAsset_getLength", (uintptr_t)&AAsset_getLength_wrapper},
    {"AAsset_getLength64", (uintptr_t)&AAsset_getLength64_wrapper},
    {"AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength_wrapper},
    {"AAsset_getRemainingLength64", (uintptr_t)&AAsset_getRemainingLength64_wrapper},
    {"AAsset_read", (uintptr_t)&AAsset_read_wrapper},
    {"AAsset_seek", (uintptr_t)&AAsset_seek_wrapper},
    {"AAsset_seek64", (uintptr_t)&AAsset_seek64_wrapper},

    // ANativeWindow wrappers (return Switch NWindow instead of NULL)
    {"ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_wrapper},
    {"ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight_wrapper},
    {"ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth_wrapper},
    {"ANativeWindow_release", (uintptr_t)&ret0},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_wrapper},

    // pthread TLS - must use real implementations! The game's render thread
    // stores per-thread state (EGL context, thread names, etc.) via TLS.
    // Stubbing these as ret0 causes the render thread to crash.
    {"pthread_key_create", (uintptr_t)&pthread_key_create},
    {"pthread_key_delete", (uintptr_t)&pthread_key_delete},

    {"pthread_getspecific", (uintptr_t)&pthread_getspecific},
    {"pthread_setspecific", (uintptr_t)&pthread_setspecific},

    {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
    {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
    {"pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},
    {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},
    {"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},
    {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},

    {"pthread_create", (uintptr_t)&pthread_create_fake},
    {"pthread_join", (uintptr_t)&pthread_join},
    {"pthread_self", (uintptr_t)&pthread_self},

    {"pthread_setschedparam", (uintptr_t)&ret0},

    {"pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake},
    {"pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake},
    {"pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_fake},
    {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},
    {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},
    {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},
    {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},

    {"pthread_once", (uintptr_t)&pthread_once_fake},

    {"sched_get_priority_min", (uintptr_t)&retm1},

    {"__android_log_print", (uintptr_t)__android_log_print},

    {"__errno", (uintptr_t)&__errno},

    {"__stack_chk_fail", (uintptr_t)__stack_chk_fail_nop},
    // freezes with real __stack_chk_guard
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},

    {"_ctype_", (uintptr_t)&__ctype_},

    // TODO: use math neon?
    {"acos", (uintptr_t)&acos},
    {"acosf", (uintptr_t)&acosf},
    {"asinf", (uintptr_t)&asinf},
    {"atan2f", (uintptr_t)&atan2f},
    {"atanf", (uintptr_t)&atanf},
    {"cos", (uintptr_t)&cos},
    {"cosf", (uintptr_t)&cosf},
    {"exp", (uintptr_t)&exp},
    {"floor", (uintptr_t)&floor},
    {"floorf", (uintptr_t)&floorf},
    {"fmod", (uintptr_t)&fmod},
    {"fmodf", (uintptr_t)&fmodf},
    {"log", (uintptr_t)&log},
    {"log10f", (uintptr_t)&log10f},
    {"pow", (uintptr_t)&pow},
    {"powf", (uintptr_t)&powf},
    {"sin", (uintptr_t)&sin},
    {"sinf", (uintptr_t)&sinf},
    {"tan", (uintptr_t)&tan},
    {"tanf", (uintptr_t)&tanf},
    {"sqrt", (uintptr_t)&sqrt},
    {"sqrtf", (uintptr_t)&sqrtf},
    {"cbrt", (uintptr_t)&cbrt},
    {"ceilf", (uintptr_t)&ceilf},
    {"ldexp", (uintptr_t)&ldexp},
    {"logf", (uintptr_t)&logf},

    {"atoi", (uintptr_t)&atoi},
    {"atof", (uintptr_t)&atof},
    {"isspace", (uintptr_t)&isspace},
    {"tolower", (uintptr_t)&tolower},
    {"towlower", (uintptr_t)&towlower},
    {"toupper", (uintptr_t)&toupper},
    {"towupper", (uintptr_t)&towupper},

    {"calloc", (uintptr_t)&calloc},
    {"free", (uintptr_t)&free_wrapper},
    {"malloc", (uintptr_t)&malloc},
    {"realloc", (uintptr_t)&realloc_wrapper},
    {"memalign", (uintptr_t)&memalign_wrapper},

    {"clock_gettime", (uintptr_t)&clock_gettime},
    {"gettimeofday", (uintptr_t)&gettimeofday},
    {"time", (uintptr_t)&time},
    {"asctime", (uintptr_t)&asctime},
    {"localtime", (uintptr_t)&localtime},
    {"localtime_r", (uintptr_t)&localtime_r},
    {"strftime", (uintptr_t)&strftime},

    {"eglGetProcAddress", (uintptr_t)&eglGetProcAddress_wrapper},
    {"eglGetDisplay", (uintptr_t)&eglGetDisplay_wrapper},
    {"eglQueryString", (uintptr_t)&eglQueryString},
    {"eglQuerySurface", (uintptr_t)&eglQuerySurface_wrapper},
    {"eglChooseConfig", (uintptr_t)&eglChooseConfig_wrapper},
    {"eglCreateContext", (uintptr_t)&eglCreateContext_wrapper},
    {"eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface_wrapper},
    {"eglDestroySurface", (uintptr_t)&eglDestroySurface_wrapper},
    {"eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib},
    {"eglGetError", (uintptr_t)&eglGetError},
    {"eglInitialize", (uintptr_t)&eglInitialize_wrapper},
    {"eglMakeCurrent", (uintptr_t)&eglMakeCurrent_wrapper},
    {"eglSwapBuffers", (uintptr_t)&eglSwapBuffers_wrapper},
    {"eglSwapInterval", (uintptr_t)&eglSwapInterval},

    {"abort", (uintptr_t)&abort},
    {"exit", (uintptr_t)&exit},

    {"fopen", (uintptr_t)&fopen_wrapper},
    {"fclose", (uintptr_t)&fclose},
    {"fdopen", (uintptr_t)&fdopen},
    {"fflush", (uintptr_t)&fflush},
    {"fgetc", (uintptr_t)&fgetc},
    {"fgets", (uintptr_t)&fgets},
    {"fputs", (uintptr_t)&fputs},
    {"fputc", (uintptr_t)&fputc},
    {"fprintf", (uintptr_t)&fprintf},
    {"fread", (uintptr_t)&fread},
    {"fseek", (uintptr_t)&fseek},
    {"ftell", (uintptr_t)&ftell},
    {"fwrite", (uintptr_t)&fwrite},
    {"fstat", (uintptr_t)&fstat},
    {"ferror", (uintptr_t)&ferror},
    {"feof", (uintptr_t)&feof},
    {"setvbuf", (uintptr_t)&setvbuf},

    {"getenv", (uintptr_t)&getenv},

    {"glActiveTexture", (uintptr_t)&glActiveTexture},
    {"glAttachShader", (uintptr_t)&glAttachShader},
    {"glBindAttribLocation", (uintptr_t)&glBindAttribLocation},
    {"glBindBuffer", (uintptr_t)&glBindBuffer},
    {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer},
    {"glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},
    {"glBindTexture", (uintptr_t)&glBindTexture},
    {"glBlendFunc", (uintptr_t)&glBlendFunc},
    {"glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate},
    {"glBufferData", (uintptr_t)&glBufferData},
    {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},
    {"glClear", (uintptr_t)&glClear_wrapper},
    {"glClearColor", (uintptr_t)&glClearColor},
    {"glClearDepthf", (uintptr_t)&glClearDepthf},
    {"glClearStencil", (uintptr_t)&glClearStencil},
    {"glCompileShader", (uintptr_t)&glCompileShader},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},
    {"glCreateProgram", (uintptr_t)&glCreateProgram},
    {"glCreateShader", (uintptr_t)&glCreateShader_wrapper},
    {"glCullFace", (uintptr_t)&glCullFace},
    {"glDeleteBuffers", (uintptr_t)&glDeleteBuffers},
    {"glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers},
    {"glDeleteProgram", (uintptr_t)&glDeleteProgram},
    {"glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers},
    {"glDeleteShader", (uintptr_t)&glDeleteShader},
    {"glDeleteTextures", (uintptr_t)&glDeleteTextures},
    {"glDepthFunc", (uintptr_t)&glDepthFunc},
    {"glDepthMask", (uintptr_t)&glDepthMask},
    {"glDepthRangef", (uintptr_t)&glDepthRangef},
    {"glDisable", (uintptr_t)&glDisable},
    {"glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray},
    {"glDrawArrays", (uintptr_t)&glDrawArrays_wrapper},
    {"glDrawElements", (uintptr_t)&glDrawElements_wrapper},
    {"glEnable", (uintptr_t)&glEnable},
    {"glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},
    {"glFinish", (uintptr_t)&glFinish},
    {"glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer},
    {"glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D},
    {"glFrontFace", (uintptr_t)&glFrontFace},
    {"glGenBuffers", (uintptr_t)&glGenBuffers},
    {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers},
    {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},
    {"glGenTextures", (uintptr_t)&glGenTextures},
    {"glGetAttribLocation", (uintptr_t)&glGetAttribLocation},
    {"glGetError", (uintptr_t)&glGetError},
    {"glGetBooleanv", (uintptr_t)&glGetBooleanv},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv},
    {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},
    {"glGetProgramiv", (uintptr_t)&glGetProgramiv},
    {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLogHook},
    {"glGetShaderiv", (uintptr_t)&glGetShaderiv},
    {"glGetString", (uintptr_t)&glGetString},
    {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},
    {"glHint", (uintptr_t)&glHint},
    {"glLinkProgram", (uintptr_t)&glLinkProgram},
    {"glPolygonOffset", (uintptr_t)&glPolygonOffset},
    {"glReadPixels", (uintptr_t)&glReadPixels},
    {"glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage},
    {"glScissor", (uintptr_t)&glScissor},
    {"glShaderSource", (uintptr_t)&glShaderSource_wrapper},
    {"glTexImage2D", (uintptr_t)&glTexImage2D},
    {"glTexParameterf", (uintptr_t)&glTexParameterf},
    {"glTexParameteri", (uintptr_t)&glTexParameteri},
    {"glUniform1f", (uintptr_t)&glUniform1f},
    {"glUniform1fv", (uintptr_t)&glUniform1fv},
    {"glUniform1i", (uintptr_t)&glUniform1i},
    {"glUniform2fv", (uintptr_t)&glUniform2fv},
    {"glUniform3f", (uintptr_t)&glUniform3f},
    {"glUniform3fv", (uintptr_t)&glUniform3fv},
    {"glUniform4fv", (uintptr_t)&glUniform4fv},
    {"glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv},
    {"glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv},
    {"glUseProgram", (uintptr_t)&glUseProgram},
    {"glVertexAttrib4fv", (uintptr_t)&glVertexAttrib4fv},
    {"glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},
    {"glViewport", (uintptr_t)&glViewport_wrapper},

    // GLES3 functions that Bully uses but Max Payne didn't
    {"glBindVertexArray", (uintptr_t)&glBindVertexArray},
    {"glDeleteVertexArrays", (uintptr_t)&glDeleteVertexArrays},
    {"glGenVertexArrays", (uintptr_t)&glGenVertexArrays},
    {"glColorMask", (uintptr_t)&glColorMask},
    {"glDrawBuffers", (uintptr_t)&glDrawBuffers},
    {"glLineWidth", (uintptr_t)&glLineWidth},
    {"glPixelStorei", (uintptr_t)&glPixelStorei},
    {"glStencilFunc", (uintptr_t)&glStencilFunc},
    {"glStencilMask", (uintptr_t)&glStencilMask},
    {"glStencilOp", (uintptr_t)&glStencilOp},
    {"glTexStorage2D", (uintptr_t)&glTexStorage2D},
    {"glTexSubImage2D", (uintptr_t)&glTexSubImage2D},
    {"glCompressedTexSubImage2D", (uintptr_t)&glCompressedTexSubImage2D},
    {"glDrawArrays", (uintptr_t)&glDrawArrays_wrapper},
    {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},
    {"glDepthRangef", (uintptr_t)&glDepthRangef},
    {"glHint", (uintptr_t)&glHint},
    {"glBufferData", (uintptr_t)&glBufferData},

    // OpenAL
    {"alBufferData", (uintptr_t)&alBufferDataHook},
    {"alDeleteBuffers", (uintptr_t)&alDeleteBuffersHook},
    {"alDeleteSources", (uintptr_t)&alDeleteSourcesHook},
    {"alGenBuffers", (uintptr_t)&alGenBuffersHook},
    {"alGenSources", (uintptr_t)&alGenSourcesHook},
    {"alGetBufferi", (uintptr_t)&alGetBufferiHook},
    {"alGetError", (uintptr_t)&alGetErrorHook},
    {"alGetSource3f", (uintptr_t)&alGetSource3fHook},
    {"alGetSourcef", (uintptr_t)&alGetSourcefHook},
    {"alGetSourcei", (uintptr_t)&alGetSourceiHook},
    {"alListener3f", (uintptr_t)&alListener3fHook},
    {"alListenerfv", (uintptr_t)&alListenerfvHook},
    {"alSource3f", (uintptr_t)&alSource3fHook},
    {"alSourcePause", (uintptr_t)&alSourcePauseHook},
    {"alSourcePlay", (uintptr_t)&alSourcePlayHook},
    {"alSourceQueueBuffers", (uintptr_t)&alSourceQueueBuffersHook},
    {"alSourceRewind", (uintptr_t)&alSourceRewindHook},
    {"alSourceStop", (uintptr_t)&alSourceStopHook},
    {"alSourceUnqueueBuffers", (uintptr_t)&alSourceUnqueueBuffersHook},
    {"alSourcef", (uintptr_t)&alSourcefHook},
    {"alSourcei", (uintptr_t)&alSourceiHook},
    {"alcCloseDevice", (uintptr_t)&alcCloseDeviceHook},
    {"alcCreateContext", (uintptr_t)&alcCreateContextHook},
    {"alcDestroyContext", (uintptr_t)&alcDestroyContextHook},
    {"alcIsExtensionPresent", (uintptr_t)&alcIsExtensionPresentHook},
    {"alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrentHook},
    {"alcOpenDevice", (uintptr_t)&alcOpenDeviceHook},
    // OpenAL EFX (filters)
    {"alDeleteFilters", (uintptr_t)&alDeleteFiltersHook},
    {"alFilterf", (uintptr_t)&alFilterfHook},
    {"alFilteri", (uintptr_t)&alFilteriHook},
    {"alGenFilters", (uintptr_t)&alGenFiltersHook},
    {"alIsFilter", (uintptr_t)&alIsFilterHook},

    // this only uses setjmp in the JPEG loader but not longjmp
    // probably doesn't matter if they're compatible or not
    {"setjmp", (uintptr_t)&setjmp},

    {"memcmp", (uintptr_t)&memcmp_wrapper},
    {"wmemcmp", (uintptr_t)&wmemcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"memmove", (uintptr_t)&memmove},
    {"memset", (uintptr_t)&memset},
    {"memchr", (uintptr_t)&memchr},

    {"printf", (uintptr_t)&debugPrintf},

    {"bsearch", (uintptr_t)&bsearch},
    {"qsort", (uintptr_t)&qsort},

    {"snprintf", (uintptr_t)&snprintf},
    {"sprintf", (uintptr_t)&sprintf},
    {"vsnprintf", (uintptr_t)&vsnprintf},
    {"vsprintf", (uintptr_t)&vsprintf},

    {"sscanf", (uintptr_t)&sscanf},

    {"close", (uintptr_t)&close},
    {"lseek", (uintptr_t)&lseek},
    {"mkdir", (uintptr_t)&mkdir},
    {"open", (uintptr_t)&open_wrapper},
    {"read", (uintptr_t)&read},
    {"stat", (uintptr_t)stat},
    {"write", (uintptr_t)&write},

    {"strcasecmp", (uintptr_t)&strcasecmp},
    {"strcat", (uintptr_t)&strcat},
    {"strchr", (uintptr_t)&strchr},
    {"strcmp", (uintptr_t)&strcmp_wrapper},
    {"strcoll", (uintptr_t)&strcoll},
    {"strcpy", (uintptr_t)&strcpy},
    {"stpcpy", (uintptr_t)&stpcpy},
    {"strerror", (uintptr_t)&strerror},
    {"strlen", (uintptr_t)&strlen_wrapper},
    {"strncasecmp", (uintptr_t)&strncasecmp},
    {"strncat", (uintptr_t)&strncat},
    {"strncmp", (uintptr_t)&strncmp_wrapper},
    {"strncpy", (uintptr_t)&strncpy},
    {"strpbrk", (uintptr_t)&strpbrk},
    {"strrchr", (uintptr_t)&strrchr},
    {"strstr", (uintptr_t)&strstr},
    {"strtod", (uintptr_t)&strtod},
    {"strtok", (uintptr_t)&strtok},
    {"strtol", (uintptr_t)&strtol},
    {"strtoul", (uintptr_t)&strtoul},
    {"strtof", (uintptr_t)&strtof},
    {"strxfrm", (uintptr_t)&strxfrm},

    {"srand", (uintptr_t)&srand},
    {"rand", (uintptr_t)&rand},

    {"nanosleep", (uintptr_t)&nanosleep},
    {"usleep", (uintptr_t)&usleep},

    {"wctob", (uintptr_t)&wctob},
    {"wctype", (uintptr_t)&wctype},
    {"wcsxfrm", (uintptr_t)&wcsxfrm},
    {"iswctype", (uintptr_t)&iswctype},
    {"wcscoll", (uintptr_t)&wcscoll},
    {"wcsftime", (uintptr_t)&wcsftime},
    {"mbrtowc", (uintptr_t)&mbrtowc},
    {"wcrtomb", (uintptr_t)&wcrtomb},
    {"wcslen", (uintptr_t)&wcslen},
    {"btowc", (uintptr_t)&btowc},

    // Fortified libc functions (Android _chk variants)
    {"__strlen_chk", (uintptr_t)&__strlen_chk_wrapper},
    {"__memset_chk", (uintptr_t)&memset},
    {"__memmove_chk", (uintptr_t)&memmove},
    {"__memcpy_chk", (uintptr_t)&memcpy},
    {"__strcpy_chk", (uintptr_t)&strcpy},
    {"__strncpy_chk", (uintptr_t)&strncpy},
    {"__strncpy_chk2", (uintptr_t)&strncpy},
    {"__strcat_chk", (uintptr_t)&strcat},
    {"__strrchr_chk", (uintptr_t)&strrchr},
    {"__strchr_chk", (uintptr_t)&strchr},
    {"__vsprintf_chk", (uintptr_t)&__vsprintf_chk_wrapper},
    {"__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_wrapper},
    {"__fread_chk", (uintptr_t)&fread},

    // Additional libc
    {"strcspn", (uintptr_t)&strcspn},
    {"strtok_r", (uintptr_t)&strtok_r},
    {"ungetc", (uintptr_t)&ungetc},
    {"getc", (uintptr_t)&getc},
    {"putchar", (uintptr_t)&putchar},
    {"puts", (uintptr_t)&puts},
    {"unlink", (uintptr_t)&unlink},
    {"rename", (uintptr_t)&rename},
    {"longjmp", (uintptr_t)&longjmp},
    {"stdin", (uintptr_t)&stdin_fake},
    {"getpid", (uintptr_t)&getpid},

    // Math
    {"sincosf", (uintptr_t)&sincosf},
    {"sincos", (uintptr_t)&sincos},
    {"exp2f", (uintptr_t)&exp2f},
    {"expf", (uintptr_t)&expf},

    // POSIX semaphores
    {"sem_init", (uintptr_t)&sem_init},
    {"sem_destroy", (uintptr_t)&sem_destroy},
    {"sem_post", (uintptr_t)&sem_post_wrapper},
    {"sem_wait", (uintptr_t)&sem_wait_wrapper},
    {"sem_trywait", (uintptr_t)&sem_trywait},
    {"sem_getvalue", (uintptr_t)&sem_getvalue},

    // pthread extended
    {"pthread_setname_np", (uintptr_t)&ret0},
    {"pthread_attr_init", (uintptr_t)&pthread_attr_init},
    {"pthread_attr_getstacksize", (uintptr_t)&pthread_attr_getstacksize},
    {"pthread_attr_getschedparam", (uintptr_t)&pthread_attr_getschedparam},
    {"pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam},
    {"pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy},
    {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock},
    {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock},
    {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock},
    {"sched_get_priority_max", (uintptr_t)&sched_get_priority_max},
    {"__register_atfork", (uintptr_t)&ret0},
    {"__cxa_finalize", (uintptr_t)&ret0},

    // Android-specific stubs
    {"gettid", (uintptr_t)&ret0},
    {"syscall", (uintptr_t)&syscall_wrapper},
    {"getauxval", (uintptr_t)&ret0},
    {"__system_property_get", (uintptr_t)&ret0},
    {"dl_iterate_phdr", (uintptr_t)&ret0},

    // Thread-local game globals (harmless stubs)
    {"_ZTH7gString", (uintptr_t)&ret0},
    {"_ZTH8gString2", (uintptr_t)&ret0},
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void)
{
  // only use the hooks if the relevant config options are enabled to avoid possible overhead
  if (config.disable_mipmaps)
    so_find_import(dynlib_functions, dynlib_numfunctions, "glCompressedTexImage2D")->func = (uintptr_t)glCompressedTexImage2DHook;
  if (config.trilinear_filter)
    so_find_import(dynlib_functions, dynlib_numfunctions, "glTexParameteri")->func = (uintptr_t)glTexParameteriHook;
}
