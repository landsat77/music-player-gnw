/*
 * The C++ runtime of Game_Music_Emu (built without exceptions, RTTI and
 * libstdc++): operator new / delete on the player's heap (mp_alloc.c).
 * Replaces the SDK's gw_core_cxx_support.cpp, whose allocator takes
 * memory from the pools the player's heap already owns.
 */
#include <stddef.h>
#include <new>

extern "C" {
#include "mp_platform.h"
}

void *operator new(size_t n) { return mp_malloc(n ? n : 1); }
void *operator new[](size_t n) { return mp_malloc(n ? n : 1); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return mp_malloc(n ? n : 1); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return mp_malloc(n ? n : 1); }
void operator delete(void *p) noexcept { mp_free(p); }
void operator delete[](void *p) noexcept { mp_free(p); }
void operator delete(void *p, size_t) noexcept { mp_free(p); }
void operator delete[](void *p, size_t) noexcept { mp_free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { mp_free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { mp_free(p); }

#ifndef MP_HOST
extern "C" void __cxa_pure_virtual(void)
{
    for (;;) {
    }
}
namespace std {
const nothrow_t nothrow{};
void __throw_length_error(const char *)
{
    for (;;) {
    }
}
}
extern "C" int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
void *__dso_handle = 0;
#endif
