#include <exception>

extern "C" int debugPrintf(const char *text, ...);

extern "C" int thread_guard_run_int(int (*func)(void *), void *arg)
{
  try
  {
    return func ? func(arg) : 0;
  }
  catch (...)
  {
    debugPrintf("thread_guard_run_int: swallowed uncaught C++ exception\n");
    return -1;
  }
}

extern "C" void *thread_guard_run_ptr(void *(*func)(void *), void *arg)
{
  try
  {
    return func ? func(arg) : nullptr;
  }
  catch (...)
  {
    debugPrintf("thread_guard_run_ptr: swallowed uncaught C++ exception\n");
    return nullptr;
  }
}
