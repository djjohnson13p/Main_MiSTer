#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "miniz.h"
#ifdef __linux__
#include <linux/fs.h>
#else
typedef off_t __off64_t;
#define BLKGETSIZE64 0x80081272UL
#endif
#include "file_open_types.inc"

struct FixtureFile : fileTYPE {
    FixtureFile() { name[0] = 0; path[0] = 0; }
};

static std::string storage_root;
const char *getRootDir() { return storage_root.c_str(); }
static fileTYPE *active_file;
static int forbidden_name_writes, forbidden_path_writes, forbidden_overlaps;
struct BlockedWrite {};

// Stop the old unsafe libc call before writing; valid calls use real libc.
static char *guarded_strcpy(char *dest, const char *source)
{
    if (active_file && dest == active_file->name && std::strlen(source) >= sizeof(active_file->name)) {
        ++forbidden_name_writes;
        throw BlockedWrite{};
    }
    return std::strcpy(dest, source);
}
static int guarded_sprintf(char *dest, const char *format, ...)
{
    va_list args, count_args, inspect_args;
    va_start(args, format);
    va_copy(count_args, args);
    const int needed = std::vsnprintf(nullptr, 0, format, count_args);
    va_end(count_args);
    bool overlap = false;
    if (dest == full_path) {
        va_copy(inspect_args, args);
        const int strings = !std::strcmp(format, "%s/%s") ? 2 : 1;
        for (int i = 0; i < strings; ++i) {
            const uintptr_t value = reinterpret_cast<uintptr_t>(va_arg(inspect_args, const char *));
            const uintptr_t begin = reinterpret_cast<uintptr_t>(full_path);
            overlap |= value >= begin && value < begin + sizeof(full_path);
        }
        va_end(inspect_args);
        if (needed < 0 || static_cast<size_t>(needed) >= sizeof(full_path)) {
            ++forbidden_path_writes;
            va_end(args);
            throw BlockedWrite{};
        }
        if (overlap) {
            ++forbidden_overlaps;
            va_end(args);
            throw BlockedWrite{};
        }
    }
    const int result = std::vsprintf(dest, format, args);
    va_end(args);
    return result;
}

static int shm_open_calls, shm_unlink_calls;
static std::string shm_open_name, shm_unlink_name;
static int spy_shm_open(const char *name, int flags, mode_t mode)
{
    ++shm_open_calls;
    shm_open_name = name;
    if (flags != (O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC) || mode != 0777) throw std::runtime_error("unexpected shared-memory flags");
    std::string pattern = storage_root + "/shm-spy-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back(0);
    const int fd = mkstemp(writable.data());
    if (fd < 0) throw std::runtime_error("shared-memory file spy failed");
    unlink(writable.data());
    return fd;
}
static int spy_shm_unlink(const char *name)
{
    ++shm_unlink_calls;
    shm_unlink_name = name;
    errno = EIO; // A successful peripheral cleanup may still change errno.
    return 0;
}
#define strcpy guarded_strcpy
#define sprintf guarded_sprintf
#define shm_open spy_shm_open
#define shm_unlink spy_shm_unlink
#include "file_open_production.inc"
#undef shm_unlink
#undef shm_open
#undef sprintf
#undef strcpy

static int checks, failures;
static void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct OpenResult { int result = 0, error = 0; bool blocked = false; };
static OpenResult invoke(fileTYPE &file, const std::string &path, int mode = O_RDONLY,
                         bool zip_api = false, uint32_t crc = 0, int use_zip = 1)
{
    OpenResult result;
    active_file = &file;
    try { result.result = zip_api ? FileOpenZip(&file, path.c_str(), crc) : FileOpenEx(&file, path.c_str(), mode, 1, use_zip); }
    catch (const BlockedWrite &) { result.blocked = true; }
    result.error = errno;
    active_file = nullptr;
    return result;
}
static OpenResult invoke_alias(fileTYPE &file, const char *source)
{
    OpenResult result;
    active_file = &file;
    try { result.result = FileOpen(&file, source, 1); }
    catch (const BlockedWrite &) { result.blocked = true; }
    result.error = errno;
    active_file = nullptr;
    return result;
}
static std::string path(const char *leaf) { return storage_root + "/" + leaf; }
static std::string basename(const std::string &value) { return value.substr(value.find_last_of('/') + 1); }
static void read_payload(fileTYPE &file, const std::string &wanted)
{
    if (!file.opened()) {
        for (int i = 0; i < 4; ++i) check(false, "accepted fitting input must provide a handle for actual payload checks");
        return;
    }
    std::vector<char> bytes(wanted.size() + 1);
    check(FileReadAdv(&file, bytes.data(), wanted.size(), -1) == static_cast<int>(wanted.size()), "actual file reader must return the entire expected payload");
    check(std::string(bytes.data(), wanted.size()) == wanted, "actual file or ZIP payload must remain exact");
    check(FileSeek(&file, 0, SEEK_SET), "actual seek must rewind the successful open");
    std::fill(bytes.begin(), bytes.end(), 0);
    check(FileReadAdv(&file, bytes.data(), wanted.size(), -1) == static_cast<int>(wanted.size()) && std::string(bytes.data(), wanted.size()) == wanted,
          "rewound reader must return the same bytes");
}
static void accepted(const std::string &target, const std::string &payload, bool zip_api = false, uint32_t crc = 0)
{
    FixtureFile file;
    const auto result = invoke(file, target, O_RDONLY, zip_api, crc);
    check(result.result == 1 && !result.blocked, "complete fitting path must open through the real file or ZIP API");
    check(file.opened() && file.mode == O_RDONLY && !file.type && file.size == static_cast<__off64_t>(payload.size()), "successful open must retain actual handle, mode and size");
    check(std::string(file.name) == basename(target), "stored basename must retain every byte and its suffix");
    read_payload(file, payload);
    check(!FileClose(&file) && !file.opened() && !file.size, "actual close must release the successful file or ZIP handle");
}
static void rejected(const std::string &target, bool zip_api = false, int mode = O_RDONLY, int use_zip = 1)
{
    FixtureFile file;
    const auto initial = invoke(file, path("Small.gba"));
    if (!initial.result) throw std::runtime_error("rejection seed must open");
    const int old_fd = fileno(file.filp);
    const int before_shm = shm_open_calls;
    std::memset(file.path, 'P', sizeof(file.path));
    const auto result = invoke(file, target, mode, zip_api, 0, use_zip);
    check(!result.result && !result.blocked, "overcapacity open must reject before attempting an unsafe libc write");
    check(result.error == ENAMETOOLONG, "bounds rejection must report ENAMETOOLONG after old-handle cleanup");
    check(!file.opened() && !file.filp && !file.zip && !file.size && !file.mode && !file.type,
          "bounds rejection must close the old handle and leave no new file or ZIP state");
    check(!file.name[0], "bounds rejection must clear the failed stored name");
    check(std::all_of(file.path, file.path + sizeof(file.path), [](char c) { return c == 'P'; }), "independent saved path field must remain untouched");
    check(fcntl(old_fd, F_GETFD) == -1 && errno == EBADF, "previous ordinary descriptor must be closed on a rejected open attempt");
    check(shm_open_calls == before_shm, "rejected virtual-disk name must not call the shared-memory API");
    FileClose(&file);
}
static void name_boundaries()
{
    for (int size : {255, 260}) for (bool api : {false, true}) {
        const auto member = std::string(size - 4, 'a') + ".gba";
        accepted(path("Data.zip/") + member, "BASENAME_" + std::to_string(size), api);
    }
    std::string utf8;
    for (int i = 0; i < 128; ++i) utf8 += "\xc3\xa9";
    for (bool api : {false, true}) accepted(path("Data.zip/") + utf8 + ".gba", "UTF8_260", api);
    for (int size : {261, 300}) for (bool api : {false, true}) {
        rejected(path("Data.zip/") + std::string(size - 4, 'a') + ".gba", api);
    }
    for (bool api : {false, true}) rejected(path("Data.zip/") + utf8 + "\xc3\xa9.gba", api);
    rejected(path("") + std::string(261, 'x'), false, O_CREAT | O_RDWR | O_TRUNC, 0);
    rejected(path("") + std::string(261, 'x'), true);
}
static void full_path_boundaries(const std::string &fit, const std::string &over, const std::string &huge, const std::string &plain)
{
    check(fit.size() == sizeof(full_path) - 1 && over.size() == sizeof(full_path) && huge.size() > sizeof(full_path), "fixtures must match the actual production full-path capacity");
    for (bool api : {false, true}) {
        accepted(fit, "FULL_PATH_2099", api);
        accepted(fit.substr(storage_root.size() + 1), "FULL_PATH_2099", api);
        rejected(over, api);
        rejected(huge, api);
        rejected(over.substr(storage_root.size() + 1), api);
    }
    rejected(over, false, O_CREAT | O_RDWR | O_TRUNC, 0);
    accepted(over.substr(0, sizeof(full_path) - 1), "WRONG_PREFIX_PAYLOAD");
    if (plain != "-") {
        accepted(plain, "PREFIX_ORIGINAL");
        rejected(plain + "x", false, O_CREAT | O_RDWR | O_TRUNC, 0);
        accepted(plain, "PREFIX_ORIGINAL");
    }
}
static void alias_controls()
{
    for (bool relative : {false, true}) {
        FixtureFile file;
        const std::string target = relative ? "Small.gba" : path("Small.gba");
        std::memcpy(full_path, target.c_str(), target.size() + 1);
        const auto result = invoke_alias(file, full_path);
        check(result.result == 1 && !result.blocked, "open must support its own full_path buffer as an absolute or relative source");
        check(std::string(file.name) == "Small.gba", "aliased source must preserve the complete basename");
        read_payload(file, "ORDINARY_PAYLOAD");
        FileClose(&file);
    }
    FixtureFile file;
    invoke(file, path("Small.gba"));
    const auto result = invoke_alias(file, file.name);
    check(result.result == 1 && !result.blocked && std::string(file.name) == "Small.gba", "existing stored-name source must survive close and reset");
    FileClose(&file);
}
static void terminal_zip_controls()
{
    // Physical capacity is one byte larger; the old write is safe to observe.
    std::vector<char> diagnostic(sizeof(full_path) + 1, 'C');
    const std::string terminal = "/" + std::string(sizeof(full_path) - 6, 'd') + ".ZIP";
    std::memcpy(diagnostic.data(), terminal.c_str(), terminal.size() + 1);
    char *archive = nullptr, *member = nullptr;
    check(terminal.size() == sizeof(full_path) - 1, "terminal ZIP fixture must fill the conceptual path through its terminator");
    check(FileIsZipped(diagnostic.data(), &archive, &member) && archive == diagnostic.data() && !member[0], "terminal ZIP must retain a valid empty member string");
    check(diagnostic[sizeof(full_path)] == 'C', "terminal ZIP splitter must not write after the conceptual path buffer");
    char normal[] = "/tmp/Case.ZIP/folder/Inner.gba";
    check(FileIsZipped(normal, &archive, &member) && !std::strcmp(archive, "/tmp/Case.ZIP") && !std::strcmp(member, "folder/Inner.gba"), "nonterminal ZIP splitting must retain its existing case-insensitive path semantics");
    char ordinary[] = "/tmp/Plain.gba";
    check(!FileIsZipped(ordinary, nullptr, nullptr) && !std::strcmp(ordinary, "/tmp/Plain.gba"), "ordinary path must remain intact when ZIP splitting finds no marker");
}
static void virtual_disk_controls()
{
    for (const std::string &name : {std::string("vdsk"), std::string("/vdsk"), "/" + std::string(259, 'v')}) {
        FixtureFile file;
        const int before = shm_open_calls, before_close = shm_unlink_calls;
        const auto result = invoke(file, name, -1);
        const std::string normalized = name[0] == '/' ? name : "/" + name;
        check(result.result == 1 && !result.blocked && shm_open_calls == before + 1 && shm_open_name == "/vdsk", "fitting virtual-disk name must preserve the legacy shared-memory open boundary");
        check(file.type == 1 && file.mode == (O_CREAT | O_RDWR | O_TRUNC) && !file.size && std::string(file.name) == normalized, "mode -1 must retain the full normalized name rather than a basename");
        if (result.result) {
            char payload[] = "VDISK";
            check(FileWriteAdv(&file, payload, 5) == 5 && FileGetSize(&file) == 5, "virtual-disk spy descriptor must exercise actual write and size APIs");
            check(FileSeek(&file, 0, SEEK_SET), "virtual-disk actual seek must rewind written bytes");
            read_payload(file, "VDISK");
        }
        FileClose(&file);
        check(shm_unlink_calls == before_close + 1 && shm_unlink_name == normalized, "virtual-disk close must preserve the stored full-name cleanup contract");
    }
    rejected("/" + std::string(260, 'v'), false, -1);
    rejected(std::string(260, 'v'), false, -1);
    rejected("/" + std::string(2099, 'v'), false, -1);
}
static void ordinary_controls()
{
    accepted(path("Small.gba"), "ORDINARY_PAYLOAD");
    accepted("Small.gba", "ORDINARY_PAYLOAD");
    accepted(path("Plain"), "NO_EXTENSION");
    accepted(path("Data.zip/Placeholder.bin"), "CRC_SELECTED", true, mz_crc32(0, reinterpret_cast<const unsigned char *>("CRC_SELECTED"), 12));
    accepted(path("Data.zip/Fallback.bin"), "NAME_FALLBACK", true, 1);
    FixtureFile file;
    auto result = invoke(file, path("Writable.img"), O_RDWR, false, 0, 0);
    check(result.result == 1 && file.mode == O_RDWR, "ordinary writable disk must still open with its requested mode");
    if (result.result) {
        char byte = 'w';
        check(FileWriteAdv(&file, &byte, 1) == 1 && FileSeek(&file, 0, SEEK_SET), "ordinary writable disk must retain actual write and seek APIs");
        char actual = 0;
        check(FileReadAdv(&file, &actual, 1) == 1 && actual == 'w', "written synthetic disk byte must be readable");
    }
    FileClose(&file);
    for (int mode : {O_RDWR, O_WRONLY}) {
        result = invoke(file, path("Data.zip/Fallback.bin"), mode);
        check(!result.result && !result.blocked && !file.opened(), "ZIP members must remain unavailable for writing");
    }
    for (bool api : {false, true}) for (const char *name : {"Data.zip/Missing.bin", "Bad.zip/Missing.bin", "Missing.zip/Inner.gba"}) {
        result = invoke(file, path(name), O_RDONLY, api);
        check(!result.result && !result.blocked, "missing or malformed ZIP opens must keep ordinary failure behavior");
        FileClose(&file);
    }
    result = invoke(file, path("Small.gba"), O_RDONLY, true);
    check(!result.result && !result.blocked, "ZIP-only API must reject a fitting ordinary path");
    FileClose(&file);
}
static int descriptor_count()
{
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) throw std::runtime_error("descriptor inventory is unavailable");
    int count = 0;
    while (auto entry = readdir(directory)) if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) ++count;
    closedir(directory);
    return count;
}
static void prior_handle_controls(const std::string &over)
{
    for (const auto &target : {path("Data.zip/") + std::string(261, 'n'), over}) {
        FixtureFile file;
        const int before = descriptor_count();
        if (!invoke(file, path("Data.zip/Fallback.bin")).result) throw std::runtime_error("prior ZIP seed must open");
        const auto result = invoke(file, target);
        check(!result.result && !result.blocked, "rejected open must safely finish after an existing ZIP handle");
        check(!file.opened() && !file.zip && !file.size && !file.name[0], "rejected open must clear the prior ZIP and failed name");
        check(descriptor_count() == before, "rejected open must release the prior miniz archive descriptor");
        FileClose(&file);
    }
    FixtureFile file;
    if (!invoke(file, "vdsk", -1).result) throw std::runtime_error("prior virtual-disk seed must open");
    const int prior_fd = fileno(file.filp), before_unlink = shm_unlink_calls, before_open = shm_open_calls;
    const auto result = invoke(file, over);
    check(!result.result && !result.blocked && result.error == ENAMETOOLONG, "path rejection must restore ENAMETOOLONG after shared-memory cleanup changes errno");
    check(shm_unlink_calls == before_unlink + 1 && shm_unlink_name == "/vdsk" && shm_open_calls == before_open,
          "rejected ordinary open must close a prior virtual disk without creating another");
    check(!file.opened() && !file.size && !file.mode && !file.type && !file.name[0], "virtual-disk replacement rejection must reset the same bounded file state");
    check(fcntl(prior_fd, F_GETFD) == -1 && errno == EBADF, "prior virtual-disk spy descriptor must be closed on rejection");
    FileClose(&file);
}
int main(int argc, char **argv)
{
    if (argc != 6) return 2;
    storage_root = argv[1];
    try {
        ordinary_controls();
        name_boundaries();
        full_path_boundaries(argv[2], argv[3], argv[4], argv[5]);
        alias_controls();
        terminal_zip_controls();
        virtual_disk_controls();
        prior_handle_controls(argv[3]);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FIXTURE: %s\n", error.what());
        return 2;
    }
    std::printf("JSON {\"checks\":%d,\"failures\":%d,\"prevented_name_writes\":%d,\"prevented_path_writes\":%d,\"prevented_overlapping_formats\":%d}\n",
                checks, failures, forbidden_name_writes, forbidden_path_writes, forbidden_overlaps);
    return failures ? 1 : 0;
}
