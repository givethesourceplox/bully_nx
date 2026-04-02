/* cxx_diag.cpp
 *
 * Copyright (C) 2026 givethesourceplox
 *
 */

#include "cpplib_loader.h"

extern "C" int debugPrintf(const char *text, ...);
extern "C" void abort(void);
extern "C" void __cxa_throw(void *ex, void *tinfo, void (*dest)(void *)) __attribute__((noreturn));
extern "C" void __attribute__((noreturn)) __cxa_throw_wrapper(void *ex, void *tinfo, void (*dest)(void *));

using cxa_throw_fn = void (*)(void *, void *, void (*)(void *));

extern "C" void __attribute__((noreturn)) __cxa_throw_wrapper(void *ex, void *tinfo, void (*dest)(void *))
{
  const void *caller = __builtin_return_address(0);
  cxa_throw_fn cpplib_throw = nullptr;
  uintptr_t addr = cpplib_find_symbol("__cxa_throw");

  if (addr && addr != reinterpret_cast<uintptr_t>(&__cxa_throw_wrapper))
    cpplib_throw = reinterpret_cast<cxa_throw_fn>(addr);

  debugPrintf("__cxa_throw_wrapper: caller=%p tinfo=%p obj=%p dest=%p\n",
              caller, tinfo, ex, reinterpret_cast<void *>(dest));

  if (cpplib_throw)
  {
    cpplib_throw(ex, tinfo, dest);
    __builtin_unreachable();
  }

  debugPrintf("__cxa_throw_wrapper: cpplib __cxa_throw missing, falling back to host runtime\n");
  __cxa_throw(ex, tinfo, dest);
  __builtin_unreachable();
}

extern "C" void __attribute__((noreturn)) terminate_wrapper(void)
{
  const void *caller = __builtin_return_address(0);
  debugPrintf("terminate_wrapper: caller=%p\n", caller);
  abort();
  __builtin_unreachable();
}
