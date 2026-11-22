// recomp::for_each_rom_in_archive (rom_archive.hpp). An archive is known by its first bytes, never by its name: .zip
// and .gz with miniz (already here for mods), .7z and .xz with the LZMA SDK's decoders (thirdparty/lzma-sdk), and
// .tar read here. A .gz or .xz holds one file, which is a ROM or a .tar of them. Each file's size is read from the
// archive before it is unpacked, so an archive of other things costs its directory, and nothing unpacks to more than
// max_unpacked, so a damaged or hostile archive can't take the memory.

#include "rom_archive.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "miniz.h"

extern "C" {
#include "7z.h"
#include "7zCrc.h"
#include "Xz.h"
#include "XzCrc64.h"
}

namespace {

constexpr size_t min_rom_size = 4 << 10; // a ROM's header and boot code
constexpr size_t max_rom_size = 64 << 20; // the cartridge bus's largest
constexpr size_t max_unpacked = 256 << 20; // a .gz's or .xz's file, or a 7z block of several files

using FoundFunc = std::function<bool(std::vector<uint8_t>&&)>;

// check_rom_start's three byte orders (recomp.cpp).
bool starts_as_rom(const uint8_t* data, size_t size) {
    if (size < 4) {
        return false;
    }
    uint32_t start = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
    return start == 0x80371240 || start == 0x37804012 || start == 0x40123780;
}

bool rom_sized(uint64_t size) {
    return size >= min_rom_size && size <= max_rom_size;
}

bool starts_with(std::span<const uint8_t> data, std::initializer_list<uint8_t> magic) {
    return data.size() >= magic.size() && std::equal(magic.begin(), magic.end(), data.begin());
}

bool scan(std::span<const uint8_t> data, int depth, const FoundFunc& found, bool& stop);

// A .gz's or .xz's one file: a .tar of ROMs, or a ROM.
void scan_unpacked(std::vector<uint8_t>&& unpacked, int depth, const FoundFunc& found, bool& stop) {
    if (scan(unpacked, depth + 1, found, stop)) {
        return;
    }
    if (rom_sized(unpacked.size()) && starts_as_rom(unpacked.data(), unpacked.size())) {
        stop = found(std::move(unpacked));
    }
}

void scan_zip(std::span<const uint8_t> data, const FoundFunc& found, bool& stop) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) {
        return;
    }
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && !stop; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory || stat.m_is_encrypted ||
            !stat.m_is_supported || !rom_sized(stat.m_uncomp_size)) {
            continue;
        }
        // Its first bytes before all of it, so a large file that isn't a ROM isn't unpacked.
        uint8_t start[4];
        mz_zip_reader_extract_iter_state* iter = mz_zip_reader_extract_iter_new(&zip, i, 0);
        if (iter == nullptr) {
            continue;
        }
        size_t read = mz_zip_reader_extract_iter_read(iter, start, sizeof(start));
        mz_zip_reader_extract_iter_free(iter);
        if (!starts_as_rom(start, read)) {
            continue;
        }
        std::vector<uint8_t> rom(stat.m_uncomp_size);
        if (mz_zip_reader_extract_to_mem(&zip, i, rom.data(), rom.size(), 0)) {
            stop = found(std::move(rom));
        }
    }
    mz_zip_reader_end(&zip);
}

// RFC 1952: the header, a raw deflate stream, then CRC-32 and the size mod 2^32. One member, as gzip writes.
void scan_gzip(std::span<const uint8_t> data, int depth, const FoundFunc& found, bool& stop) {
    if (data.size() < 18 || data[2] != 8) {
        return;
    }
    uint8_t flags = data[3];
    size_t pos = 10;
    if (flags & 0x04) { // FEXTRA
        if (pos + 2 > data.size()) {
            return;
        }
        pos += 2 + (data[pos] | (data[pos + 1] << 8));
    }
    for (uint8_t flag : { uint8_t(0x08), uint8_t(0x10) }) { // FNAME, FCOMMENT: zero-terminated
        if (flags & flag) {
            while (pos < data.size() && data[pos] != 0) {
                pos++;
            }
            pos++;
        }
    }
    if (flags & 0x02) { // FHCRC
        pos += 2;
    }
    if (pos + 8 > data.size()) {
        return;
    }
    const uint8_t* tail = data.data() + data.size() - 8;
    uint32_t crc = tail[0] | (tail[1] << 8) | (tail[2] << 16) | (uint32_t(tail[3]) << 24);
    size_t size = tail[4] | (tail[5] << 8) | (tail[6] << 16) | (size_t(tail[7]) << 24);
    if (size == 0 || size > max_unpacked) {
        return;
    }
    std::vector<uint8_t> unpacked(size);
    size_t in_size = data.size() - 8 - pos;
    size_t out_size = size;
    tinfl_decompressor inflator;
    tinfl_init(&inflator);
    tinfl_status status = tinfl_decompress(&inflator, data.data() + pos, &in_size, unpacked.data(), unpacked.data(),
        &out_size, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE || out_size != size || mz_crc32(MZ_CRC32_INIT, unpacked.data(), size) != crc) {
        return;
    }
    scan_unpacked(std::move(unpacked), depth, found, stop);
}

void* sz_alloc(ISzAllocPtr, size_t size) {
    return std::malloc(size);
}

void sz_free(ISzAllocPtr, void* address) {
    std::free(address);
}

const ISzAlloc sz_allocator = { sz_alloc, sz_free };

void init_crc_tables() {
    static std::once_flag once;
    std::call_once(once, []() {
        CrcGenerateTable();
        Crc64GenerateTable();
    });
}

void scan_xz(std::span<const uint8_t> data, int depth, const FoundFunc& found, bool& stop) {
    init_crc_tables();
    CXzUnpacker xz;
    XzUnpacker_Construct(&xz, &sz_allocator);
    XzUnpacker_Init(&xz);
    std::vector<uint8_t> unpacked;
    size_t in_pos = 0;
    size_t out_pos = 0;
    bool ok = false;
    while (true) {
        if (unpacked.size() - out_pos < (1 << 20)) {
            if (unpacked.size() >= max_unpacked) {
                break;
            }
            unpacked.resize(std::min(max_unpacked, unpacked.size() + (8 << 20)));
        }
        SizeT out_len = unpacked.size() - out_pos;
        SizeT in_len = data.size() - in_pos;
        ECoderStatus status;
        SRes result = XzUnpacker_Code(&xz, unpacked.data() + out_pos, &out_len, data.data() + in_pos, &in_len, 1,
            CODER_FINISH_ANY, &status);
        in_pos += in_len;
        out_pos += out_len;
        if (result != SZ_OK) {
            break;
        }
        if (in_len == 0 && out_len == 0) {
            ok = in_pos == data.size() && XzUnpacker_IsStreamWasFinished(&xz);
            break;
        }
    }
    XzUnpacker_Free(&xz);
    if (ok) {
        unpacked.resize(out_pos);
        scan_unpacked(std::move(unpacked), depth, found, stop);
    }
}

struct MemoryInStream {
    ISeekInStream vt;
    std::span<const uint8_t> data;
    size_t pos;
};

SRes memory_read(ISeekInStreamPtr p, void* buf, size_t* size) {
    MemoryInStream* stream = Z7_CONTAINER_FROM_VTBL(p, MemoryInStream, vt);
    *size = std::min(*size, stream->data.size() - stream->pos);
    std::memcpy(buf, stream->data.data() + stream->pos, *size);
    stream->pos += *size;
    return SZ_OK;
}

SRes memory_seek(ISeekInStreamPtr p, Int64* pos, ESzSeek origin) {
    MemoryInStream* stream = Z7_CONTAINER_FROM_VTBL(p, MemoryInStream, vt);
    Int64 base = origin == SZ_SEEK_SET ? 0 : origin == SZ_SEEK_CUR ? Int64(stream->pos) : Int64(stream->data.size());
    Int64 target = base + *pos;
    if (target < 0 || target > Int64(stream->data.size())) {
        return SZ_ERROR_PARAM;
    }
    stream->pos = size_t(target);
    *pos = target;
    return SZ_OK;
}

void scan_7z(std::span<const uint8_t> data, const FoundFunc& found, bool& stop) {
    init_crc_tables();
    MemoryInStream memory{ { memory_read, memory_seek }, data, 0 };
    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    std::vector<Byte> look_buffer(1 << 16);
    look.buf = look_buffer.data();
    look.bufSize = look_buffer.size();
    look.realStream = &memory.vt;
    LookToRead2_INIT(&look);

    CSzArEx db;
    SzArEx_Init(&db);
    if (SzArEx_Open(&db, &look.vt, &sz_allocator, &sz_allocator) == SZ_OK) {
        UInt32 block_index = 0xFFFFFFFF;
        Byte* block = nullptr;
        size_t block_size = 0;
        for (UInt32 i = 0; i < db.NumFiles && !stop; i++) {
            UInt32 folder = db.FileToFolder[i];
            if (SzArEx_IsDir(&db, i) || folder == (UInt32)-1 || !rom_sized(SzArEx_GetFileSize(&db, i)) ||
                SzAr_GetFolderUnpackSize(&db.db, folder) > max_unpacked) {
                continue;
            }
            size_t offset = 0;
            size_t size = 0;
            if (SzArEx_Extract(&db, &look.vt, i, &block_index, &block, &block_size, &offset, &size, &sz_allocator,
                    &sz_allocator) != SZ_OK) {
                break;
            }
            if (starts_as_rom(block + offset, size)) {
                stop = found(std::vector<uint8_t>(block + offset, block + offset + size));
            }
        }
        ISzAlloc_Free(&sz_allocator, block);
    }
    SzArEx_Free(&db, &sz_allocator);
}

uint64_t tar_number(const uint8_t* field, size_t length) {
    uint64_t value = 0;
    for (size_t i = 0; i < length && field[i] >= '0' && field[i] <= '7'; i++) {
        value = value * 8 + (field[i] - '0');
    }
    return value;
}

// POSIX ustar (GNU's too): 512-byte headers, each file's data padded to 512. Long names (GNU 'L', pax 'x') are
// records of their own, skipped like any file that isn't a regular one.
void scan_tar(std::span<const uint8_t> data, const FoundFunc& found, bool& stop) {
    size_t pos = 0;
    while (!stop && pos + 512 <= data.size()) {
        const uint8_t* header = data.data() + pos;
        if (header[0] == 0) {
            break;
        }
        uint64_t size = tar_number(header + 124, 12);
        uint8_t type = header[156];
        pos += 512;
        if (size > data.size() - pos) {
            break;
        }
        if ((type == '0' || type == 0 || type == '7') && rom_sized(size) && starts_as_rom(data.data() + pos, size)) {
            stop = found(std::vector<uint8_t>(data.data() + pos, data.data() + pos + size));
        }
        pos += (size + 511) & ~uint64_t(511);
    }
}

// Whether data is an archive (and so was searched).
bool scan(std::span<const uint8_t> data, int depth, const FoundFunc& found, bool& stop) {
    if (starts_with(data, { 'P', 'K', 3, 4 }) || starts_with(data, { 'P', 'K', 5, 6 })) {
        scan_zip(data, found, stop);
    }
    else if (starts_with(data, { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C })) {
        scan_7z(data, found, stop);
    }
    else if (depth == 0 && starts_with(data, { 0x1F, 0x8B })) {
        scan_gzip(data, depth, found, stop);
    }
    else if (depth == 0 && starts_with(data, { 0xFD, '7', 'z', 'X', 'Z', 0 })) {
        scan_xz(data, depth, found, stop);
    }
    else if (data.size() >= 512 && std::memcmp(data.data() + 257, "ustar", 5) == 0) {
        scan_tar(data, found, stop);
    }
    else {
        return false;
    }
    return true;
}

} // namespace

bool recomp::for_each_rom_in_archive(std::span<const uint8_t> data, const FoundFunc& found) {
    bool stop = false;
    return scan(data, 0, found, stop);
}
