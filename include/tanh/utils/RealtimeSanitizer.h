#pragma once

#ifdef TANH_WITH_RTSAN
#include <sanitizer/rtsan_interface.h>
#define TANH_NONBLOCKING_FUNCTION [[clang::nonblocking]]
#define TANH_NONBLOCKING_SCOPED_DISABLER __rtsan::ScopedDisabler sd;
#define TANH_NONBLOCKING_ENABLE __rtsan_enable();
#define TANH_NONBLOCKING_DISABLE __rtsan_disable();
#else
#define TANH_NONBLOCKING_FUNCTION
#define TANH_NONBLOCKING_SCOPED_DISABLER
#define TANH_NONBLOCKING_ENABLE
#define TANH_NONBLOCKING_DISABLE
#endif
