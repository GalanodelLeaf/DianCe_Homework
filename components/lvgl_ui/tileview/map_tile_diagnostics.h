#ifndef MAP_TILE_DIAGNOSTICS_H
#define MAP_TILE_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Read-only diagnostics for one 256 x 256, raw RGB565 map tile.
// No LVGL, ESP, filesystem paths, byte swapping, or pixel correction is involved.
namespace mapdiag {

constexpr size_t tile_bytes = 131072;

enum class FileStatus { Ok, Missing, Short, Long, ReadError, Unsupported };

inline const char* status_name(FileStatus status) {
    switch (status) {
    case FileStatus::Ok: return "ok";
    case FileStatus::Missing: return "missing";
    case FileStatus::Short: return "short";
    case FileStatus::Long: return "long";
    case FileStatus::ReadError: return "read_error";
    case FileStatus::Unsupported: return "unsupported";
    }
    return "unknown";
}

struct PixelStats {
    unsigned count;
    unsigned zero;
    unsigned ffff;
    unsigned bright_le;
    unsigned dark_le;
    unsigned bright_be;
    unsigned dark_be;
    uint32_t hash;
    uint8_t first[8];
    uint16_t sample[64];
};

namespace detail {
inline uint16_t little_pixel(const uint8_t* bytes) {
    return static_cast<uint16_t>(static_cast<uint16_t>(bytes[0]) |
                                 (static_cast<uint16_t>(bytes[1]) << 8));
}

inline void classify(uint16_t pixel, unsigned& bright, unsigned& dark) {
    const unsigned r5 = (pixel >> 11) & 31u;
    const unsigned g6 = (pixel >> 5) & 63u;
    const unsigned b5 = pixel & 31u;
    // Standard RGB565 -> RGB888 bit replication, not an average/luma threshold.
    const unsigned r = (r5 << 3) | (r5 >> 2);
    const unsigned g = (g6 << 2) | (g6 >> 4);
    const unsigned b = (b5 << 3) | (b5 >> 2);
    if (r >= 220u && g >= 220u && b >= 220u) ++bright;
    if (r <= 32u && g <= 32u && b <= 32u) ++dark;
}

inline bool compressed_signature(const uint8_t* bytes, size_t len) {
    const uint8_t png[8] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    bool is_png = len >= 8;
    for (size_t i = 0; is_png && i < 8; ++i) {
        if (bytes[i] != png[i]) is_png = false;
    }
    return is_png || (len >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8);
}

// Shared validation for normal loading and diagnostics. length excludes the
// extra probe byte and remains available even when validation fails.
inline FileStatus read_and_validate(FILE* file, uint8_t* buffer, size_t& length) {
    length = 0;
    if (file == NULL) return FileStatus::Missing;
    if (buffer == NULL) return FileStatus::ReadError;

    length = fread(buffer, 1, tile_bytes, file);
    if (ferror(file)) return FileStatus::ReadError;
    if (compressed_signature(buffer, length)) return FileStatus::Unsupported;
    if (length < tile_bytes) return FileStatus::Short;

    const int extra = fgetc(file);
    if (ferror(file)) return FileStatus::ReadError;
    return extra == EOF ? FileStatus::Ok : FileStatus::Long;
}
} // namespace detail

// Counts complete pixels only; the hash includes every byte, even an odd tail.
// first[] is zero padded. sample[] remains zero unless a full tile is available.
// A null data pointer is treated as an empty input (regardless of len).
inline PixelStats stats(const uint8_t* bytes, size_t len) {
    PixelStats result = {};
    result.hash = UINT32_C(2166136261);
    if (bytes == NULL) return result;

    for (size_t i = 0; i < len; ++i) {
        result.hash ^= bytes[i];
        result.hash *= UINT32_C(16777619);
        if (i < 8) result.first[i] = bytes[i];
    }
    for (size_t i = 0; i + 1 < len; i += 2) {
        const uint16_t le = detail::little_pixel(bytes + i);
        const uint16_t be = static_cast<uint16_t>(
            (static_cast<uint16_t>(bytes[i]) << 8) | bytes[i + 1]);
        ++result.count;
        if (le == 0) ++result.zero;
        if (le == 0xffff) ++result.ffff;
        detail::classify(le, result.bright_le, result.dark_le);
        detail::classify(be, result.bright_be, result.dark_be);
    }
    if (len >= tile_bytes) {
        for (size_t row = 0; row < 8; ++row) {
            for (size_t col = 0; col < 8; ++col) {
                const size_t x = 16 + 32 * col;
                const size_t y = 16 + 32 * row;
                result.sample[row * 8 + col] =
                    detail::little_pixel(bytes + 2 * (y * 256 + x));
            }
        }
    }
    return result;
}

// Reads from the current stream position; ownership/closing stays with caller.
// out always describes the bytes fread actually supplied (at most tile_bytes),
// including failed/unsupported reads. An extra probe byte is not part of out.
// buffer must point to tile_bytes writable bytes. A null buffer is ReadError.
// I/O errors take precedence over format/length; PNG/JPEG precede short/long.
inline FileStatus read_tile(FILE* file, uint8_t* buffer, PixelStats& out) {
    size_t length = 0;
    const FileStatus status = detail::read_and_validate(file, buffer, length);
    out = stats(buffer, length);
    return status;
}

// Same stream, buffer, and validation contract as read_tile, but without
// PixelStats, byte hashing, sampling, or pixel classification work.
inline FileStatus read_tile_fast(FILE* file, uint8_t* buffer) {
    size_t length = 0;
    return detail::read_and_validate(file, buffer, length);
}

} // namespace mapdiag

#endif // MAP_TILE_DIAGNOSTICS_H
