// KUSEG memory for a recomp whose code reads through TLB-mapped addresses (recomp::make_kuseg_accessible, addresses.hpp).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#endif

#include "librecomp/addresses.hpp"

bool recomp::make_kuseg_accessible(uint8_t* rdram, uint32_t vaddr, uint32_t size) {
    if (size == 0 || vaddr >= 0x80000000u || size > 0x80000000u - vaddr) {
        return false;
    }
#ifdef __EMSCRIPTEN__
    // The window is part of RDRAM's allocation, and WebAssembly memory is all accessible.
    (void)rdram;
#ifdef RECOMP_KUSEG_WINDOW
    return vaddr + size <= RECOMP_KUSEG_WINDOW_SIZE;
#else
    return false;
#endif
#else
    // A KUSEG address lies 2 GB or more past rdram, as the generated code computes it (recomp.h's RECOMP_RDRAM_OFFSET).
    constexpr uint32_t page = 0x1000;
    uint32_t start = vaddr & ~(page - 1);
    uint32_t end = (uint32_t)(((uint64_t)vaddr + size + page - 1) & ~(uint64_t)(page - 1));
    uint8_t* host = rdram + 0x80000000ULL + start;
#ifdef _WIN32
    // The range past mem_size is reserved and not committed (recomp.cpp), which VirtualProtect can't open.
    return VirtualAlloc(host, end - start, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(host, end - start, PROT_READ | PROT_WRITE) == 0;
#endif
#endif
}
