#pragma once

#if defined(_WIN32) && !defined(VIDEOCUT_FRAME_STATIC)
#if defined(VIDEOCUT_FRAME_BUILDING_LIBRARY)
#define VIDEOCUT_FRAME_API __declspec(dllexport)
#else
#define VIDEOCUT_FRAME_API __declspec(dllimport)
#endif
#else
#define VIDEOCUT_FRAME_API
#endif
