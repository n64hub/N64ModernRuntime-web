// recomp::select_rom_in_archive (rom_archive.hpp). Each ROM found in the archive is written to a temporary file and
// given to select_rom, so it is checked and stored exactly as a ROM picked by itself is, by whatever select_rom a game's
// runtime has. select_rom only gets this one call (and the include) so that the change merges onto runtimes that
// change select_rom's own lines.

#include "rom_archive.hpp"

#include <fstream>
#include <random>

#include "librecomp/game.hpp"

namespace {

constexpr uint64_t max_archive_size = 256 << 20;

// How near an error is to the game's ROM: another version of it, before one too new, before another game's ROM.
int closeness(recomp::RomValidationError error) {
    switch (error) {
        case recomp::RomValidationError::IncorrectVersion: return 3;
        case recomp::RomValidationError::NotYet: return 2;
        case recomp::RomValidationError::IncorrectRom: return 1;
        default: return 0;
    }
}

} // namespace

std::optional<recomp::RomValidationError> recomp::select_rom_in_archive(const std::filesystem::path& rom_path, const std::u8string& game_id) {
    std::ifstream file{ rom_path, std::ios::binary };
    if (!file) {
        return std::nullopt;
    }
    std::vector<uint8_t> data(512);
    file.read(reinterpret_cast<char*>(data.data()), data.size());
    data.resize(static_cast<size_t>(file.gcount()));
    if (!is_rom_archive(data)) {
        return std::nullopt;
    }

    file.clear();
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > max_archive_size) {
        return RomValidationError::NotARom;
    }
    data.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), size);
    if (!file) {
        return RomValidationError::FailedToOpen;
    }
    file.close();

    std::error_code ec;
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return RomValidationError::FailedToOpen;
    }
    std::filesystem::path temp_path = temp_dir / ("recomp-rom-" + std::to_string(std::random_device{}()) + ".z64");

    // A copy, as select_rom took game_id without const in older runtimes.
    std::u8string id = game_id;
    RomValidationError result = RomValidationError::NotARom;
    for_each_rom_in_archive(data, [&](std::vector<uint8_t>&& rom) {
        {
            std::ofstream out{ temp_path, std::ios::binary | std::ios::trunc };
            out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
            if (!out) {
                result = RomValidationError::FailedToOpen;
                return true;
            }
        }
        RomValidationError rom_result = select_rom(temp_path, id);
        if (rom_result == RomValidationError::Good) {
            result = rom_result;
            return true;
        }
        if (closeness(rom_result) > closeness(result)) {
            result = rom_result;
        }
        return false;
    });
    std::filesystem::remove(temp_path, ec);
    return result;
}
