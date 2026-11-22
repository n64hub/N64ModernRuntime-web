#ifndef __RECOMP_ROM_ARCHIVE_HPP__
#define __RECOMP_ROM_ARCHIVE_HPP__

// The ROMs inside an archive, for select_rom (RECOMP_ROM_ARCHIVES, on with Emscripten): ROMs are often kept and
// downloaded packed, and Load ROM said "not a valid ROM file" of every one.

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace recomp {
    // If data is a .zip, .7z, .gz, .xz or .tar (a .tar inside a .gz or .xz too), calls found with each file in it that
    // starts as an N64 ROM does in any byte order, and is from 4 KiB to 64 MiB, in the archive's order, until found
    // returns true, and returns true. Returns false, calling nothing, for anything else (a ROM itself). A file that
    // unpacks to more than 256 MiB, or is damaged, ends the search where it is.
    bool for_each_rom_in_archive(std::span<const uint8_t> data, const std::function<bool(std::vector<uint8_t>&& rom)>& found);
}

#endif
