/* cpplib_loader.c -- load libc++_shared.so for __ndk1 symbol resolution
 *
 * Copyright (C) 2026 givethesourceplox
 *
 * This is a minimal ELF loader specifically designed to load
 * libc++_shared.so from the Bully APK. It maps the library into
 * heap memory, resolves its imports against our existing libc/libm,
 * applies relocations, and exports a symbol lookup function.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <elf.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <setjmp.h>
#include <malloc.h>
#include <sys/types.h>

#include "so_util.h"
#include "main.h"
#include "zip_fs.h"

// Forward declaration for vasprintf since it might be hidden by strictly POSIX headers
extern int vasprintf(char **strp, const char *fmt, va_list ap);

#define CPPLIB_ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

// Module state for libc++_shared.so
static void *cpplib_base = NULL;     // heap allocation base
static void *cpplib_virtbase = NULL; // executable virtual mapping
static size_t cpplib_load_size = 0;
static uintptr_t cpplib_min_vaddr = 0;
static Elf64_Sym *cpplib_syms = NULL;
static char *cpplib_dynstrtab = NULL;
static int cpplib_num_syms = 0;

extern void *memalign_wrapper(size_t alignment, size_t size);
extern void *realloc_wrapper(void *ptr, size_t size);
extern void free_wrapper(void *ptr);

// Android-specific stubs
static int __system_property_get_stub(const char *name, char *value)
{
  value[0] = '\0';
  return 0;
}

static void android_set_abort_message_stub(const char *msg)
{
  debugPrintf("abort: %s\n", msg);
}

static int dl_iterate_phdr_stub(void *callback, void *data)
{
  return 0;
}

static unsigned long getauxval_stub(unsigned long type)
{
  return 0;
}

static int __system_property_find_stub(const char *name)
{
  return 0;
}

static void __assert2_stub(const char *file, int line, const char *func, const char *msg)
{
  debugPrintf("assert: %s:%d %s: %s\n", file, line, func, msg);
  abort();
}

// C++ runtime (provided by libsupc++ on Switch)
extern void *__cxa_atexit;
extern void __cxa_finalize(void *);

static size_t __ctype_get_mb_cur_max_stub(void)
{
  return 4; // UTF-8
}

// Locale stubs
static void *newlocale_stub(int mask, const char *locale, void *base)
{
  return (void *)1; // non-NULL placeholder
}

static void freelocale_stub(void *loc) {}

static void *uselocale_stub(void *loc)
{
  return (void *)1;
}

// Bionic-specific functions
static int isdigit_l_stub(int c, void *l) { return isdigit(c); }
static int islower_l_stub(int c, void *l) { return islower(c); }
static int isupper_l_stub(int c, void *l) { return isupper(c); }
static int isxdigit_l_stub(int c, void *l) { return isxdigit(c); }
static int iswlower_l_stub(wint_t c, void *l) { return iswlower(c); }
static int toupper_l_stub(int c, void *l) { return toupper(c); }
static int tolower_l_stub(int c, void *l) { return tolower(c); }
static int iswspace_l_stub(wint_t c, void *l) { return iswspace(c); }
static int iswprint_l_stub(wint_t c, void *l) { return iswprint(c); }
static int iswcntrl_l_stub(wint_t c, void *l) { return iswcntrl(c); }
static int iswupper_l_stub(wint_t c, void *l) { return iswupper(c); }
static int iswalpha_l_stub(wint_t c, void *l) { return iswalpha(c); }
static int iswdigit_l_stub(wint_t c, void *l) { return iswdigit(c); }
static int iswpunct_l_stub(wint_t c, void *l) { return iswpunct(c); }
static int iswxdigit_l_stub(wint_t c, void *l) { return iswxdigit(c); }
static int iswblank_l_stub(wint_t c, void *l) { return iswblank(c); }
static wint_t towupper_l_stub(wint_t c, void *l) { return towupper(c); }
static wint_t towlower_l_stub(wint_t c, void *l) { return towlower(c); }
static size_t strftime_l_stub(char *s, size_t max, const char *format, const struct tm *tm, void *l) { return strftime(s, max, format, tm); }
static int strcoll_l_stub(const char *s1, const char *s2, void *l) { return strcoll(s1, s2); }
static size_t strxfrm_l_stub(char *dest, const char *src, size_t n, void *l) { return strxfrm(dest, src, n); }
static int wcscoll_l_stub(const wchar_t *s1, const wchar_t *s2, void *l) { return wcscoll(s1, s2); }
static size_t wcsxfrm_l_stub(wchar_t *dest, const wchar_t *src, size_t n, void *l) { return wcsxfrm(dest, src, n); }
static long long strtoll_l_stub(const char *nptr, char **endptr, int base, void *l) { return strtoll(nptr, endptr, base); }
static unsigned long long strtoull_l_stub(const char *nptr, char **endptr, int base, void *l) { return strtoull(nptr, endptr, base); }
static long double strtold_l_stub(const char *nptr, char **endptr, void *l) { return strtod(nptr, endptr); }

static int __cxa_thread_atexit_impl_stub(void (*func)(void *), void *arg, void *dso_handle) { return 0; }
static int syscall_stub(int number, ...)
{
  (void)number;
  errno = ENOSYS;
  return -1;
}
static int sysconf_stub(int name) { return 0; }
static long double strtold_stub(const char *nptr, char **endptr) { return strtod(nptr, endptr); }
static long double wcstold_stub(const wchar_t *nptr, wchar_t **endptr) { return wcstod(nptr, endptr); }
static char *realpath_stub(const char *path, char *resolved_path)
{
  char *ret = realpath(path, resolved_path);
  if (!ret)
    debugPrintf("cpplib: realpath FAILED \"%s\" err=%d\n", path ? path : "(null)", errno);
  return ret;
}
static ssize_t sendfile_stub(int out_fd, int in_fd, off_t *offset, size_t count) { return -1; }
static int utimensat_stub(int dirfd, const char *pathname, const struct timespec times[2], int flags) { return -1; }
static int cpplib_open_count = 0;
static int cpplib_open_wrapper(const char *path, int flags, ...)
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

  cpplib_open_count++;
  if (fd < 0)
    debugPrintf("cpplib: open FAILED \"%s\" flags=0x%x err=%d (#%d)\n",
                path ? path : "(null)", flags, errno, cpplib_open_count);
  else if (cpplib_open_count <= 20 || (cpplib_open_count % 200 == 0))
    debugPrintf("cpplib: open \"%s\" flags=0x%x -> fd=%d (#%d)\n",
                path ? path : "(null)", flags, fd, cpplib_open_count);
  return fd;
}

static int cpplib_openat_wrapper(int dirfd, const char *pathname, int flags, ...)
{
  mode_t mode = 0;
  int has_mode = (flags & O_CREAT) != 0;
  if (has_mode)
  {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }

  // Most libc++ paths pass AT_FDCWD; emulate that path on Switch.
  if (dirfd == AT_FDCWD || (pathname && pathname[0] == '/'))
  {
    if (has_mode)
      return cpplib_open_wrapper(pathname, flags, mode);
    return cpplib_open_wrapper(pathname, flags);
  }

  // Relative openat with custom dirfd isn't currently emulated.
  errno = ENOTSUP;
  debugPrintf("cpplib: openat unsupported dirfd=%d path=\"%s\" flags=0x%x\n",
              dirfd, pathname ? pathname : "(null)", flags);
  return -1;
}

static FILE *cpplib_fopen_wrapper(const char *path, const char *mode)
{
  FILE *f = fopen(path, mode);
  if (!f && path && mode && mode[0] == 'r')
    f = zip_fs_fopen(path);
  if (!f)
    debugPrintf("cpplib: fopen FAILED \"%s\" mode=%s err=%d\n",
                path ? path : "(null)", mode ? mode : "?", errno);
  else
  {
    static int cpplib_fopen_count = 0;
    cpplib_fopen_count++;
    if (cpplib_fopen_count <= 20 || (cpplib_fopen_count % 200 == 0))
      debugPrintf("cpplib: fopen \"%s\" mode=%s -> %p (#%d)\n",
                  path ? path : "(null)", mode ? mode : "?", f, cpplib_fopen_count);
  }
  return f;
}
static int unlinkat_stub(int dirfd, const char *pathname, int flags) { return -1; }
static int statvfs_stub(const char *path, void *buf) { return -1; }

static int posix_memalign_stub(void **memptr, size_t alignment, size_t size)
{
  if (!memptr)
    return EINVAL;
  *memptr = memalign_wrapper(alignment, size);
  return *memptr ? 0 : ENOMEM;
}

extern uint8_t fake_sF[3][0x100];
extern void free_wrapper(void *ptr);
extern void *realloc_wrapper(void *ptr, size_t size);

static void syslog_stub(int priority, const char *fmt, ...) {}
static void openlog_stub(const char *ident, int option, int facility) {}
static void closelog_stub(void) {}

// Import table for libc++_shared.so
static DynLibFunction cpplib_imports[] = {
    // libc core
    {"abort", (uintptr_t)&abort},
    {"calloc", (uintptr_t)&calloc},
    {"free", (uintptr_t)&free_wrapper},
    {"malloc", (uintptr_t)&malloc},
    {"memalign", (uintptr_t)&memalign_wrapper},
    {"realloc", (uintptr_t)&realloc_wrapper},
    {"memchr", (uintptr_t)&memchr},
    {"memcmp", (uintptr_t)&memcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"memmove", (uintptr_t)&memmove},
    {"memset", (uintptr_t)&memset},
    {"strcmp", (uintptr_t)&strcmp},
    {"strncmp", (uintptr_t)&strncmp},
    {"strcpy", (uintptr_t)&strcpy},
    {"strncpy", (uintptr_t)&strncpy},
    {"strlen", (uintptr_t)&strlen},
    {"strcat", (uintptr_t)&strcat},
    {"strchr", (uintptr_t)&strchr},
    {"strrchr", (uintptr_t)&strrchr},
    {"strstr", (uintptr_t)&strstr},
    {"strerror", (uintptr_t)&strerror},
    {"strtol", (uintptr_t)&strtol},
    {"strtoul", (uintptr_t)&strtoul},
    {"strtoll", (uintptr_t)&strtoll},
    {"strtoull", (uintptr_t)&strtoull},
    {"strtof", (uintptr_t)&strtof},
    {"strtod", (uintptr_t)&strtod},
    {"sprintf", (uintptr_t)&sprintf},
    {"snprintf", (uintptr_t)&snprintf},
    {"vsnprintf", (uintptr_t)&vsnprintf},
    {"fprintf", (uintptr_t)&fprintf},
    {"printf", (uintptr_t)&printf},
    {"sscanf", (uintptr_t)&sscanf},

    // stdio
    {"fopen", (uintptr_t)&cpplib_fopen_wrapper},
    {"fclose", (uintptr_t)&fclose},
    {"fread", (uintptr_t)&fread},
    {"fwrite", (uintptr_t)&fwrite},
    {"fseek", (uintptr_t)&fseek},
    {"fseeko", (uintptr_t)&fseeko},
    {"ftello", (uintptr_t)&ftello},
    {"fflush", (uintptr_t)&fflush},
    {"fputc", (uintptr_t)&fputc},
    {"getc", (uintptr_t)&getc},
    {"fstat", (uintptr_t)&fstat},

    // unistd / fcntl
    {"close", (uintptr_t)&close},
    {"read", (uintptr_t)&read},
    {"write", (uintptr_t)&write},
    {"getcwd", (uintptr_t)&getcwd},
    {"getpid", (uintptr_t)&getpid},
    {"chdir", (uintptr_t)&chdir},
    {"fchmod", (uintptr_t)&fchmod},
    {"ftruncate", (uintptr_t)&ftruncate},
    {"ioctl", (uintptr_t)&ret0}, // stub - not available on Switch

    // dirent
    {"closedir", (uintptr_t)&closedir},
    {"fdopendir", (uintptr_t)&ret0}, // stub — not on Switch
    {"opendir", (uintptr_t)&opendir},
    {"readdir", (uintptr_t)&readdir},

    // stat
    {"mkdir", (uintptr_t)&mkdir},
    {"stat", (uintptr_t)&stat},
    {"lstat", (uintptr_t)&lstat},
    {"fchmodat", (uintptr_t)&ret0}, // stub — not on Switch

    // pthread
    {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast},
    {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy},
    {"pthread_cond_init", (uintptr_t)&pthread_cond_init},
    {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal},
    {"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait},
    {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait},
    {"pthread_create", (uintptr_t)&pthread_create},
    {"pthread_detach", (uintptr_t)&pthread_detach},
    {"pthread_equal", (uintptr_t)&pthread_equal},
    {"pthread_join", (uintptr_t)&pthread_join},
    {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy},
    {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init},
    {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock},
    {"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock},
    {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock},
    {"pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy},
    {"pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init},
    {"pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype},
    {"pthread_once", (uintptr_t)&pthread_once},
    {"pthread_self", (uintptr_t)&pthread_self},
    {"pthread_setspecific", (uintptr_t)&pthread_setspecific},
    {"pthread_getspecific", (uintptr_t)&pthread_getspecific},
    {"pthread_key_create", (uintptr_t)&pthread_key_create},
    {"pthread_key_delete", (uintptr_t)&pthread_key_delete},

    // time
    {"clock_gettime", (uintptr_t)&clock_gettime},
    {"nanosleep", (uintptr_t)&nanosleep},

    // wchar / wctype
    {"btowc", (uintptr_t)&btowc},
    {"mbrlen", (uintptr_t)&mbrlen},
    {"mbrtowc", (uintptr_t)&mbrtowc},
    {"mbsnrtowcs", (uintptr_t)&mbsnrtowcs},
    {"mbsrtowcs", (uintptr_t)&mbsrtowcs},
    {"wcrtomb", (uintptr_t)&wcrtomb},
    {"wcsnrtombs", (uintptr_t)&wcsnrtombs},
    {"wcslen", (uintptr_t)&wcslen},
    {"wmemchr", (uintptr_t)&wmemchr},
    {"wmemcmp", (uintptr_t)&wmemcmp},
    {"wmemcpy", (uintptr_t)&wmemcpy},
    {"wmemmove", (uintptr_t)&wmemmove},
    {"wmemset", (uintptr_t)&wmemset},
    {"wctob", (uintptr_t)&wctob},
    {"wcscoll", (uintptr_t)&wcscoll},
    {"wcsxfrm", (uintptr_t)&wcsxfrm},
    {"towlower", (uintptr_t)&towlower},
    {"towupper", (uintptr_t)&towupper},
    {"iswspace", (uintptr_t)&iswspace},

    // libm
    {"copysignl", (uintptr_t)&copysignl},
    {"scalbnl", (uintptr_t)&scalbnl},

    // environment
    {"getenv", (uintptr_t)&getenv},

    // errno
    {"__errno", (uintptr_t)&__errno},

    // __cxa
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},

    // Android-specific stubs
    {"__system_property_get", (uintptr_t)&__system_property_get_stub},
    {"__system_property_find", (uintptr_t)&__system_property_find_stub},
    {"android_set_abort_message", (uintptr_t)&android_set_abort_message_stub},
    {"dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_stub},
    {"getauxval", (uintptr_t)&getauxval_stub},
    {"__assert2", (uintptr_t)&__assert2_stub},
    {"__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_stub},

    // locale stubs
    {"newlocale", (uintptr_t)&newlocale_stub},
    {"freelocale", (uintptr_t)&freelocale_stub},
    {"uselocale", (uintptr_t)&uselocale_stub},

    // ctype locale variants
    {"isdigit_l", (uintptr_t)&isdigit_l_stub},
    {"islower_l", (uintptr_t)&islower_l_stub},
    {"isupper_l", (uintptr_t)&isupper_l_stub},
    {"isxdigit_l", (uintptr_t)&isxdigit_l_stub},
    {"iswlower_l", (uintptr_t)&iswlower_l_stub},
    {"toupper_l", (uintptr_t)&toupper_l_stub},
    {"tolower_l", (uintptr_t)&tolower_l_stub},
    {"iswspace_l", (uintptr_t)&iswspace_l_stub},
    {"iswprint_l", (uintptr_t)&iswprint_l_stub},
    {"iswcntrl_l", (uintptr_t)&iswcntrl_l_stub},
    {"iswupper_l", (uintptr_t)&iswupper_l_stub},
    {"iswalpha_l", (uintptr_t)&iswalpha_l_stub},
    {"iswdigit_l", (uintptr_t)&iswdigit_l_stub},
    {"iswpunct_l", (uintptr_t)&iswpunct_l_stub},
    {"iswxdigit_l", (uintptr_t)&iswxdigit_l_stub},
    {"iswblank_l", (uintptr_t)&iswblank_l_stub},
    {"towupper_l", (uintptr_t)&towupper_l_stub},
    {"towlower_l", (uintptr_t)&towlower_l_stub},
    {"strftime_l", (uintptr_t)&strftime_l_stub},
    {"strcoll_l", (uintptr_t)&strcoll_l_stub},
    {"strxfrm_l", (uintptr_t)&strxfrm_l_stub},
    {"wcscoll_l", (uintptr_t)&wcscoll_l_stub},
    {"wcsxfrm_l", (uintptr_t)&wcsxfrm_l_stub},
    {"strtoll_l", (uintptr_t)&strtoll_l_stub},
    {"strtoull_l", (uintptr_t)&strtoull_l_stub},
    {"strtold_l", (uintptr_t)&strtold_l_stub},

    // missing imports from debug.log
    {"__sF", (uintptr_t)&fake_sF},
    {"vfprintf", (uintptr_t)&vfprintf},
    {"vasprintf", (uintptr_t)&vasprintf},
    {"posix_memalign", (uintptr_t)&posix_memalign_stub},
    {"__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_stub},
    {"strtold", (uintptr_t)&strtold_stub},
    {"wcstol", (uintptr_t)&wcstol},
    {"wcstoul", (uintptr_t)&wcstoul},
    {"wcstoll", (uintptr_t)&wcstoll},
    {"wcstoull", (uintptr_t)&wcstoull},
    {"wcstof", (uintptr_t)&wcstof},
    {"wcstod", (uintptr_t)&wcstod},
    {"wcstold", (uintptr_t)&wcstold_stub},
    {"swprintf", (uintptr_t)&swprintf},
    {"strerror_r", (uintptr_t)&strerror_r},
    {"sysconf", (uintptr_t)&sysconf_stub},
    {"open", (uintptr_t)&cpplib_open_wrapper},
    {"ungetc", (uintptr_t)&ungetc},
    {"vsscanf", (uintptr_t)&vsscanf},
    {"mbtowc", (uintptr_t)&mbtowc},
    {"localeconv", (uintptr_t)&localeconv},
    {"setlocale", (uintptr_t)&setlocale},
    {"realpath", (uintptr_t)&realpath_stub},
    {"symlink", (uintptr_t)&symlink},
    {"link", (uintptr_t)&link},
    {"sendfile", (uintptr_t)&sendfile_stub},
    {"readlink", (uintptr_t)&readlink},
    {"pathconf", (uintptr_t)&pathconf},
    {"utimensat", (uintptr_t)&utimensat_stub},
    {"remove", (uintptr_t)&remove},
    {"openat", (uintptr_t)&cpplib_openat_wrapper},
    {"unlinkat", (uintptr_t)&unlinkat_stub},
    {"rename", (uintptr_t)&rename},
    {"truncate", (uintptr_t)&truncate},
    {"statvfs", (uintptr_t)&statvfs_stub},
    {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock},
    {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock},
    {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock},
    {"syscall", (uintptr_t)&syscall_stub},

    // syslog stubs
    {"syslog", (uintptr_t)&syslog_stub},
    {"openlog", (uintptr_t)&openlog_stub},
    {"closelog", (uintptr_t)&closelog_stub},

    // signal
    {"raise", (uintptr_t)&raise}};

#define CPPLIB_NUM_IMPORTS (sizeof(cpplib_imports) / sizeof(DynLibFunction))

int cpplib_load(const char *filename)
{
  FILE *fd = fopen(filename, "rb");
  if (!fd)
  {
    debugPrintf("cpplib: cannot open %s\n", filename);
    return -1;
  }

  fseek(fd, 0, SEEK_END);
  size_t file_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  void *file_buf = malloc(file_size);
  if (!file_buf)
  {
    fclose(fd);
    return -2;
  }
  fread(file_buf, file_size, 1, fd);
  fclose(fd);

  Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_buf;
  if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0)
  {
    free(file_buf);
    return -3;
  }

  Elf64_Phdr *phdr = (Elf64_Phdr *)((uintptr_t)file_buf + ehdr->e_phoff);
  Elf64_Shdr *shdr = (Elf64_Shdr *)((uintptr_t)file_buf + ehdr->e_shoff);
  char *shstrtab_local = (char *)((uintptr_t)file_buf + shdr[ehdr->e_shstrndx].sh_offset);

  // Calculate total memory needed
  uintptr_t min_vaddr = UINT64_MAX, max_vaddr = 0;
  uintptr_t text_end = 0; // end of executable segment
  for (int i = 0; i < ehdr->e_phnum; i++)
  {
    if (phdr[i].p_type == PT_LOAD)
    {
      if (phdr[i].p_vaddr < min_vaddr)
        min_vaddr = phdr[i].p_vaddr;
      uintptr_t end = phdr[i].p_vaddr + CPPLIB_ALIGN(phdr[i].p_memsz, phdr[i].p_align);
      if (end > max_vaddr)
        max_vaddr = end;
      if (phdr[i].p_flags & PF_X)
      {
        text_end = phdr[i].p_vaddr + CPPLIB_ALIGN(phdr[i].p_memsz, 0x1000) - min_vaddr;
      }
    }
  }
  cpplib_load_size = CPPLIB_ALIGN(max_vaddr - min_vaddr, 0x1000);
  cpplib_min_vaddr = min_vaddr;

  // Allocate memory
  cpplib_base = memalign(0x1000, cpplib_load_size);
  if (!cpplib_base)
  {
    free(file_buf);
    return -4;
  }
  memset(cpplib_base, 0, cpplib_load_size);

  // Reserve virtual memory for executable mapping BEFORE relocations
  // so all internal pointers use the virtual (executable) address
  virtmemLock();
  cpplib_virtbase = virtmemFindCodeMemory(cpplib_load_size, 0x1000);
  VirtmemReservation *rv = virtmemAddReservation(cpplib_virtbase, cpplib_load_size);
  (void)rv; // suppress unused warning
  virtmemUnlock();

  debugPrintf("cpplib: loading %s at %p -> %p (size=%u KB)\n",
              filename, cpplib_base, cpplib_virtbase, (unsigned)(cpplib_load_size / 1024));

  // Copy LOAD segments
  for (int i = 0; i < ehdr->e_phnum; i++)
  {
    if (phdr[i].p_type == PT_LOAD)
    {
      void *dst = (void *)((uintptr_t)cpplib_base + phdr[i].p_vaddr - min_vaddr);
      void *src = (void *)((uintptr_t)file_buf + phdr[i].p_offset);
      memcpy(dst, src, phdr[i].p_filesz);
    }
  }

  // Find .dynsym and .dynstr
  Elf64_Sym *local_syms = NULL;
  char *local_dynstr = NULL;
  int local_num_syms = 0;

  for (int i = 0; i < ehdr->e_shnum; i++)
  {
    char *name = shstrtab_local + shdr[i].sh_name;
    if (strcmp(name, ".dynsym") == 0)
    {
      local_syms = (Elf64_Sym *)((uintptr_t)cpplib_base + shdr[i].sh_addr - min_vaddr);
      local_num_syms = shdr[i].sh_size / sizeof(Elf64_Sym);
    }
    else if (strcmp(name, ".dynstr") == 0)
    {
      local_dynstr = (char *)((uintptr_t)cpplib_base + shdr[i].sh_addr - min_vaddr);
    }
  }

  if (!local_syms || !local_dynstr)
  {
    debugPrintf("cpplib: no symtab/strtab\n");
    free(cpplib_base);
    free(file_buf);
    return -5;
  }

  cpplib_syms = local_syms;
  cpplib_dynstrtab = local_dynstr;
  cpplib_num_syms = local_num_syms;

  // Apply relocations
  for (int i = 0; i < ehdr->e_shnum; i++)
  {
    char *name = shstrtab_local + shdr[i].sh_name;
    if (strcmp(name, ".rela.dyn") == 0 || strcmp(name, ".rela.plt") == 0)
    {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)cpplib_base + shdr[i].sh_addr - min_vaddr);
      int num_rels = shdr[i].sh_size / sizeof(Elf64_Rela);

      for (int j = 0; j < num_rels; j++)
      {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)cpplib_base + rels[j].r_offset - min_vaddr);
        int type = ELF64_R_TYPE(rels[j].r_info);
        int sym_idx = ELF64_R_SYM(rels[j].r_info);

        switch (type)
        {
        case R_AARCH64_RELATIVE:
          // Use virtbase so pointers work after svcMapProcessCodeMemory
          *ptr = (uintptr_t)cpplib_virtbase + rels[j].r_addend - min_vaddr;
          break;

        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
        {
          Elf64_Sym *sym = &local_syms[sym_idx];
          if (sym->st_shndx == SHN_UNDEF)
          {
            char *sym_name = local_dynstr + sym->st_name;
            // Strip @LIBC version suffix for lookup
            char clean_name[256];
            strncpy(clean_name, sym_name, sizeof(clean_name) - 1);
            clean_name[sizeof(clean_name) - 1] = '\0';
            char *at = strchr(clean_name, '@');
            if (at)
              *at = '\0';

            int found = 0;
            for (int k = 0; k < (int)CPPLIB_NUM_IMPORTS; k++)
            {
              if (strcmp(clean_name, cpplib_imports[k].symbol) == 0)
              {
                *ptr = cpplib_imports[k].func;
                found = 1;
                break;
              }
            }
            if (!found)
            {
              debugPrintf("cpplib: MISSING import: %s\n", sym_name);
              *ptr = 0; // will crash if called
            }
          }
          else
          {
            // Internal symbol — resolve to virtual (executable) address
            *ptr = (uintptr_t)cpplib_virtbase + sym->st_value - min_vaddr;
          }
          break;
        }

        case R_AARCH64_ABS64:
        {
          Elf64_Sym *sym = &local_syms[sym_idx];
          if (sym->st_shndx != SHN_UNDEF)
          {
            *ptr = (uintptr_t)cpplib_virtbase + sym->st_value - min_vaddr + rels[j].r_addend;
          }
          else
          {
            char *sym_name = local_dynstr + sym->st_name;
            char clean_name[256];
            strncpy(clean_name, sym_name, sizeof(clean_name) - 1);
            clean_name[sizeof(clean_name) - 1] = '\0';
            char *at = strchr(clean_name, '@');
            if (at)
              *at = '\0';

            for (int k = 0; k < (int)CPPLIB_NUM_IMPORTS; k++)
            {
              if (strcmp(clean_name, cpplib_imports[k].symbol) == 0)
              {
                *ptr = cpplib_imports[k].func + rels[j].r_addend;
                break;
              }
            }
          }
          break;
        }

        default:
          break;
        }
      }
    }
  }

  int defined_count = 0;
  for (int i = 0; i < cpplib_num_syms; i++)
  {
    if (cpplib_syms[i].st_shndx != SHN_UNDEF &&
        ELF64_ST_BIND(cpplib_syms[i].st_info) == STB_GLOBAL &&
        ELF64_ST_TYPE(cpplib_syms[i].st_info) == STT_FUNC)
      defined_count++;
  }

  debugPrintf("cpplib: loaded %d symbols (%d defined funcs)\n", cpplib_num_syms, defined_count);

  // Make code executable via svcMapProcessCodeMemory (same as so_finalize)
  debugPrintf("cpplib: mapping code at %p -> %p\n", cpplib_base, cpplib_virtbase);

  Result rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(),
                                      (u64)cpplib_virtbase, (u64)cpplib_base, cpplib_load_size);
  if (R_FAILED(rc))
  {
    debugPrintf("cpplib: svcMapProcessCodeMemory failed: %08x\n", rc);
    free(file_buf);
    return -6;
  }

  // Set text as RX, data as RW
  u64 text_asize = CPPLIB_ALIGN(text_end, 0x1000);
  if (text_asize > 0)
  {
    rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(),
                                       (u64)cpplib_virtbase, text_asize, Perm_Rx);
    if (R_FAILED(rc))
      debugPrintf("cpplib: RX perm failed: %08x\n", rc);
  }

  u64 rest_asize = cpplib_load_size - text_asize;
  if (rest_asize > 0)
  {
    rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(),
                                       (u64)cpplib_virtbase + text_asize, rest_asize, Perm_Rw);
    if (R_FAILED(rc))
      debugPrintf("cpplib: RW perm failed: %08x\n", rc);
  }

  // Flush caches for the new code region
  armDCacheFlush(cpplib_virtbase, cpplib_load_size);
  armICacheInvalidate(cpplib_virtbase, cpplib_load_size);

  debugPrintf("cpplib: code mapped and executable\n");

  // Update symtab/strtab pointers to the virtual mapping
  // (heap pages are now Perm_None after svcMapProcessCodeMemory)
  ptrdiff_t virt_offset = (uintptr_t)cpplib_virtbase - (uintptr_t)cpplib_base;
  cpplib_syms = (Elf64_Sym *)((uintptr_t)cpplib_syms + virt_offset);
  cpplib_dynstrtab = (char *)((uintptr_t)cpplib_dynstrtab + virt_offset);

  free(file_buf);
  return 0;
}

// Look up a symbol in libc++_shared.so by name
// Returns the EXECUTABLE address (cpplib_virtbase-relative)
uintptr_t cpplib_find_symbol(const char *name)
{
  if (!cpplib_syms || !cpplib_dynstrtab || !cpplib_virtbase)
    return 0;

  for (int i = 0; i < cpplib_num_syms; i++)
  {
    if (cpplib_syms[i].st_shndx != SHN_UNDEF)
    {
      char *sym_name = cpplib_dynstrtab + cpplib_syms[i].st_name;
      if (strcmp(name, sym_name) == 0)
      {
        // Return virtual (executable) address, not heap address
        return (uintptr_t)cpplib_virtbase + cpplib_syms[i].st_value - cpplib_min_vaddr;
      }
    }
  }
  return 0;
}

// Used by so_resolve: check cpplib symbols for unresolved imports
int cpplib_resolve_symbol(const char *name, uintptr_t *out_addr)
{
  uintptr_t addr = cpplib_find_symbol(name);
  if (addr)
  {
    *out_addr = addr;
    return 1;
  }
  return 0;
}
