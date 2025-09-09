#pragma once

#ifdef TANH_WITH_RTSAN
#define TANH_NONBLOCKING [[clang::nonblocking]]
#else
#define TANH_NONBLOCKING
#endif
