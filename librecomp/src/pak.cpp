// The Controller Pak: libultra's osPfs* functions, over a file-backed pak in each port the game asks for.
//
// A GameEntry's controller_pak_ports (librecomp/game.hpp) is a bit for each port holding a Controller Pak: bit 0 is
// controller 1. A port without its bit has none, and every function answers PFS_ERR_NOPACK there, so a game that sets
// no bit sees what it saw before this file had a pak. It is the game's choice and not every port's by default: some
// games probe a port with osPfsInitPak before they look for a Rumble Pak in it, and start its rumble only when that
// probe fails.
//
// A game whose own settings choose the pak's size, or a Rumble Pak in its place, says so with
// GameEntry::controller_pak_banks, asked each time the game initializes a port's pak.
//
// A game that defines every function this file does keeps its own pak: the linker uses its functions and never pulls
// this object in.
//
// The image is the real on-pak layout, the same `.mpk` format emulators use, of one bank of 32 KiB as Nintendo's own
// paks are, unless the game asks for more:
//   page 0          ID area (ID block copies at 0x20, 0x60, 0x80, 0xC0; byte 0x1A of the block is the bank count, 1)
//   page 1          inode table: 128 big-endian u16 entries, one per page. 0x0001 ends a chain, 0x0003 is free,
//                   anything else is the next page. Byte 1 is a checksum of its entries from the first data page on.
//   page 2          backup of the inode table
//   pages 3-4       note table: 16 entries of 32 bytes
//   pages 5-127     data, 256 bytes a page (123 pages, the "123 pages" games' menus count)
// A larger pak (1 to 62 banks, libultra's PFS_MAX_BANKS) has an inode table and a backup for each bank after page 0,
// the note table after them, and its other banks' pages from page 1 on all data.
// Files are controllerpak_<port>.mpk, port 1 to 4, in the folder of the game's own save (saves/ in the config folder),
// so a page's save sync keeps them like any other save. A port without a file gets a freshly formatted pak, written
// out on the first change, and a file of another size (an emulator's larger pak, or the game's setting changed) has its
// files moved to a pak of the size the game asks for when the game initializes it, if they fit. Each write is whole: to <file>.tmp, renamed over the file, and a file that is
// a symlink, as when saves are shared through a synced folder, has the file it points at replaced and stays a link.

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

namespace {

constexpr int PFS_ERR_NOPACK = 1;
constexpr int PFS_ERR_INVALID = 5;
constexpr int PFS_DATA_FULL = 7;
constexpr int PFS_DIR_FULL = 8;
constexpr int PFS_ERR_EXIST = 9;
constexpr int PFS_ERR_DEVICE = 11;
constexpr int PFS_INITIALIZED = 1;

constexpr size_t page_size = 256;
constexpr int pages_per_bank = 128;
constexpr size_t bank_size = pages_per_bank * page_size;
constexpr int max_banks = 62;
constexpr int num_notes = 16;
constexpr size_t note_size = 32;
constexpr size_t id_offset = 0x20;
constexpr size_t block_size = 32; // osPfsReadWriteFile offsets and sizes are in these units

constexpr uint16_t inode_last = 0x0001;
constexpr uint16_t inode_free = 0x0003;

// Pages are named the way inode entries and notes name them: bank << 8 | page.
using Page = uint16_t;

struct Pak {
    bool loaded = false;
    int banks = 1;
    std::vector<uint8_t> data;
};

std::mutex pak_mutex;
std::array<Pak, 4> paks;

bool has_pak(int port) {
    return port >= 0 && port < 4 && (recomp::get_controller_pak_ports() >> port) & 1;
}

// The banks the game asks for in a port, or 0 for a Rumble Pak in place of the pak.
int wanted_banks(int port) {
    int banks = recomp::get_controller_pak_banks(port);
    return banks < 0 ? 1 : banks > max_banks ? max_banks : banks;
}

std::filesystem::path pak_path(int port) {
    return ultramodern::get_save_file_path().parent_path() / ("controllerpak_" + std::to_string(port + 1) + ".mpk");
}

uint16_t read16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t read32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void write16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void write32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

// libultra's inode_start_page: a bank's first data page.
int first_data_page(const Pak& pak, int bank) { return bank == 0 ? 3 + 2 * pak.banks : 1; }
size_t inode_table_offset(const Pak& pak, int bank) { return (1 + (size_t)bank) * page_size; }
size_t inode_backup_offset(const Pak& pak, int bank) { return (1 + (size_t)pak.banks + bank) * page_size; }
size_t note_table_offset(const Pak& pak) { return (1 + 2 * (size_t)pak.banks) * page_size; }
size_t page_offset(Page page) { return (page >> 8) * bank_size + (page & 0xFF) * page_size; }

bool is_data_page(const Pak& pak, Page page) {
    int bank = page >> 8;
    return bank < pak.banks && (page & 0xFF) >= first_data_page(pak, bank);
}

uint16_t get_inode(const Pak& pak, Page page) { return read16(&pak.data[inode_table_offset(pak, page >> 8) + (page & 0xFF) * 2]); }
void set_inode(Pak& pak, Page page, uint16_t value) { write16(&pak.data[inode_table_offset(pak, page >> 8) + (page & 0xFF) * 2], value); }
uint8_t* note(Pak& pak, int index) { return &pak.data[note_table_offset(pak) + index * note_size]; }
bool note_used(Pak& pak, int index) { return read16(note(pak, index) + 4) != 0 && read32(note(pak, index)) != 0; }
Page note_start(Pak& pak, int index) { return read16(note(pak, index) + 6); }

std::vector<Page> data_pages(const Pak& pak) {
    std::vector<Page> pages;
    for (int bank = 0; bank < pak.banks; bank++) {
        for (int page = first_data_page(pak, bank); page < pages_per_bank; page++) {
            pages.push_back((Page)(bank << 8 | page));
        }
    }
    return pages;
}

void update_inode_checksums(Pak& pak) {
    for (int bank = 0; bank < pak.banks; bank++) {
        uint8_t* table = &pak.data[inode_table_offset(pak, bank)];
        uint32_t sum = 0;
        for (size_t i = first_data_page(pak, bank) * 2; i < page_size; i++) {
            sum += table[i];
        }
        table[1] = (uint8_t)sum;
        std::memcpy(&pak.data[inode_backup_offset(pak, bank)], table, page_size);
    }
}

void format(Pak& pak, int port, int banks) {
    pak.banks = banks;
    pak.data.assign(banks * bank_size, 0);

    // Page 0, block 0: 0x81 then 0x01..0x1F, as freshly formatted paks have it.
    pak.data[0] = 0x81;
    for (int i = 1; i < 32; i++) {
        pak.data[i] = (uint8_t)i;
    }

    // ID block (__OSPackId): repaired, random, serial_mid, serial_low, deviceid, banks, version, checksum,
    // inverted checksum. The checksum is the sum of the first 14 halfwords.
    std::array<uint8_t, 32> id{};
    write32(&id[0x00], 0xFFFFFFFF);
    // Games tell paks apart by this block, so each port's pak gets its own serial.
    write32(&id[0x04], 0x52A92000 + (uint32_t)port);
    write16(&id[0x18], 0x0001); // deviceid
    id[0x1A] = (uint8_t)banks;
    id[0x1B] = 0;               // version
    uint16_t sum = 0;
    for (int i = 0; i < 28; i += 2) {
        sum += read16(&id[i]);
    }
    write16(&id[0x1C], sum);
    write16(&id[0x1E], (uint16_t)(0xFFF2 - sum));
    for (size_t offset : { 0x20, 0x60, 0x80, 0xC0 }) {
        std::memcpy(&pak.data[offset], id.data(), id.size());
    }

    for (Page page : data_pages(pak)) {
        set_inode(pak, page, inode_free);
    }
    update_inode_checksums(pak);
}

// A file's pages in order, from its note's start page.
std::vector<Page> file_pages(const Pak& pak, Page start) {
    std::vector<Page> pages;
    size_t limit = pak.banks * (size_t)pages_per_bank;
    for (Page page = start; is_data_page(pak, page) && pages.size() < limit;) {
        pages.push_back(page);
        uint16_t next = get_inode(pak, page);
        if (next == inode_last) {
            break;
        }
        page = next;
    }
    return pages;
}

int free_pages(const Pak& pak) {
    int count = 0;
    for (Page page : data_pages(pak)) {
        count += get_inode(pak, page) == inode_free ? 1 : 0;
    }
    return count;
}

// Chains the lowest `count` free pages together and zeroes them. Empty when there aren't that many.
std::vector<Page> allocate_pages(Pak& pak, int count) {
    std::vector<Page> pages;
    for (Page page : data_pages(pak)) {
        if ((int)pages.size() == count) {
            break;
        }
        if (get_inode(pak, page) == inode_free) {
            pages.push_back(page);
        }
    }
    if (count <= 0 || (int)pages.size() < count) {
        return {};
    }
    for (size_t i = 0; i < pages.size(); i++) {
        set_inode(pak, pages[i], i + 1 < pages.size() ? pages[i + 1] : inode_last);
        std::memset(&pak.data[page_offset(pages[i])], 0, page_size);
    }
    update_inode_checksums(pak);
    return pages;
}

// Moves the pak's files, in the same note slots, to a freshly formatted pak of `banks` banks.
bool resize(Pak& pak, int port, int banks) {
    Pak resized;
    resized.loaded = true;
    format(resized, port, banks);
    for (int i = 0; i < num_notes; i++) {
        if (!note_used(pak, i)) {
            continue;
        }
        std::vector<Page> from = file_pages(pak, note_start(pak, i));
        std::vector<Page> to = allocate_pages(resized, (int)from.size());
        if (to.empty()) {
            return false;
        }
        for (size_t k = 0; k < from.size(); k++) {
            std::memcpy(&resized.data[page_offset(to[k])], &pak.data[page_offset(from[k])], page_size);
        }
        uint8_t* n = note(resized, i);
        std::memcpy(n, note(pak, i), note_size);
        write16(n + 6, to[0]);
    }
    pak = std::move(resized);
    return true;
}

std::filesystem::path follow_links(std::filesystem::path path) {
    std::error_code ec;
    // 40 is Linux's own limit on a chain of links; it also ends a loop of them.
    for (int i = 0; i < 40 && std::filesystem::is_symlink(path, ec); i++) {
        std::filesystem::path target = std::filesystem::read_symlink(path, ec);
        if (ec) {
            break;
        }
        path = target.is_absolute() ? target : path.parent_path() / target;
    }
    return path;
}

// Writes the file in one piece: to <file>.tmp beside the link's target, renamed over it.
std::string replace_file(const std::filesystem::path& path, const void* data, size_t size) {
    std::filesystem::path target = follow_links(path);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream file{ temp, std::ios::binary };
        file.write(static_cast<const char*>(data), (std::streamsize)size);
        if (!file.good()) {
            return "could not write " + temp.string();
        }
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        return "could not replace " + target.string() + ": " + ec.message();
    }
    return {};
}

void save(int port) {
    const Pak& pak = paks[port];
    std::string error = replace_file(pak_path(port), pak.data.data(), pak.data.size());
    if (!error.empty()) {
        fprintf(stderr, "[pak] %s\n", error.c_str());
    }
}

Pak& get_pak(int port) {
    Pak& pak = paks[port];
    if (!pak.loaded) {
        std::error_code ec;
        std::filesystem::path path = pak_path(port);
        uintmax_t size = std::filesystem::file_size(path, ec);
        bool good = false;
        if (!ec && size >= bank_size && size <= max_banks * bank_size && size % bank_size == 0) {
            pak.banks = (int)(size / bank_size);
            pak.data.resize(size);
            std::ifstream file{ path, std::ios::binary };
            file.read(reinterpret_cast<char*>(pak.data.data()), (std::streamsize)size);
            good = file.good() && pak.data[id_offset + 0x1A] == pak.banks;
        }
        if (!good) {
            int banks = wanted_banks(port);
            format(pak, port, banks == 0 ? 1 : banks);
        }
        pak.loaded = true;
    }
    return pak;
}

// The pak's port, from OSPfs.channel.
int pfs_port(uint8_t* rdram, gpr pfs) {
    return MEM_W(0x08, pfs) & 3;
}

void read_guest(uint8_t* rdram, gpr addr, uint8_t* out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        out[i] = (uint8_t)MEM_B(i, addr);
    }
}

void write_guest(uint8_t* rdram, gpr addr, const uint8_t* in, size_t count) {
    for (size_t i = 0; i < count; i++) {
        MEM_B(i, addr) = (int8_t)in[i];
    }
}

int find_note(uint8_t* rdram, Pak& pak, uint16_t company, uint32_t game_code, gpr game_name, gpr ext_name) {
    uint8_t name[16];
    uint8_t ext[4];
    read_guest(rdram, game_name, name, sizeof(name));
    read_guest(rdram, ext_name, ext, sizeof(ext));
    for (int i = 0; i < num_notes; i++) {
        uint8_t* n = note(pak, i);
        if (note_used(pak, i) && read32(n) == game_code && read16(n + 4) == company &&
            std::memcmp(n + 0x10, name, sizeof(name)) == 0 && std::memcmp(n + 0x0C, ext, sizeof(ext)) == 0) {
            return i;
        }
    }
    return -1;
}

} // namespace

// s32 osPfsInitPak(OSMesgQueue* mq, OSPfs* pfs, int channel)
extern "C" void osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr pfs = ctx->r5;
    int channel = (int)ctx->r6;
    if (!has_pak(channel)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    int banks = wanted_banks(channel);
    if (banks == 0) {
        // A Rumble Pak in the slot: libultra finds an accessory there, fills in status, queue and channel, and fails
        // to read a pak's ID from it. Games tell this from an empty slot by the error, or by the queue it filled in.
        MEM_W(0x00, pfs) = 0;                // status
        MEM_W(0x04, pfs) = (int32_t)ctx->r4; // queue
        MEM_W(0x08, pfs) = channel;          // channel
        ctx->r2 = PFS_ERR_DEVICE;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    Pak& pak = get_pak(channel);

    if (pak.banks != banks) {
        int from = pak.banks;
        if (resize(pak, channel, banks)) {
            fprintf(stderr, "[pak] port %d: %d bank(s) -> %d\n", channel + 1, from, banks);
            save(channel);
        }
        else {
            fprintf(stderr, "[pak] port %d: its files don't fit in %d bank(s), keeping %d\n", channel + 1, banks, from);
        }
    }

    // OSPfs fields as libultra fills them. Games compare id across ports to notice a pak moving between controllers,
    // so it has to be the pak's own ID block.
    MEM_W(0x00, pfs) = PFS_INITIALIZED;   // status
    MEM_W(0x04, pfs) = (int32_t)ctx->r4; // queue
    MEM_W(0x08, pfs) = channel;           // channel
    write_guest(rdram, pfs + 0x0C, &pak.data[id_offset], 32); // id
    write_guest(rdram, pfs + 0x2C, &pak.data[0x00], 32); // label
    MEM_W(0x4C, pfs) = pak.data[id_offset + 0x1B]; // version
    MEM_W(0x50, pfs) = num_notes;         // dir_size
    // Table offsets in 32-byte blocks.
    MEM_W(0x54, pfs) = (int32_t)(inode_table_offset(pak, 0) / block_size);   // inode_table
    MEM_W(0x58, pfs) = (int32_t)(inode_backup_offset(pak, 0) / block_size);  // minode_table
    MEM_W(0x5C, pfs) = (int32_t)(note_table_offset(pak) / block_size);       // dir_table
    MEM_W(0x60, pfs) = first_data_page(pak, 0); // inode_start_page
    MEM_B(0x64, pfs) = (int8_t)pak.banks; // banks
    MEM_B(0x65, pfs) = 0;                 // activebank
    ctx->r2 = 0;
}

// s32 osPfsRepairId(OSPfs* pfs)
extern "C" void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = has_pak(pfs_port(rdram, ctx->r4)) ? 0 : PFS_ERR_NOPACK;
}

// s32 osPfsChecker(OSPfs* pfs)
extern "C" void osPfsChecker_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = has_pak(pfs_port(rdram, ctx->r4)) ? 0 : PFS_ERR_NOPACK;
}

// s32 osPfsFreeBlocks(OSPfs* pfs, s32* bytes_not_used)
extern "C" void osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx) {
    int port = pfs_port(rdram, ctx->r4);
    if (!has_pak(port)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    MEM_W(0, ctx->r5) = free_pages(get_pak(port)) * (int32_t)page_size;
    ctx->r2 = 0;
}

// s32 osPfsNumFiles(OSPfs* pfs, s32* max_files, s32* files_used)
extern "C" void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx) {
    int port = pfs_port(rdram, ctx->r4);
    if (!has_pak(port)) {
        MEM_W(0, ctx->r5) = 0;
        MEM_W(0, ctx->r6) = 0;
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    Pak& pak = get_pak(port);
    int used = 0;
    for (int i = 0; i < num_notes; i++) {
        used += note_used(pak, i) ? 1 : 0;
    }
    MEM_W(0, ctx->r5) = num_notes;
    MEM_W(0, ctx->r6) = used;
    ctx->r2 = 0;
}

// s32 osPfsFileState(OSPfs* pfs, s32 file_no, OSPfsState* state)
extern "C" void osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx) {
    int port = pfs_port(rdram, ctx->r4);
    if (!has_pak(port)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    Pak& pak = get_pak(port);
    int file_no = (int)ctx->r5;
    gpr state = ctx->r6;
    if (file_no < 0 || file_no >= num_notes || !note_used(pak, file_no)) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    uint8_t* n = note(pak, file_no);
    MEM_W(0x00, state) = (int32_t)(file_pages(pak, note_start(pak, file_no)).size() * page_size); // file_size
    MEM_W(0x04, state) = (int32_t)read32(n);                         // game_code
    MEM_H(0x08, state) = (int16_t)read16(n + 4);                     // company_code
    write_guest(rdram, state + 0x0A, n + 0x0C, 4);                   // ext_name
    write_guest(rdram, state + 0x0E, n + 0x10, 16);                  // game_name
    ctx->r2 = 0;
}

// s32 osPfsFindFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name, s32* file_no)
extern "C" void osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    int port = pfs_port(rdram, ctx->r4);
    if (!has_pak(port)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    Pak& pak = get_pak(port);
    gpr ext_name = (gpr)(int32_t)MEM_W(0x10, ctx->r29);
    gpr file_no = (gpr)(int32_t)MEM_W(0x14, ctx->r29);
    int found = find_note(rdram, pak, (uint16_t)ctx->r5, (uint32_t)ctx->r6, ctx->r7, ext_name);
    if (found < 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    MEM_W(0, file_no) = found;
    ctx->r2 = 0;
}

// s32 osPfsAllocateFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name,
//                       int file_size_in_bytes, s32* file_no)
extern "C" void osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    int port = pfs_port(rdram, ctx->r4);
    if (!has_pak(port)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    Pak& pak = get_pak(port);
    uint16_t company = (uint16_t)ctx->r5;
    uint32_t game_code = (uint32_t)ctx->r6;
    gpr game_name = ctx->r7;
    gpr ext_name = (gpr)(int32_t)MEM_W(0x10, ctx->r29);
    int size = (int)MEM_W(0x14, ctx->r29);
    gpr file_no = (gpr)(int32_t)MEM_W(0x18, ctx->r29);

    if (company == 0 || game_code == 0 || size <= 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    if (find_note(rdram, pak, company, game_code, game_name, ext_name) >= 0) {
        ctx->r2 = PFS_ERR_EXIST;
        return;
    }
    int slot = -1;
    for (int i = 0; i < num_notes && slot < 0; i++) {
        if (!note_used(pak, i)) {
            slot = i;
        }
    }
    if (slot < 0) {
        ctx->r2 = PFS_DIR_FULL;
        return;
    }
    std::vector<Page> pages = allocate_pages(pak, (int)((size + page_size - 1) / page_size));
    if (pages.empty()) {
        ctx->r2 = PFS_DATA_FULL;
        return;
    }

    uint8_t* n = note(pak, slot);
    std::memset(n, 0, note_size);
    write32(n, game_code);
    write16(n + 4, company);
    write16(n + 6, pages[0]); // start bank and page
    n[8] = 0x02;              // status: occupied
    read_guest(rdram, ext_name, n + 0x0C, 4);
    read_guest(rdram, game_name, n + 0x10, 16);

    save(port);
    MEM_W(0, file_no) = slot;
    ctx->r2 = 0;
}

// s32 osPfsDeleteFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name)
extern "C" void osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    int port = pfs_port(rdram, ctx->r4);
    if (!has_pak(port)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    Pak& pak = get_pak(port);
    gpr ext_name = (gpr)(int32_t)MEM_W(0x10, ctx->r29);
    int found = find_note(rdram, pak, (uint16_t)ctx->r5, (uint32_t)ctx->r6, ctx->r7, ext_name);
    if (found < 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }

    for (Page page : file_pages(pak, note_start(pak, found))) {
        set_inode(pak, page, inode_free);
    }
    update_inode_checksums(pak);
    std::memset(note(pak, found), 0, note_size);

    save(port);
    ctx->r2 = 0;
}

// s32 osPfsReadWriteFile(OSPfs* pfs, s32 file_no, u8 flag, int offset, int size_in_bytes, u8* data_buffer)
// flag: 0 = PFS_READ, 1 = PFS_WRITE.
extern "C" void osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    int port = pfs_port(rdram, ctx->r4);
    if (!has_pak(port)) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{ pak_mutex };
    Pak& pak = get_pak(port);
    int file_no = (int)ctx->r5;
    bool write = (ctx->r6 & 0xFF) == 1;
    int offset = (int)ctx->r7;
    int size = (int)MEM_W(0x10, ctx->r29);
    gpr buffer = (gpr)(int32_t)MEM_W(0x14, ctx->r29);

    if (file_no < 0 || file_no >= num_notes || !note_used(pak, file_no) || offset < 0 || size < 0 ||
        offset % block_size != 0 || size % block_size != 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }

    std::vector<Page> pages = file_pages(pak, note_start(pak, file_no));
    if ((size_t)offset + size > pages.size() * page_size) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }

    for (int i = 0; i < size; i++) {
        size_t file_pos = (size_t)offset + i;
        uint8_t& byte = pak.data[page_offset(pages[file_pos / page_size]) + file_pos % page_size];
        if (write) {
            byte = (uint8_t)MEM_B(i, buffer);
        }
        else {
            MEM_B(i, buffer) = (int8_t)byte;
        }
    }

    if (write) {
        save(port);
    }
    ctx->r2 = 0;
}
