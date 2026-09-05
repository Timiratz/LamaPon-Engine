#pragma once

#if defined(LAMAPON_HEADLESS)
#define LAMAPON_API
#elif defined(_WIN32)
#if defined(LAMAPON_RUNTIME_BUILD)
#define LAMAPON_API __declspec(dllexport)
#else
#define LAMAPON_API __declspec(dllimport)
#endif
#else
#define LAMAPON_API
#endif
