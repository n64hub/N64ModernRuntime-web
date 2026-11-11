#ifndef __RECOMP_ADDRESSES_HPP__
#define __RECOMP_ADDRESSES_HPP__

#include <cstdint>
#include "ultramodern/ultra64.h"
#include "recomp.h"

namespace recomp {
    // 512GB (kseg0 size)
    constexpr size_t mem_size = 512ULL * 1024ULL * 1024ULL;
    // 4GB (the full address space)
#ifdef __EMSCRIPTEN__
    // A 32-bit address space can't reserve 4 GiB, and WebAssembly memory has no page protection to catch stray
    // accesses with anyway. A recomp built with RECOMP_KUSEG_WINDOW gets KUSEG's window right after kseg0 (recomp.h).
#ifdef RECOMP_KUSEG_WINDOW
    static_assert(RECOMP_KUSEG_WINDOW_BASE == mem_size);
    constexpr size_t allocation_size = mem_size + RECOMP_KUSEG_WINDOW_SIZE;
#else
    constexpr size_t allocation_size = mem_size;
#endif
#else
    constexpr size_t allocation_size = 4096ULL * 1024ULL * 1024ULL;
#endif
    // We need a place in rdram to hold the PI handles, so pick an address in extended rdram
    constexpr int32_t cart_handle = 0x80800000;
    constexpr int32_t drive_handle = (int32_t)(cart_handle + sizeof(OSPiHandle));
    constexpr int32_t flash_handle = (int32_t)(drive_handle + sizeof(OSPiHandle));
    constexpr int32_t flash_handle_end = (int32_t)(flash_handle + sizeof(OSPiHandle));
    constexpr int32_t patch_rdram_start = 0x80801000;
    static_assert(patch_rdram_start >= flash_handle_end);
    constexpr int32_t mod_rdram_start = 0x81000000;

    // Flashram occupies the same physical address as sram, but that issue is avoided because libultra exposes
    // a high-level interface for flashram. Because that high-level interface is reimplemented, low level accesses
    // that involve physical addresses don't need to be handled for flashram.
    constexpr uint32_t sram_base = 0x08000000;
    constexpr uint32_t rom_base = 0x10000000;
    constexpr uint32_t drive_base = 0x06000000;

    // Makes [vaddr, vaddr + size) of KUSEG (TLB-mapped addresses, below 0x80000000) readable and writable, for a recomp
    // whose code reads its strings, tables and constants through them: on a native build by opening the pages of
    // allocation_size's range it reserves there, and on the web if the range lies in the window RECOMP_KUSEG_WINDOW
    // allocates (recomp.h). It starts zeroed, and the recomp fills it (do_rom_read writes through these addresses).
    // False if the range can't be opened: on the web past the window, or without RECOMP_KUSEG_WINDOW.
    bool make_kuseg_accessible(uint8_t* rdram, uint32_t vaddr, uint32_t size);

    void register_heap_exports();
    void init_heap(uint8_t* rdram, uint32_t address);
    void* alloc(uint8_t* rdram, size_t size);
    void free(uint8_t* rdram, void* mem);
}

extern "C" void recomp_alloc(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_free(uint8_t* rdram, recomp_context* ctx);

#endif
