/* md_cxx_stubs.cpp - minimal C++ runtime for the libstdc++-less Amiga link.
 * Linked once into each binary that carries ymfm objects (game + loader).
 * The heap operators only exist because ymfm's (unused) save_restore paths
 * instantiate std::vector; they are malloc-backed so they'd even work if
 * reached. Excluded from host builds (libstdc++ provides the real ones). */
#ifndef MD_HOST_BUILD
#include <stdlib.h>
#include <new>
extern "C" void __cxa_pure_virtual(void) { for (;;) {} }
void *operator new(std::size_t n) { return malloc(n); }
void *operator new[](std::size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, std::size_t) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete[](void *p, std::size_t) noexcept { free(p); }
namespace std {
void __throw_length_error(const char *) { for (;;) {} }
void __throw_bad_alloc() { for (;;) {} }
}
#endif
