#ifndef __RECOMP_ROM_ARCHIVE_HPP__
#define __RECOMP_ROM_ARCHIVE_HPP__

// The ROMs inside an archive, for select_rom (RECOMP_ROM_ARCHIVES, on with Emscripten): ROMs are often kept and
// downloaded packed, and Load ROM said "not a valid ROM file" of every one.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recomp {
    enum class RomValidationError; // librecomp/game.hpp

    // If data is a .zip, .7z, .gz, .xz or .tar (a .tar inside a .gz or .xz too), calls found with each file in it that
    // starts as an N64 ROM does in any byte order, and is from 4 KiB to 64 MiB, in the archive's order, until found
    // returns true, and returns true. Returns false, calling nothing, for anything else (a ROM itself). A file that
    // unpacks to more than 256 MiB, or is damaged, ends the search where it is.
    bool for_each_rom_in_archive(std::span<const uint8_t> data, const std::function<bool(std::vector<uint8_t>&& rom)>& found);

    // Whether a file that starts with these bytes (its first 512, or all of a shorter one) is an archive
    // for_each_rom_in_archive searches.
    bool is_rom_archive(std::span<const uint8_t> start);

    // select_rom of an archive (rom_archive_select.cpp), called first thing by select_rom: if the file at rom_path is
    // an archive, select_rom is given each ROM in it, unpacked to a temporary file, until one is game_id's, and what it
    // said of that one is returned, or, with none, of the closest (the game in another version before another game
    // before no ROM at all). nullopt for a file that isn't an archive, which select_rom goes on to check itself.
    std::optional<RomValidationError> select_rom_in_archive(const std::filesystem::path& rom_path, const std::u8string& game_id);
}

#endif
