#pragma once

#if defined(_WIN32) && defined(VIDEOCUT_BASE_SHARED)
#  if defined(VIDEOCUT_BASE_BUILD)
#    define VIDEOCUT_BASE_API __declspec(dllexport)
#  else
#    define VIDEOCUT_BASE_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(VIDEOCUT_BASE_SHARED)
#  define VIDEOCUT_BASE_API __attribute__((visibility("default")))
#else
#  define VIDEOCUT_BASE_API
#endif
