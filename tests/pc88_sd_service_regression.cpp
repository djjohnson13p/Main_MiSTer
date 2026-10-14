// Ordinary in-memory service boundary. No mounted files, device or full poll.
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <strings.h>
#include <sys/types.h>
#include <type_traits>
#include <utility>
#include <vector>
#ifndef __linux__
typedef off_t __off64_t; // Native host spelling adapter; field text stays actual.
#endif
#include "pc88_types.inc"
static_assert(sizeof(__off64_t) == 8 && std::is_signed<__off64_t>::value, "signed64 file offsets");
#include "pc88_presence.h"
#include "pc88_api.h"
static_assert(UIO_BUFFER_SIZE == 16384, "ordinary maximum transport boundary");

struct MemoryDisk { std::vector<uint8_t> bytes; };
static MemoryDisk media[16];
static std::vector<std::string> trace, stages, failures;
static std::vector<uint8_t> incoming, outgoing;
static bool io_enabled = false;
static int assertions = 0;
#include "pc88_globals.inc"
#include "pc88_providers.inc"

static void require(bool value, const char *label)
{
    ++assertions;
    if (!value) failures.emplace_back(label);
}
static std::string hex(const std::vector<uint8_t>& bytes)
{
    const char *digits = "0123456789abcdef";
    std::string result; result.reserve(bytes.size() * 2);
    for (uint8_t value : bytes) { result += digits[value >> 4]; result += digits[value & 15]; }
    return result;
}
static std::string quoted(const std::string& value) { return "\"" + value + "\""; }
static std::string strings(const std::vector<std::string>& values)
{
    std::string result = "[";
    for (size_t i = 0; i < values.size(); ++i) { if (i) result += ','; result += quoted(values[i]); }
    return result + "]";
}
static int slot(fileTYPE *file)
{
    for (int i = 0; i < 16; ++i) if (file == &sd_image[i]) return i;
    throw std::logic_error("unbound in-memory file");
}
int FileClose(fileTYPE *file) { file->filp = nullptr; file->zip = nullptr; return 0; }
int FileOpenEx(fileTYPE *, const char *, int, char, int)
{ throw std::logic_error("unexecuted generic type2 file creation"); }
void menu_process_save() { throw std::logic_error("unexecuted generic use_save route"); }
int FileSeek(fileTYPE *file, __off64_t offset, int origin)
{
    if (origin != SEEK_SET || offset < 0) throw std::logic_error("nonordinary seek");
    trace.push_back("seek:" + std::to_string(slot(file)) + ":" + std::to_string(offset));
    file->offset = offset; return 1;
}
int FileReadAdv(fileTYPE *file, void *data, int length, int)
{
    auto &bytes = media[slot(file)].bytes;
    const size_t off = static_cast<size_t>(file->offset);
    const size_t n = std::min(static_cast<size_t>(length), off < bytes.size() ? bytes.size() - off : size_t(0));
    if (n) std::memcpy(data, bytes.data() + off, n);
    trace.push_back("read:" + std::to_string(slot(file)) + ":" + std::to_string(off) + ":" + std::to_string(n));
    file->offset += static_cast<__off64_t>(n); return static_cast<int>(n);
}
int FileWriteAdv(fileTYPE *file, void *data, int length, int)
{
    auto &bytes = media[slot(file)].bytes;
    const size_t off = static_cast<size_t>(file->offset);
    if (bytes.size() < off + static_cast<size_t>(length)) bytes.resize(off + length, 0);
    if (length) std::memcpy(bytes.data() + off, data, static_cast<size_t>(length));
    trace.push_back("write:" + std::to_string(slot(file)) + ":" + std::to_string(off) + ":" + std::to_string(length));
    file->offset += length; file->size = static_cast<__off64_t>(bytes.size()); return length;
}
void diskled_on() { trace.emplace_back("led"); }
void EnableIO() { if (io_enabled) throw std::logic_error("nested chip select"); io_enabled = true; trace.emplace_back("enable"); }
void DisableIO() { if (!io_enabled) throw std::logic_error("unselected chip release"); io_enabled = false; trace.emplace_back("disable"); }
uint16_t fpga_spi(uint16_t word)
{ if (!io_enabled) throw std::logic_error("unselected SPI word"); trace.push_back("word:" + std::to_string(word)); return 0; }
void spi_block_read(uint8_t *data, int wide, int size)
{
    if (!io_enabled || incoming.size() != static_cast<size_t>(size)) throw std::logic_error("ordinary write frame mismatch");
    std::memcpy(data, incoming.data(), static_cast<size_t>(size));
    trace.push_back("spi-read:" + std::to_string(wide) + ":" + std::to_string(size));
}
void spi_block_write(const uint8_t *data, int wide, int size)
{
    if (!io_enabled) throw std::logic_error("unselected SPI payload");
    outgoing.assign(data, data + size);
    trace.push_back("spi-write:" + std::to_string(wide) + ":" + std::to_string(size));
}
#include "pc88_lifecycle.inc"
#include "pc88_module.inc"
#include "pc88_generic.inc"

static std::vector<uint8_t> pattern(size_t size, unsigned seed)
{
    std::vector<uint8_t> result(size);
    for (size_t i = 0; i < size; ++i) result[i] = static_cast<uint8_t>(i * 29 + seed);
    return result;
}
static std::vector<uint8_t> header(unsigned declared)
{
    auto frame = pattern(512, 13);
    for (unsigned i = 0; i < 4; ++i) frame[0x1c + i] = static_cast<uint8_t>(declared >> (8 * i));
    return frame;
}
static void reset(size_t size, int width)
{
    for (int i = 0; i < 16; ++i) {
        media[i].bytes = pattern(size, static_cast<unsigned>(i + 7));
        sd_image[i].size = static_cast<__off64_t>(size); sd_image[i].offset = 0;
        sd_image[i].type = 0; sd_image_cangrow[i] = 0; buffer_lba[i] = 123;
    }
    std::strcpy(orig_name, "PC8801"); std::strcpy(core_name, "DisplayAlias"); fio_size = width; use_save = 0;
#if PC88_PRESENT
    pc88_mount(0); pc88_mount(1);
#endif
    trace.clear(); incoming.clear(); outgoing.clear(); io_enabled = false;
}
static void snapshot(const char *engine, const char *step, int disk, bool returned, int result)
{
    std::ostringstream row;
    row << "{\"engine\":" << quoted(engine) << ",\"step\":" << quoted(step)
        << ",\"return_observed\":" << (returned ? "true" : "false") << ",\"result\":";
    if (returned) row << result; else row << "null";
    row << ",\"disk\":" << disk << ",\"size\":" << sd_image[disk].size << ",\"offset\":" << sd_image[disk].offset
        << ",\"buffer_lba\":" << buffer_lba[disk] << ",\"trace\":" << strings(trace)
        << ",\"input_hex\":" << quoted(hex(incoming)) << ",\"output_hex\":" << quoted(hex(outgoing))
        << ",\"file_hex\":" << quoted(hex(media[disk].bytes)) << ",\"grow_to\":";
#if PC88_PRESENT
    if (returned) row << '[' << grow_to[0] << ',' << grow_to[1] << ']';
    else row << "null"; // Generic branch does not observe private module state.
#else
    row << "null";
#endif
    row << '}'; stages.push_back(row.str());
}
static int call(bool module, const char *step, int disk, int op, uint64_t lba, int size, std::vector<uint8_t> frame = {})
{
    incoming = std::move(frame); outgoing.clear(); trace.clear();
    int result = 0;
    if (module) {
#if PC88_PRESENT
        result = pc88_sd_service(disk, &sd_image[disk], op, lba, size, (disk + 1) << 8);
#else
        throw std::logic_error("absent PC88 API must be excluded");
#endif
    } else generic_write_branch(disk, op, lba, static_cast<uint32_t>(size), 512, (disk + 1) << 8);
    require(!io_enabled, "transaction completed");
    snapshot(module ? "module" : "generic-branch-only", step, disk, module, result);
    return result;
}
static void check_transaction(int disk, int width, int size, bool write)
{
    const std::vector<std::string> wire = {"enable", "word:" + std::to_string((write ? UIO_SECTOR_WR : UIO_SECTOR_RD) | ((disk + 1) << 8)),
        std::string(write ? "spi-read:" : "spi-write:") + std::to_string(width) + ":" + std::to_string(size), "disable"};
    auto where = std::search(trace.begin(), trace.end(), wire.begin(), wire.end());
    require(where != trace.end(), "exact wire command width length order");
}
static void ordinary_write(int disk, int width, int size, bool module)
{
    reset(32768, width); auto expected = media[disk].bytes; auto frame = pattern(static_cast<size_t>(size), 71);
    std::copy(frame.begin(), frame.end(), expected.begin() + 512);
    const int result = call(module, "in-place", disk, 2, 1, size, frame);
    if (module) require(result == 1, "module write handled");
    require(media[disk].bytes == expected && sd_image[disk].size == 32768 && sd_image[disk].offset == 512 + size, "ordinary complete write bytes and size offset");
    if (!module) require(buffer_lba[disk] == ULLONG_MAX, "generic cache invalidation");
    check_transaction(disk, width, size, true);
}
static void growth(int disk, int width, bool module)
{
    reset(2048, width); auto original = media[disk].bytes;
    call(module, "before-grant", disk, 2, 8, 512, pattern(512, 91)); require(media[disk].bytes == original, "before grant unchanged");
    const auto h = header(8192); call(module, "header", disk, 2, 0, 512, h);
    auto expected = original; std::copy(h.begin(), h.end(), expected.begin());
    const auto first = pattern(1536, 101); call(module, "moved-track", disk, 2, 8, 1536, first);
    if (module) { expected.resize(5632, 0); std::copy(first.begin(), first.end(), expected.begin() + 4096); }
    require(media[disk].bytes == expected, "header then moved track actual contrast");
    const auto next = pattern(512, 131); call(module, "sequential-track", disk, 2, 11, 512, next);
    if (module) expected.insert(expected.end(), next.begin(), next.end());
    require(media[disk].bytes == expected, "sequential growth or retained generic EOF");
    if (module) {
        call(true, "readback", disk, 1, 8, 1536); require(outgoing == first, "appended readback exact");
        call(true, "declared-end-last-sector", disk, 2, 15, 512, next); expected.resize(8192, 0); std::copy(next.begin(), next.end(), expected.begin() + 7680);
        call(true, "next-normal-sector", disk, 2, 16, 512, next); require(media[disk].bytes == expected, "declared end exact no further sector");
    }
}
static void partial(int disk, int width, bool module)
{
    reset(1300, width); auto expected = media[disk].bytes; const auto frame = pattern(1024, 43);
    std::copy(frame.begin(), frame.begin() + 276, expected.begin() + 1024);
    call(module, "partial-final-write", disk, 2, 2, 1024, frame); require(media[disk].bytes == expected && sd_image[disk].size == 1300, "partial final block retained");
    if (module) {
        call(true, "partial-final-read", disk, 1, 2, 1024); std::vector<uint8_t> wanted(1024, 0); std::copy(expected.begin() + 1024, expected.end(), wanted.begin()); require(outgoing == wanted, "partial read zero tail");
        call(true, "ordinary-EOF-read", disk, 1, 3, 512); require(outgoing == std::vector<uint8_t>(512, 0), "EOF returns zeros");
    }
}
static void packet(int disk, int width, bool module)
{
    reset(512, width); auto frame = pattern(1536, 47); const auto h = header(4096);
    std::copy(h.begin(), h.end(), frame.begin());
    call(module, "header-and-later-sectors-one-packet", disk, 2, 0, 1536, frame);
    const auto expected = std::vector<uint8_t>(frame.begin(), frame.begin() + (module ? 1536 : 512));
    require(media[disk].bytes == expected && sd_image[disk].size == (module ? 1536 : 512), "grant effective within same packet or generic EOF");
    check_transaction(disk, width, 1536, true);
}
static void shorter(int disk, int width, bool module)
{
    reset(2048, width); const auto frame = pattern(512, 53);
    call(module, "initial-large-header", disk, 2, 0, 512, header(8192));
    call(module, "initial-track", disk, 2, 8, 512, frame);
    require(sd_image[disk].size == (module ? 4608 : 2048), "initial growth distinction");
    auto expected = media[disk].bytes; const auto small = header(2048);
    std::copy(small.begin(), small.end(), expected.begin());
    call(module, "shorter-replacement-header", disk, 2, 0, 512, small);
    require(media[disk].bytes == expected, "shorter header does not truncate");
    call(module, "later-growth-after-shorter-header", disk, 2, 10, 512, frame);
    require(media[disk].bytes == expected, "shorter grant replaces prior growth permission");
    const auto changed = pattern(512, 59); call(module, "existing-track-after-shorter-header", disk, 2, 8, 512, changed);
    if (module) std::copy(changed.begin(), changed.end(), expected.begin() + 4096);
    require(media[disk].bytes == expected, "shorter declared end retains existing in-place writes");
}
static void lifecycle(int disk, int width)
{
#if PC88_PRESENT
    reset(2048, width); auto h = header(8192); auto frame = pattern(512, 67);
    call(true, "grant-before-remount", disk, 2, 0, 512, h); pc88_mount(disk); const auto kept = media[disk].bytes;
    call(true, "after-remount", disk, 2, 8, 512, frame); require(media[disk].bytes == kept, "mount resets permission");
    call(true, "grant-before-header-read", disk, 2, 0, 512, h); call(true, "header-read", disk, 1, 0, 512);
    call(true, "after-header-read", disk, 2, 8, 512, frame); require(media[disk].bytes == kept, "header read resets permission");
    call(true, "grant-before-data-read", disk, 2, 0, 512, h); call(true, "data-read", disk, 1, 1, 512);
    call(true, "after-data-read", disk, 2, 8, 512, frame); require(sd_image[disk].size == 4608, "nonheader read keeps grant");
#else
    (void)disk; (void)width;
#endif
}
static void independence(int width)
{
#if PC88_PRESENT
    reset(2048, width); const auto h = header(8192); const auto frame = pattern(512, 61);
    call(true, "slot0-grant", 0, 2, 0, 512, h); call(true, "slot1-without-grant", 1, 2, 8, 512, frame); require(sd_image[1].size == 2048, "slot1 does not inherit");
    call(true, "slot1-grant", 1, 2, 0, 512, h); pc88_mount(0); pc88_mount(3);
    call(true, "slot1-after-other-mount", 1, 2, 8, 512, frame); require(sd_image[1].size == 4608, "slot1 independent permission");
    call(true, "slot0-after-mount", 0, 2, 8, 512, frame); require(sd_image[0].size == 2048, "slot0 reset independent");
#else
    (void)width;
#endif
}

int main(int argc, char **argv)
{
    try {
        if (argc != 2) throw std::logic_error("one frozen catalog name required");
        const std::string name = argv[1]; std::string group; int disk = 0, width = 0, size = 0;
        std::string fields = name; std::replace(fields.begin(), fields.end(), '_', ' '); std::istringstream args(fields); args >> group >> disk >> width >> size;
        bool generic_ran = false;
        if (group == "write" || group == "growth" || group == "partial" || group == "packet" || group == "shorter") {
            generic_ran = true;
            for (bool module : {false, true}) { if (module && !PC88_PRESENT) continue; if (group == "write") ordinary_write(disk, width, size, module); else if (group == "growth") growth(disk, width, module); else if (group == "partial") partial(disk, width, module); else if (group == "packet") packet(disk, width, module); else shorter(disk, width, module); }
        } else if (PC88_PRESENT) {
            if (group == "read") { reset(24576, width); const auto expected = std::vector<uint8_t>(media[disk].bytes.begin() + 1024, media[disk].bytes.begin() + 1024 + size); require(call(true, "in-range-read", disk, 1, 2, size) == 1, "read handled"); require(outgoing == expected, "read payload exact"); check_transaction(disk, width, size, false); }
            else if (group == "lifecycle") lifecycle(disk, width);
            else if (group == "independent") independence(disk);
            else { reset(2048, 1); int target = 0, op = 1, expected = 0;
                if (name == "non-pc88") { std::strcpy(orig_name, "Menu"); std::strcpy(core_name, "Menu"); }
                else if (name == "original-name-decline") { std::strcpy(orig_name, "OtherCore"); std::strcpy(core_name, "PC8801"); }
                else if (name == "slot2") target = 2; else if (name == "slot15") target = 15;
                else if (name == "unmounted") { sd_image[0].size = 0; media[0].bytes.clear(); }
                else if (name == "op0") { op = 0; expected = -1; }
                else if (name == "original-casefold") { std::strcpy(orig_name, "pc8801"); std::strcpy(core_name, "OtherDisplay"); expected = 1; }
                else throw std::logic_error("unknown frozen case");
                const auto before = media[target].bytes; require(call(true, "identity-or-idle", target, op, 1, 512) == expected, "actual service result");
                require(media[target].bytes == before, "identity control no file mutation"); if (expected != 1) require(trace.empty(), "decline or idle no file SPI LED calls");
            }
        }
        std::cout << "{\"name\":" << quoted(name) << ",\"module_present\":" << (PC88_PRESENT ? "true" : "false") << ",\"module_excluded\":" << (PC88_PRESENT ? "false" : "true")
                  << ",\"generic_branch_ran\":" << (generic_ran ? "true" : "false") << ",\"case_status\":" << quoted(!PC88_PRESENT && !generic_ran ? "excluded-api-absent" : "completed")
                  << ",\"passed\":" << (!PC88_PRESENT && !generic_ran ? "null" : failures.empty() ? "true" : "false")
                  << ",\"assertions\":" << assertions << ",\"failed_assertions\":" << failures.size() << ",\"failures\":" << strings(failures) << ",\"stages\":[";
        for (size_t i = 0; i < stages.size(); ++i) { if (i) std::cout << ','; std::cout << stages[i]; }
        std::cout << "]}\n"; return failures.empty() ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << "FIXTURE_ERROR " << error.what() << '\n'; return 2; }
}
