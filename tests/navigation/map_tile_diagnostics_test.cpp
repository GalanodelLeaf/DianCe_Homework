#include "../../components/lvgl_ui/tileview/map_tile_diagnostics.h"

#include <stdlib.h>
#include <string.h>

namespace {
unsigned checks = 0;

void require(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        exit(1);
    }
}
#define CHECK(expression) require((expression), #expression, __LINE__)

uint8_t tile[mapdiag::tile_bytes];
uint8_t buffer[mapdiag::tile_bytes];

void put_pixel(size_t index, uint16_t value) {
    tile[index * 2] = static_cast<uint8_t>(value);
    tile[index * 2 + 1] = static_cast<uint8_t>(value >> 8);
}

void fill(uint16_t value) {
    for (size_t i = 0; i < mapdiag::tile_bytes / 2; ++i) put_pixel(i, value);
}

FILE* temporary(const uint8_t* bytes, size_t length, bool extra = false) {
    // Host CRT temporary files only. No device path, SD path, or backup file.
    FILE* file = tmpfile();
#if defined(_MSC_VER)
    // UCRT tmpfile may use a drive root that is not writable. Keep its fallback
    // in the current English host workspace; D deletes it when fclose runs.
    if (file == NULL) {
        char* name = _tempnam(".", "mapdiag-");
        CHECK(name != NULL);
        file = fopen(name, "w+bxD"); // exclusive create: never overwrite a file
        if (file == NULL) perror("host temporary fallback");
        free(name);
    }
#endif
    CHECK(file != NULL);
    CHECK(fwrite(bytes, 1, length, file) == length);
    if (extra) CHECK(fputc(0x5a, file) == 0x5a);
    CHECK(fflush(file) == 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    return file;
}

void close_file(FILE* file) { CHECK(fclose(file) == 0); }

void test_stats_basics() {
    const mapdiag::PixelStats empty = mapdiag::stats(NULL, 0);
    CHECK(empty.count == 0 && empty.hash == UINT32_C(2166136261));
    CHECK(empty.first[0] == 0 && empty.first[7] == 0);
    CHECK(empty.sample[0] == 0 && empty.sample[63] == 0);
    CHECK(mapdiag::stats(NULL, 99).count == 0);

    fill(0);
    mapdiag::PixelStats s = mapdiag::stats(tile, sizeof(tile));
    CHECK(s.count == 65536 && s.zero == 65536 && s.ffff == 0);
    CHECK(s.dark_le == 65536 && s.dark_be == 65536);
    CHECK(s.bright_le == 0 && s.bright_be == 0);
    CHECK(s.hash == UINT32_C(0x3b849dc5)); // independent FNV1a known answer
    for (unsigned i = 0; i < 64; ++i) CHECK(s.sample[i] == 0);

    fill(0xffff);
    s = mapdiag::stats(tile, sizeof(tile));
    CHECK(s.count == 65536 && s.ffff == 65536 && s.zero == 0);
    CHECK(s.bright_le == 65536 && s.bright_be == 65536);
    CHECK(s.dark_le == 0 && s.dark_be == 0);
    for (unsigned i = 0; i < 64; ++i) CHECK(s.sample[i] == 0xffff);
    for (unsigned i = 0; i < 8; ++i) CHECK(s.first[i] == 0xff);

    const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
    s = mapdiag::stats(hello, sizeof(hello));
    CHECK(s.count == 2); // odd tail is hashed, not decoded as a pixel
    CHECK(s.hash == UINT32_C(0x4f9f2cab)); // published FNV1a hello vector
    CHECK(memcmp(s.first, hello, sizeof(hello)) == 0);
    CHECK(s.first[5] == 0 && s.first[6] == 0 && s.first[7] == 0);
    CHECK(s.sample[0] == 0 && s.sample[63] == 0);
    CHECK(mapdiag::stats(hello, 4).hash != s.hash);
}

void test_colors_and_byte_order() {
    const uint16_t colors[] = {0, 0xffff, 0xf800, 0x07e0, 0x001f,
                               0x07ff, 0xf81f, 0xffe0};
    for (size_t i = 0; i < 65536; ++i) put_pixel(i, colors[i % 8]);
    mapdiag::PixelStats s = mapdiag::stats(tile, sizeof(tile));
    CHECK(s.count == 65536 && s.zero == 8192 && s.ffff == 8192);
    CHECK(s.bright_le == 8192 && s.dark_le == 8192);
    CHECK(tile[4] == 0 && tile[5] == 0xf8); // stats did not alter red
    const uint32_t original_hash = s.hash;
    CHECK(mapdiag::stats(tile, sizeof(tile)).hash == original_hash);

    fill(0xdefb); // RGB888 (222, 223, 222), bright LE but not bright BE
    s = mapdiag::stats(tile, sizeof(tile));
    CHECK(s.bright_le == 65536 && s.bright_be == 0);
    CHECK(s.dark_le == 0 && s.dark_be == 0);
    for (size_t i = 0; i < sizeof(tile); i += 2) {
        const uint8_t saved = tile[i]; tile[i] = tile[i + 1]; tile[i + 1] = saved;
    }
    mapdiag::PixelStats swapped = mapdiag::stats(tile, sizeof(tile));
    CHECK(swapped.bright_le == s.bright_be && swapped.bright_be == s.bright_le);
    CHECK(swapped.dark_le == s.dark_be && swapped.dark_be == s.dark_le);
    CHECK(swapped.hash != s.hash);

    fill(0x18e3); // RGB888 (24, 28, 24), dark LE but not dark BE
    s = mapdiag::stats(tile, sizeof(tile));
    CHECK(s.dark_le == 65536 && s.dark_be == 0);
    CHECK(s.bright_le == 0 && s.bright_be == 0);
    for (size_t i = 0; i < sizeof(tile); i += 2) {
        const uint8_t saved = tile[i]; tile[i] = tile[i + 1]; tile[i + 1] = saved;
    }
    swapped = mapdiag::stats(tile, sizeof(tile));
    CHECK(swapped.dark_le == s.dark_be && swapped.dark_be == s.dark_le);
    CHECK(swapped.bright_le == s.bright_be && swapped.bright_be == s.bright_le);

    // Explicit channel boundary tests, using RGB565 bit replication to 8 bits.
    const uint16_t boundaries[] = {
        static_cast<uint16_t>((27u << 11) | (55u << 5) | 27u),
        static_cast<uint16_t>((26u << 11) | (55u << 5) | 27u),
        static_cast<uint16_t>((27u << 11) | (54u << 5) | 27u),
        static_cast<uint16_t>((27u << 11) | (55u << 5) | 26u),
        static_cast<uint16_t>((3u << 11) | (8u << 5) | 3u),
        static_cast<uint16_t>((4u << 11) | (8u << 5) | 3u),
        static_cast<uint16_t>((3u << 11) | (9u << 5) | 3u),
        static_cast<uint16_t>((3u << 11) | (8u << 5) | 4u)
    };
    for (size_t i = 0; i < 8; ++i) put_pixel(i, boundaries[i]);
    s = mapdiag::stats(tile, 16);
    CHECK(s.bright_le == 1 && s.dark_le == 1);
}

void test_sampling() {
    for (size_t i = 0; i < 65536; ++i) put_pixel(i, static_cast<uint16_t>(i));
    const mapdiag::PixelStats s = mapdiag::stats(tile, sizeof(tile));
    CHECK(s.count == 65536 && s.zero == 1 && s.ffff == 1);
    const uint8_t first[] = {0, 0, 1, 0, 2, 0, 3, 0};
    CHECK(memcmp(s.first, first, sizeof(first)) == 0);
    for (unsigned row = 0; row < 8; ++row) {
        for (unsigned col = 0; col < 8; ++col) {
            const unsigned expected = (16 + row * 32) * 256 + 16 + col * 32;
            CHECK(s.sample[row * 8 + col] == expected);
        }
    }
    const mapdiag::PixelStats short_stats = mapdiag::stats(tile, sizeof(tile) - 1);
    CHECK(short_stats.count == 65535);
    for (unsigned i = 0; i < 64; ++i) CHECK(short_stats.sample[i] == 0);
}

void test_reader() {
    mapdiag::PixelStats s = mapdiag::stats(tile, sizeof(tile));
    CHECK(mapdiag::read_tile(NULL, buffer, s) == mapdiag::FileStatus::Missing);
    CHECK(s.count == 0 && s.hash == UINT32_C(2166136261));

    fill(0x07e0);
    const mapdiag::PixelStats expected = mapdiag::stats(tile, sizeof(tile));
    FILE* file = temporary(tile, sizeof(tile));
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Ok);
    CHECK(s.count == 65536 && s.hash == expected.hash);
    CHECK(memcmp(buffer, tile, sizeof(tile)) == 0);
    CHECK(feof(file) != 0 && ferror(file) == 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0); // read_tile did not close stream
    CHECK(mapdiag::read_tile(file, NULL, s) == mapdiag::FileStatus::ReadError);
    CHECK(s.count == 0);
    close_file(file);

    file = temporary(tile, sizeof(tile) - 1);
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Short);
    CHECK(s.count == 65535);
    CHECK(s.hash == mapdiag::stats(tile, sizeof(tile) - 1).hash);
    CHECK(memcmp(buffer, tile, sizeof(tile) - 1) == 0);
    close_file(file);

    file = temporary(tile, 0);
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Short);
    CHECK(s.count == 0 && s.hash == UINT32_C(2166136261));
    close_file(file);

    file = temporary(tile, sizeof(tile), true);
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Long);
    CHECK(s.count == 65536 && s.hash == expected.hash);
    CHECK(memcmp(buffer, tile, sizeof(tile)) == 0);
    CHECK(ftell(file) == static_cast<long>(sizeof(tile) + 1));
    close_file(file);

    const uint8_t png[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    file = temporary(png, sizeof(png));
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Unsupported);
    CHECK(s.count == 4 && s.hash == mapdiag::stats(png, sizeof(png)).hash);
    close_file(file);

    const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xe0};
    file = temporary(jpeg, sizeof(jpeg));
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Unsupported);
    CHECK(s.count == 2 && s.hash == mapdiag::stats(jpeg, sizeof(jpeg)).hash);
    close_file(file);

    fill(0);
    memcpy(tile, png, sizeof(png));
    file = temporary(tile, sizeof(tile));
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Unsupported);
    CHECK(s.count == 65536);
    close_file(file);
    file = temporary(tile, sizeof(tile), true);
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Unsupported);
    close_file(file);

    // Do not guess BMP/LVGL or incomplete PNG/JPEG signatures.
    const uint8_t bmp[] = {'B', 'M', 0, 0};
    file = temporary(bmp, sizeof(bmp));
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Short);
    close_file(file);
    file = temporary(png, sizeof(png) - 1);
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Short);
    close_file(file);
    file = temporary(jpeg, 1);
    CHECK(mapdiag::read_tile(file, buffer, s) == mapdiag::FileStatus::Short);
    close_file(file);
}

// Re-read the same stream through both APIs, comparing all destination bytes
// (including untouched short-read tails), validation, and stream side effects.
void compare_readers(const uint8_t* bytes, size_t length,
                     mapdiag::FileStatus expected, bool extra = false,
                     size_t offset = 0) {
    static uint8_t fast_buffer[mapdiag::tile_bytes];
    FILE* file = temporary(bytes, length, extra);
    CHECK(fseek(file, static_cast<long>(offset), SEEK_SET) == 0);
    memset(buffer, 0xa5, sizeof(buffer));
    mapdiag::PixelStats s = {};
    const mapdiag::FileStatus full_status = mapdiag::read_tile(file, buffer, s);
    const long full_position = ftell(file);
    const bool full_eof = feof(file) != 0;
    const bool full_error = ferror(file) != 0;
    CHECK(full_status == expected);
    const size_t available = length - offset;
    const size_t actual = available < sizeof(buffer) ? available : sizeof(buffer);
    const mapdiag::PixelStats expected_stats = mapdiag::stats(bytes + offset, actual);
    CHECK(s.count == expected_stats.count && s.hash == expected_stats.hash);
    CHECK(memcmp(buffer, bytes + offset, actual) == 0);
    if (actual < sizeof(buffer)) CHECK(buffer[actual] == 0xa5);

    CHECK(fseek(file, static_cast<long>(offset), SEEK_SET) == 0);
    memset(fast_buffer, 0xa5, sizeof(fast_buffer));
    CHECK(mapdiag::read_tile_fast(file, fast_buffer) == full_status);
    CHECK(memcmp(fast_buffer, buffer, sizeof(buffer)) == 0);
    CHECK(ftell(file) == full_position);
    CHECK((feof(file) != 0) == full_eof);
    CHECK((ferror(file) != 0) == full_error);
    close_file(file);
}

void test_fast_reader() {
    mapdiag::PixelStats s = {};
    memset(buffer, 0xa5, sizeof(buffer));
    CHECK(mapdiag::read_tile_fast(NULL, buffer) == mapdiag::FileStatus::Missing);
    CHECK(buffer[0] == 0xa5 && buffer[sizeof(buffer) - 1] == 0xa5);
    CHECK(mapdiag::read_tile(NULL, NULL, s) == mapdiag::FileStatus::Missing);
    CHECK(mapdiag::read_tile_fast(NULL, NULL) == mapdiag::FileStatus::Missing);
    CHECK(s.count == 0 && s.hash == UINT32_C(2166136261));

    fill(0x07e0);
    FILE* file = temporary(tile, sizeof(tile));
    CHECK(fseek(file, 7, SEEK_SET) == 0);
    CHECK(mapdiag::read_tile(file, NULL, s) == mapdiag::FileStatus::ReadError);
    CHECK(ftell(file) == 7 && feof(file) == 0 && ferror(file) == 0);
    CHECK(mapdiag::read_tile_fast(file, NULL) == mapdiag::FileStatus::ReadError);
    CHECK(ftell(file) == 7 && feof(file) == 0 && ferror(file) == 0);
    CHECK(s.count == 0 && s.hash == UINT32_C(2166136261));
    close_file(file);

    // Standard RGB565 colors: no byte swapping or pixel correction by fast.
    const uint16_t colors[] = {0, 0xffff, 0xf800, 0x07e0, 0x001f,
                               0x07ff, 0xf81f, 0xffe0};
    for (size_t i = 0; i < 8; ++i) {
        fill(colors[i]);
        compare_readers(tile, sizeof(tile), mapdiag::FileStatus::Ok);
    }
    for (size_t i = 0; i < 65536; ++i) put_pixel(i, colors[i % 8]);
    compare_readers(tile, sizeof(tile), mapdiag::FileStatus::Ok);
    compare_readers(tile, sizeof(tile) - 1, mapdiag::FileStatus::Short);
    compare_readers(tile, 0, mapdiag::FileStatus::Short);
    compare_readers(tile, 1, mapdiag::FileStatus::Short);
    compare_readers(tile, sizeof(tile), mapdiag::FileStatus::Long, true);
    compare_readers(tile, sizeof(tile), mapdiag::FileStatus::Short, false, 7);

    const uint8_t png[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xe0};
    compare_readers(png, sizeof(png), mapdiag::FileStatus::Unsupported);
    compare_readers(jpeg, sizeof(jpeg), mapdiag::FileStatus::Unsupported);
    compare_readers(png, sizeof(png) - 1, mapdiag::FileStatus::Short);
    compare_readers(jpeg, 1, mapdiag::FileStatus::Short);
    const uint8_t bmp[] = {'B', 'M', 0, 0};
    compare_readers(bmp, sizeof(bmp), mapdiag::FileStatus::Short);
    fill(0);
    memcpy(tile, png, sizeof(png));
    compare_readers(tile, sizeof(tile), mapdiag::FileStatus::Unsupported);
    compare_readers(tile, sizeof(tile), mapdiag::FileStatus::Unsupported, true);
    memcpy(tile, jpeg, sizeof(jpeg));
    compare_readers(tile, sizeof(tile), mapdiag::FileStatus::Unsupported);
    compare_readers(tile, sizeof(tile), mapdiag::FileStatus::Unsupported, true);
}

void test_status_names() {
    CHECK(strcmp(mapdiag::status_name(mapdiag::FileStatus::Ok), "ok") == 0);
    CHECK(strcmp(mapdiag::status_name(mapdiag::FileStatus::Missing), "missing") == 0);
    CHECK(strcmp(mapdiag::status_name(mapdiag::FileStatus::Short), "short") == 0);
    CHECK(strcmp(mapdiag::status_name(mapdiag::FileStatus::Long), "long") == 0);
    CHECK(strcmp(mapdiag::status_name(mapdiag::FileStatus::ReadError), "read_error") == 0);
    CHECK(strcmp(mapdiag::status_name(mapdiag::FileStatus::Unsupported), "unsupported") == 0);
}
} // namespace

int main() {
    test_stats_basics();
    test_colors_and_byte_order();
    test_sampling();
    test_reader();
    test_fast_reader();
    test_status_names();
    printf("map_tile_diagnostics: PASS (%u checks)\n", checks);
    return 0;
}
