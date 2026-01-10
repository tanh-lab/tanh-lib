#pragma once

#ifdef TANH_WITH_RTSAN
#include <sanitizer/rtsan_interface.h>
#define TANH_NONBLOCKING_FUNCTION [[clang::nonblocking]]
#define TANH_NONBLOCKING_SCOPED_DISABLER __rtsan::ScopedDisabler sd;
#else
#define TANH_NONBLOCKING_FUNCTION
#define TANH_NONBLOCKING_SCOPED_DISABLER
#endif
