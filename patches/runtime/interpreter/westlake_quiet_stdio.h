#pragma once

// Production-like performance arm for the Toutiao noisy/quiet A/B.
//
// Historical PFCUT probes use fprintf(stderr, ...) followed by fflush(stderr)
// directly in interpreter hot paths.  Even nominally capped thread_local probes
// become unbounded in an app with many short-lived threads.  Keep all argument
// evaluation and diagnostic helper calls intact because this heavily patched
// runtime has historically hidden behavioral work in those expressions.  The
// quiet artifact suppresses only the actual PFCUT write and its matching flush.
// Non-PFCUT output remains untouched, including fatal diagnostics, API-gap
// evidence, JIT-live markers, and low-frequency phase markers.

#if defined(WESTLAKE_QUIET_HOTPATH)

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

static thread_local bool g_westlake_quiet_dropped_stderr = false;

static inline bool WestlakeQuietDropFormat(FILE* stream, const char* format) {
  if (stream != stderr || format == nullptr) {
    return false;
  }
  return strncmp(format, "[PFCUT]", 7u) == 0 ||
         strncmp(format, "[WESTLAKE-BADCALL]", 18u) == 0 ||
         strncmp(format, "[WESTLAKE-BADMETHOD]", 20u) == 0;
}

static int WestlakeQuietFprintf(FILE* stream, const char* format, ...) {
  const bool drop = WestlakeQuietDropFormat(stream, format);
  g_westlake_quiet_dropped_stderr = drop;
  if (drop) {
    return 0;
  }
  va_list args;
  va_start(args, format);
  const int result = vfprintf(stream, format, args);
  va_end(args);
  return result;
}

static inline int WestlakeQuietFflush(FILE* stream) {
  if (stream == stderr && g_westlake_quiet_dropped_stderr) {
    g_westlake_quiet_dropped_stderr = false;
    return 0;
  }
  return ::fflush(stream);
}

}  // namespace

// A function-style wrapper intentionally preserves evaluation of __VA_ARGS__.
#define fprintf(stream, format, ...) \
  WestlakeQuietFprintf((stream), (format), ##__VA_ARGS__)
#define fflush(stream) WestlakeQuietFflush((stream))

#endif  // WESTLAKE_QUIET_HOTPATH
