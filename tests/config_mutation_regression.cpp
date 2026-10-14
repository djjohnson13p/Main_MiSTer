// Synthetic filesystem endpoints only; production definitions are extracted.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef __linux__
typedef off_t __off64_t;
#endif
#include "config_mutation_types.inc"

static bool fail_new;
__attribute__((noinline)) void *operator new(std::size_t size)
{ if (fail_new) throw std::bad_alloc(); if (void *p = std::malloc(size ? size : 1)) return p; throw std::bad_alloc(); }
__attribute__((noinline)) void *operator new[](std::size_t size) { return ::operator new(size); }
__attribute__((noinline)) void operator delete(void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void *p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
struct AllocationFailure {
    AllocationFailure() { fail_new = true; }
    ~AllocationFailure() { fail_new = false; }
};
static std::string storage_root = "/media/fat";
static bool null_root, change_root_in_directory;
static unsigned root_calls;
static const char *getRootDir() { root_calls++; return null_root ? nullptr : storage_root.c_str(); }
struct Boundary { std::string kind; };
static bool overlaps(const char *source, const char *destination, size_t room)
{
    const uintptr_t src = reinterpret_cast<uintptr_t>(source), dst = reinterpret_cast<uintptr_t>(destination);
    return src < dst + room && dst < src + std::strlen(source) + 1;
}
static int guarded_sprintf(char *out, const char *format, ...)
{
    if (out != full_path) throw Boundary{"untracked-sprintf"};
    if (!format || (std::strcmp(format, "%s") && std::strcmp(format, "%s/%s")))
        throw std::runtime_error("Unrecognized production sprintf format; no varargs inspected");
    va_list args; va_start(args, format);
    const char *first = va_arg(args, const char *);
    const char *second = !std::strcmp(format, "%s/%s") ? va_arg(args, const char *) : nullptr;
    va_end(args);
    if (!first || (!second && std::strcmp(format, "%s"))) throw Boundary{"null-sprintf-input"};
    if (overlaps(first, out, sizeof full_path) || (second && overlaps(second, out, sizeof full_path))) throw Boundary{"overlapping-sprintf"};
    const std::string value = std::string(first) + (second ? std::string("/") + second : "");
    if (value.size() >= sizeof full_path) throw Boundary{"capacity-sprintf-2100"};
    std::memcpy(out, value.c_str(), value.size() + 1);
    return static_cast<int>(value.size());
}
template<size_t N> static char *guarded_strncat(char (&out)[N], const char *text, size_t count)
{
    const char *end = static_cast<const char *>(std::memchr(out, 0, N));
    if (!end || !text) throw Boundary{"invalid-strcat-input"};
    const size_t length = end - out, extra = std::min(count, std::strlen(text));
    if (extra >= N - length) throw Boundary{"capacity-strcat-" + std::to_string(N)};
    if (overlaps(text, out + length, extra + 1)) throw Boundary{"overlapping-strcat"};
    std::memcpy(out + length, text, extra);
    out[length + extra] = 0;
    return out;
}
template<size_t N> static char *guarded_strcat(char (&out)[N], const char *text)
{ return guarded_strncat(out, text, text ? std::strlen(text) : 0); }

static std::vector<std::string> events;
static std::map<std::string, std::vector<uint8_t>> files;
static std::vector<uint8_t> temporary_payload;
static std::string opened, renamed, loaded, load_argument;
static int fault;
static bool fail_directory, fail_load, missing_map_load;
struct FileApiReturn { std::string api, name; int value; };
static std::vector<FileApiReturn> file_api_returns;
static bool dip_identity_completed;
static std::vector<std::string> dip_inputs, dip_names, dip_targets;
static std::vector<uint64_t> dip_loaded_values, dip_wanted_values, dip_saved_markers;
static void record_file_api(const char *api, const char *name, int value)
{
    const int saved_errno=errno;
    file_api_returns.push_back({api,name,value});
    errno=saved_errno;
}
static unsigned load_reads, closes;
// The query treats archive state as opaque; metadata/cache endpoints are spies.
struct QueryArchive {};
static QueryArchive last_zip_archive;
enum { QUERY_MISSING, QUERY_EXISTS, QUERY_ZIP_EXACT, QUERY_ZIP_SCAN, QUERY_ZIP_MISSING, QUERY_ZIP_OPEN_FAILURE, QUERY_ZIP_ROOT };
static int query_kind;
static char *query_memory;
static std::string query_original;
static size_t minimum_query_append_room = sizeof full_path;
static int production_FileIsZipped(char *, char **, char **);
static void overwrite_global();
static std::string absolute(const char *name)
{ return name[0] == '/' ? name : storage_root + "/" + name; }
static int fixture_open(const char *name, int flags, mode_t permissions)
{
    if (flags != (O_WRONLY | O_CREAT | O_TRUNC | O_SYNC) || permissions != (S_IRWXU | S_IRWXG | S_IRWXO)) throw std::runtime_error("Unexpected save open flags");
    opened = name;
    events.push_back("open:" + opened);
    if (fault == 1) { errno = EACCES; return -1; }
    return 7;
}
static int fixture_write(int fd, void *buffer, size_t size)
{
    if (fd != 7) throw std::runtime_error("Unexpected write descriptor");
    events.push_back("write:" + std::to_string(size));
    if (fault == 2 || fault == 7) { errno = EIO; return -1; }
    if (fault == 3) return size ? static_cast<int>(size) - 1 : 0;
    if (size) temporary_payload.assign(static_cast<uint8_t *>(buffer), static_cast<uint8_t *>(buffer) + size);
    if (fault == 6) overwrite_global();
    return static_cast<int>(size);
}
static int fixture_close(int fd)
{
    if (fd != 7) throw std::runtime_error("Unexpected close descriptor");
    events.push_back("close");
    if (fault == 4) { errno = EIO; return -1; }
    return 0;
}
static int fixture_rename(const char *from, const char *to)
{
    renamed = to;
    events.push_back("rename:" + std::string(from) + "=>" + to);
    if (fault == 5) { errno = EXDEV; return -1; }
    files[to] = temporary_payload;
    return 0;
}
static int fixture_unlink(const char *name)
{
    events.push_back("unlink:" + std::string(name));
    if (fault == 7 || fault == 8) { errno = fault == 7 ? EPERM : ENOENT; return -1; }
    files.erase(name);
    return 0;
}
static int fixture_rmdir(const char *name)
{
    events.push_back("rmdir:" + std::string(name));
    if (fault == 8) { errno = ENOTEMPTY; return -1; }
    return 0;
}
static int fixture_mkdir(const char *name, mode_t permissions)
{
    if (permissions != (S_IRWXU | S_IRWXG | S_IRWXO)) throw std::runtime_error("Unexpected directory permissions");
    events.push_back("mkdir:" + std::string(name));
    if (fail_directory) { errno = EACCES; return -1; }
    return 0;
}
static int observed_FileIsZipped(char *name, char **archive, char **member)
{
    query_memory = name;
    query_original = name;
    events.push_back("query:" + query_original);
    if (change_root_in_directory) storage_root = "/changed-root";
    return production_FileIsZipped(name, archive, member);
}
static char *guarded_query_strcat(char *destination, const char *text)
{
    const uintptr_t begin = reinterpret_cast<uintptr_t>(query_memory), address = reinterpret_cast<uintptr_t>(destination);
    if (!query_memory || address < begin || address - begin >= sizeof full_path || !text)
        throw std::runtime_error("Untracked directory query append");
    const size_t capacity = sizeof full_path - (address - begin);
    const char *end = static_cast<const char *>(std::memchr(destination, 0, capacity));
    if (!end) throw Boundary{"unterminated-directory-query"};
    const size_t length = end - destination, extra = std::strlen(text);
    if (extra >= capacity - length) throw Boundary{"capacity-directory-query-2100"};
    minimum_query_append_room = std::min(minimum_query_append_room, capacity - length);
    events.push_back("query-append:" + std::to_string(capacity - length));
    std::memcpy(destination + length, text, extra + 1);
    return destination;
}
static int get_stmode(const char *path)
{
    events.push_back("stat:" + std::string(path));
    return query_kind == QUERY_EXISTS ? S_IFDIR : 0;
}
static int OpenZipfileCached(char *path, int flags)
{
    if (flags) throw std::runtime_error("Unexpected query archive flags");
    events.push_back("zip-open:" + std::string(path));
    if (query_kind == QUERY_ZIP_OPEN_FAILURE) { errno = EIO; return 0; }
    return 1;
}
static int mz_zip_reader_locate_file(QueryArchive *, const char *member, const char *, int)
{
    events.push_back("zip-locate:" + std::string(member));
    return query_kind == QUERY_ZIP_EXACT ? 0 : -1;
}
static int mz_zip_reader_is_file_a_directory(QueryArchive *, int index)
{ return query_kind == QUERY_ZIP_EXACT && index == 0; }
static size_t mz_zip_reader_get_num_files(QueryArchive *)
{ return query_kind == QUERY_ZIP_SCAN ? 1 : 0; }
static size_t mz_zip_reader_get_filename(QueryArchive *, size_t index, char *out, size_t capacity)
{
    if (query_kind != QUERY_ZIP_SCAN || index != 0) throw std::runtime_error("Unexpected ZIP scan index");
    const char entry[] = "inner/file.dat";
    if (sizeof entry > capacity) throw std::runtime_error("ZIP fixture output capacity");
    std::memcpy(out, entry, sizeof entry);
    events.push_back("zip-entry:" + std::string(entry));
    return sizeof entry;
}
static int mz_zip_get_last_error(QueryArchive *) { return EIO; }
static const char *mz_zip_get_error_string(int) { return "synthetic archive open error"; }
int FileOpen(fileTYPE *file, const char *name, char)
{
    load_argument = name;
    loaded = absolute(name);
    events.push_back("load-open:" + loaded);
    if (fail_load || (missing_map_load && files.find(loaded)==files.end())) { errno = ENOENT; return 0; }
    file->filp = reinterpret_cast<FILE *>(uintptr_t(1));
    auto found = files.find(loaded);
    file->size = found == files.end() ? 8 : static_cast<__off64_t>(found->second.size());
    return 1;
}
int FileReadAdv(fileTYPE *, void *buffer, int size, int)
{
    load_reads++;
    events.push_back("load-read:" + std::to_string(size));
    auto found = files.find(loaded);
    const std::vector<uint8_t> fallback(8, 0xA5);
    const auto &bytes = found == files.end() ? fallback : found->second;
    if (size < 0 || static_cast<size_t>(size) > bytes.size()) throw std::runtime_error("Unexpected synthetic load size");
    std::memcpy(buffer, bytes.data(), static_cast<size_t>(size));
    return size;
}
int FileClose(fileTYPE *file)
{
    if (file->filp) { closes++; events.push_back("load-close"); }
    file->filp = nullptr;
    return 0;
}
#include "config_mutation_production.inc"
static void overwrite_global() { make_fullpath("Other.bin"); }

static uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static unsigned checks, failures;
static bool excluded, trapped, observed_return;
static int observed_status;
static std::string boundary;
static void check(bool result, const char *label)
{ checks++; if (!result) { failures++; std::fprintf(stderr, "FAIL: %s\n", label); } }
static int observe(int status) { observed_return = true; observed_status = status; return status; }
static unsigned count(const char *prefix)
{ return static_cast<unsigned>(std::count_if(events.begin(), events.end(), [=](const std::string &event) { return event.compare(0, std::strlen(prefix), prefix) == 0; })); }
static bool data_at(const std::string &name, const void *data, size_t size)
{
    auto found = files.find(name);
    return found != files.end() && found->second.size() == size && (!size || !std::memcmp(found->second.data(), data, size));
}
static bool reject_clean(int status, int error)
{ return status == 0 && errno == error && events.empty() && !std::strcmp(full_path, "PRIOR_IDENTITY"); }
static bool directory_query_controls()
{
    bool correct = true;
    for (int kind = QUERY_MISSING; kind <= QUERY_ZIP_ROOT; kind++) {
        query_kind = kind;
        const std::string directory = kind <= QUERY_EXISTS ? "/media/fat/config/inputs" :
                                      kind == QUERY_ZIP_ROOT ? "/media/fat/config/archive.zip" :
                                      "/media/fat/config/archive.zip/inner";
        std::memset(full_path, 0, sizeof full_path);
        std::strcpy(full_path, "PRIOR_IDENTITY");
        const auto before_mkdir = count("mkdir:"), before_query = count("query:"), before_zip = count("zip-open:");
        events.push_back("query-case:" + std::to_string(kind));
#if HAVE_PREPARED_DIRECTORY
        const int status = create_config_directory(directory.c_str());
#else
        const int status = FileCreatePath(directory.c_str());
#endif
        const bool missing = kind == QUERY_MISSING || kind == QUERY_ZIP_MISSING || kind == QUERY_ZIP_OPEN_FAILURE;
        correct &= status == 1 && count("query:") == before_query + 1 && count("mkdir:") == before_mkdir + missing;
        if (missing) correct &= events.back() == "mkdir:" + directory;
        if (kind == QUERY_ZIP_ROOT) correct &= count("zip-open:") == before_zip;
        if (kind >= QUERY_ZIP_EXACT && kind <= QUERY_ZIP_OPEN_FAILURE) {
            correct &= count("zip-open:") == before_zip + 1 && query_original == directory;
        }
    }
    query_kind = QUERY_MISSING;
    return correct;
}
template<size_t N> struct GuardedOutput {
    static constexpr uint64_t LEFT = 0x0123456789abcdefULL, RIGHT = 0xfedcba9876543210ULL;
    uint64_t left = LEFT;
    std::array<char, N> bytes;
    uint64_t right = RIGHT;
    GuardedOutput() { set("KEEP"); }
    char *data() { return bytes.data(); }
    void set(const char *text) {
        if (std::strlen(text) >= N) throw std::runtime_error("Oversize fixture initializer");
        bytes.fill(static_cast<char>(0xA7));
        std::memcpy(data(), text, std::strlen(text) + 1);
    }
    bool intact() const { return left == LEFT && right == RIGHT; }
    bool unchanged(const std::array<char, N> &before) const {
        return intact() && !std::memcmp(bytes.data(), before.data(), N);
    }
};
static void reset()
{
    storage_root = "/media/fat";
    null_root = change_root_in_directory = false;
    root_calls = load_reads = closes = 0;
    query_kind = QUERY_MISSING; query_memory = nullptr; query_original.clear(); minimum_query_append_room = sizeof full_path;
    fault = 0; fail_directory = fail_load = missing_map_load = dip_identity_completed = false;
    file_api_returns.clear(); dip_inputs.clear(); dip_names.clear(); dip_targets.clear();
    dip_loaded_values.clear(); dip_wanted_values.clear(); dip_saved_markers.clear();
    events.clear(); files.clear(); temporary_payload.clear();
    opened.clear(); renamed.clear(); loaded.clear(); load_argument.clear();
    std::strcpy(full_path, "PRIOR_IDENTITY");
    errno = ERANGE;
}
static void run_case(const std::string &name)
{
    if (name == "save-relative" || name == "save-absolute" || name == "save-empty" || name == "save-zero") {
        const char *path = name == "save-absolute" ? "/absolute/Game.bin" : name == "save-empty" ? "" : "Game.bin";
        const std::string target = absolute(path);
        const int size = name == "save-zero" ? 0 : 8;
        const int result = observe(FileSave(path, payload, size));
        check(result == size && opened == target + ".tmp" && renamed == target && data_at(target, payload, size) &&
              events == std::vector<std::string>{"open:" + target + ".tmp", "write:" + std::to_string(size), "close", "rename:" + target + ".tmp=>" + target} && root_calls == (path[0] == '/' ? 0U : 1U), "accepted save identity, payload, status and endpoint order");
    } else if (name == "save-absolute-bound" || name == "save-relative-bound" || name == "save-absolute-reject" || name == "save-relative-reject") {
        const bool is_absolute = name.find("absolute") != std::string::npos;
        const bool reject = name.find("reject") != std::string::npos;
        const std::string path = is_absolute ? "/" + std::string(2098 + reject, 'G') : std::string(2088 + reject, 'G');
        const int result = observe(FileSave(path.c_str(), payload, 8));
        check(reject ? reject_clean(result, ENAMETOOLONG) : result == 8 && renamed == absolute(path.c_str()) && renamed.size() == 2099 && opened.size() == 2103 && data_at(renamed, payload, 8), "synthetic complete 2099 destination and 2103 temporary path bounds");
    } else if (name == "delete-bound" || name == "rmdir-bound" || name == "delete-reject" || name == "rmdir-reject") {
        const bool reject = name.find("reject") != std::string::npos, directory = name.find("rmdir") == 0;
        const std::string path = "/" + std::string(2098 + reject, 'G');
        const int result = observe(directory ? DirDelete(path.c_str()) : FileDelete(path.c_str()));
        check(reject ? reject_clean(result, ENAMETOOLONG) : result == 1 && events == std::vector<std::string>{std::string(directory ? "rmdir:" : "unlink:") + path}, "synthetic delete/rmdir complete target bound and boolean convention");
    } else if (name.find("-alias-") != std::string::npos) {
        const bool substring = name.find("substring") != std::string::npos;
        std::strcpy(full_path, "/ordinary/Game.bin");
        const char *path = full_path + (substring ? 10 : 0);
        const std::string target = absolute(path);
        int result;
        if (name.find("save-") == 0) result = observe(FileSave(path, payload, 8));
        else if (name.find("delete-") == 0) result = observe(FileDelete(path));
        else result = observe(DirDelete(path));
        check(name.find("save-") == 0 ? result == 8 && renamed == target && data_at(target, payload, 8) :
              result == 1 && events == std::vector<std::string>{std::string(name.find("delete-") == 0 ? "unlink:" : "rmdir:") + target}, "owned full/substring alias keeps exact target without overlapping publication");
    } else if (name == "save-global-overwrite") {
        fault = 6;
        const int result = observe(FileSave("Game.bin", payload, 8));
        check(result == 8 && opened == "/media/fat/Game.bin.tmp" && renamed == "/media/fat/Game.bin" &&
              !std::strcmp(full_path, "/media/fat/Other.bin") && data_at(renamed, payload, 8), "same-thread overwrite does not change accepted rename destination; no C++ race");
    } else if (name == "save-open-failure" || name == "save-write-failure" || name == "save-short-write" || name == "save-close-failure" || name == "save-rename-failure" || name == "save-cleanup-errno") {
        fault = name == "save-open-failure" ? 1 : name == "save-write-failure" ? 2 : name == "save-short-write" ? 3 : name == "save-close-failure" ? 4 : name == "save-rename-failure" ? 5 : 7;
        const int result = observe(FileSave("Game.bin", payload, 8));
        const int expected_errno = fault == 1 ? EACCES : fault == 3 ? ERANGE : fault == 5 ? EXDEV : fault == 7 ? EPERM : EIO;
        std::vector<std::string> expected{"open:/media/fat/Game.bin.tmp"};
        if (fault != 1) { expected.push_back("write:8"); expected.push_back("close"); if (fault == 5) expected.push_back("rename:/media/fat/Game.bin.tmp=>/media/fat/Game.bin"); expected.push_back("unlink:/media/fat/Game.bin.tmp"); }
        check(result == 0 && errno == expected_errno && events == expected && files.empty(), "inherited syscall failure cleanup/status including cleanup errno replacement");
    } else if (name == "delete-valid" || name == "delete-failure" || name == "rmdir-valid" || name == "rmdir-failure") {
        const bool directory = name.find("rmdir") == 0, fail = name.find("failure") != std::string::npos;
        fault = fail ? 8 : 0;
        const int result = observe(directory ? DirDelete("Dir") : FileDelete("Game.bin"));
        check(result == !fail && events == std::vector<std::string>{directory ? "rmdir:/media/fat/Dir" : "unlink:/media/fat/Game.bin"} && (!fail || errno == (directory ? ENOTEMPTY : ENOENT)), "existing delete/rmdir boolean and errno conventions");
    } else if (name == "config-empty" || name == "config-leading-slash" || name == "config-nested" || name == "config-mkdir-failure" || name == "config-directory-root-change") {
        const char *input = name == "config-empty" ? "" : name == "config-leading-slash" ? "/Game.cfg" : "inputs/Game.cfg";
        const std::string target = "/media/fat/config/" + std::string(input);
        fail_directory = name == "config-mkdir-failure";
        change_root_in_directory = name == "config-directory-root-change";
        const int result = observe(FileSaveConfig(input, payload, 8));
        const unsigned directories = name == "config-empty" ? 0 : 1;
        bool accepted_directories = true;
        for (const auto &event : events) if (event.find("mkdir:") == 0) accepted_directories &= event.find("mkdir:/media/fat/config/") == 0 && event.size() <= 6 + 2098;
        bool correct = result == 8 && renamed == target && data_at(target, payload, 8) && count("mkdir:") == directories && count("query:") == directories && accepted_directories;
        if (name == "config-nested") correct &= directory_query_controls();
        check(correct, "config complete identity, empty/leading-slash semantics, owned prefixes, actual ZIP/stat query and best-effort mkdir");
    } else if (name == "config-load-empty" || name == "config-load-leading-slash" || name == "config-delete-empty" || name == "config-delete-leading-slash") {
        const char *input = name.find("empty") != std::string::npos ? "" : "/Game.cfg";
        const std::string target = "/media/fat/config/" + std::string(input);
        const bool load = name.find("config-load-") == 0;
        const int result = observe(load ? FileLoadConfig(input, nullptr, 0) : FileDeleteConfig(input));
        check(load ? result == 8 && loaded == target && load_reads == 0 && closes == 1 :
              result == 1 && events == std::vector<std::string>{"unlink:" + target},
              "config load query/delete preserve literal empty and leading-slash prefix semantics");
    } else if (name == "config-preflight-reject") {
        const std::string input = "inputs/" + std::string(2075, 'G');
        const int result = observe(FileSaveConfig(input.c_str(), payload, 8));
        check(reject_clean(result, ENAMETOOLONG), "entire nested config target rejected before directory or save endpoints");
    } else if (name == "config-load-long" || name == "config-save-long" || name == "config-delete-long") {
        const std::string input(249, 'G'), target = "/media/fat/config/" + input;
        uint8_t data[8] = {};
        int result;
        if (name == "config-load-long") result = observe(FileLoadConfig(input.c_str(), data, 8));
        else if (name == "config-save-long") result = observe(FileSaveConfig(input.c_str(), payload, 8));
        else result = observe(FileDeleteConfig(input.c_str()));
        check(name == "config-load-long" ? result == 8 && loaded == target && std::all_of(data, data + 8, [](uint8_t b) { return b == 0xA5; }) :
              name == "config-save-long" ? result == 8 && renamed == target && data_at(target, payload, 8) : result == 1 && events == std::vector<std::string>{"unlink:" + target}, "complete config name beyond inherited 248 allowance without truncation");
    } else if (name == "config-load-buffer-alias" || name == "config-save-buffer-alias") {
        char input[64] = "Ordinary.cfg";
        const std::string target = "/media/fat/config/Ordinary.cfg";
        uint8_t before[4]; std::memcpy(before, input, 4);
        const int result = observe(name == "config-load-buffer-alias" ? FileLoadConfig(input, input, 4) : FileSaveConfig(input, input, 4));
        check(name == "config-load-buffer-alias" ? result == 4 && loaded == target && std::all_of(input, input + 4, [](char b) { return static_cast<unsigned char>(b) == 0xA5; }) : result == 4 && renamed == target && data_at(target, before, 4), "config name/payload alias captured before endpoint reads or overwrites caller data");
    } else if (name == "config-target-bound" || name == "config-directory-bound") {
        const std::string input = name == "config-target-bound" ? std::string(2081, 'G') : std::string(2080, 'G') + "/";
        const int result = observe(FileSaveConfig(input.c_str(), payload, 8));
        bool directory_bound = true;
        for (const auto &event : events) if (event.find("query:") == 0 || event.find("mkdir:") == 0) directory_bound &= event.size() == 6 + 2098;
        bool correct = result == 8 && renamed == "/media/fat/config/" + input && renamed.size() == 2099 && opened.size() == 2103 && directory_bound && count("mkdir:") == (name == "config-directory-bound" ? 1U : 0U);
        if (name == "config-directory-bound") {
            query_kind = QUERY_ZIP_MISSING;
            const std::string zip_input = "archive.zip/" + std::string(2068, 'G') + "/";
            const std::string zip_target = "/media/fat/config/" + zip_input;
            const unsigned before_mkdir = count("mkdir:");
            correct &= FileSaveConfig(zip_input.c_str(), payload, 8) == 8 && renamed == zip_target &&
                       opened == zip_target + ".tmp" && minimum_query_append_room == 2 &&
                       count("mkdir:") == before_mkdir + 1 &&
                       std::find(events.begin(), events.end(), "mkdir:" + zip_target.substr(0, zip_target.size() - 1)) != events.end() &&
                       data_at(zip_target, payload, 8);
        }
        check(correct, "synthetic config target/temp and max directory bounds; ZIP query's exact final slash cannot change mkdir/target identity");
    } else if (name == "dip-mra-243" || name == "dip-mra-244" || name == "dip-mra-255") {
        const size_t length = name == "dip-mra-243" ? 243 : name == "dip-mra-244" ? 244 : 255;
        const std::string common(length - 5, 'G');
        const std::string ext = length == 255 ? ".MRA" : ".mra";
        const uint64_t values[2] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
        bool correct = true;
        std::string paths[2];
        for (unsigned index = 0; index < 2; index++) {
            const std::string xml = "folder/" + common + char('A' + index) + ext;
            select_arcade_dip_name(xml.c_str());
            paths[index] = "/media/fat/config/dips/" + common + char('A' + index) + ".dip";
            const auto *bytes = reinterpret_cast<const uint8_t *>(&values[index]);
            files[paths[index]] = std::vector<uint8_t>(bytes, bytes + sizeof values[index]);
            switches.dip_cur = 0;
            arcade_sw_load();
            correct &= std::strlen(switches.name) == length && loaded == paths[index] && switches.dip_cur == values[index];
            switches.dip_num = 1;
            switches.dip_cur ^= 0x1111111111111111ULL;
            switches.dip_saved = 0;
            arcade_sw_save();
            correct &= renamed == paths[index] && switches.dip_saved == switches.dip_cur && data_at(paths[index], &switches.dip_cur, 8);
        }
        check(correct && paths[0] != paths[1] && load_reads == 2 && count("rename:") == 2, "reachable complete MRA components and distinct same-prefix DIP payload identities");
    } else if (name == "dip-internal-suffix" || name == "dip-internal-case" ||
               name == "dip-internal-adjacent" || name == "dip-internal-distinct") {
        missing_map_load = true;
        const std::vector<std::string> basenames = name == "dip-internal-suffix" ? std::vector<std::string>{"A.mra.extra.mra"} :
            name == "dip-internal-case" ? std::vector<std::string>{"A.MRA.extra.MRA"} :
            name == "dip-internal-adjacent" ? std::vector<std::string>{"A.mra.mra"} :
            std::vector<std::string>{"A.mra.extraA.mra","A.mra.extraB.mra"};
        bool correct = true;
        for (size_t index = 0; index < basenames.size(); index++) {
            const std::string expected = basenames[index].substr(0,basenames[index].size()-4) + ".dip";
            const std::string target = "/media/fat/config/dips/" + expected;
            const uint64_t wanted = 0x0123456789abcdefULL + index;
            const auto *bytes = reinterpret_cast<const uint8_t *>(&wanted);
            files[target] = std::vector<uint8_t>(bytes,bytes+sizeof wanted);
            select_arcade_dip_name(("folder/"+basenames[index]).c_str());
            dip_inputs.push_back(basenames[index]); dip_names.push_back(switches.name);
            switches.dip_cur = 0; arcade_sw_load();
            dip_loaded_values.push_back(switches.dip_cur); dip_wanted_values.push_back(wanted);
            const std::string actual_load = loaded;
            switches.dip_num = 1; switches.dip_cur = wanted ^ 0x1111111111111111ULL; switches.dip_saved = 0;
            arcade_sw_save(); dip_targets.push_back(renamed); dip_saved_markers.push_back(switches.dip_saved);
            const size_t count = file_api_returns.size();
            correct &= dip_names.back() == expected && actual_load == target && dip_loaded_values.back() == wanted &&
                renamed == target && data_at(target,&switches.dip_cur,8) && switches.dip_saved == switches.dip_cur &&
                count == 2*(index+1) && file_api_returns[count-2].api == "FileLoadConfig" &&
                file_api_returns[count-2].name == "dips/"+expected && file_api_returns[count-2].value == 8 &&
                file_api_returns[count-1].api == (HAVE_CHECKED_MUTATION ? "FileSaveConfig" : "FileSave") &&
                file_api_returns[count-1].value == 8;
        }
        if (basenames.size()==2) correct &= dip_targets[0] != dip_targets[1] &&
            data_at(dip_targets[0],&dip_saved_markers[0],8) && data_at(dip_targets[1],&dip_saved_markers[1],8);
        dip_identity_completed = true;
        check(correct,"complete terminal suffix DIP load/save statuses, distinct payload identity and saved marker");
    } else if (name == "dip-reject-load" || name == "dip-reject-save") {
        std::memset(switches.name, 'G', sizeof switches.name - 1);
        switches.name[sizeof switches.name - 1] = 0;
        switches.dip_num = 1; switches.dip_cur = 0x123456789abcdef0ULL; switches.dip_saved = 0x876543210ULL;
        if (name == "dip-reject-load") arcade_sw_load(); else arcade_sw_save();
        check(errno == ENAMETOOLONG && events.empty() && switches.dip_cur == 0x123456789abcdef0ULL && switches.dip_saved == 0x876543210ULL, "synthetic rejected DIP producer preserves bytes/marker and has no endpoints");
    } else if (name == "dip-save-failure" || name == "dip-load-failure" || name == "dip-noop") {
        select_arcade_dip_name("folder/Game.mra");
        switches.dip_num = 1; switches.dip_cur = 0x1234; switches.dip_saved = 0x5678;
        if (name == "dip-save-failure") { fault = 1; arcade_sw_save(); check(switches.dip_saved == 0x5678 && switches.dip_cur == 0x1234 && count("rename:") == 0, "DIP save marker advances only after a nonzero save result"); }
        else if (name == "dip-load-failure") { fail_load = true; arcade_sw_load(); check(switches.dip_cur == 0x1234 && switches.dip_saved == 0x5678 && load_reads == 0, "failed accepted DIP load preserves DIP bytes"); }
        else { switches.dip_saved = switches.dip_cur; arcade_sw_save(); switches.dip_num = 0; switches.dip_saved = 0; arcade_sw_save(); check(events.empty(), "DIP no-change/no-switch save performs no endpoints"); }
    } else {
#if HAVE_CHECKED_MUTATION
        if (name == "mutation-null" || name == "config-null") {
            bool correct = true;
            if (name == "mutation-null") { correct &= reject_clean(FileSave(nullptr, payload, 8), EINVAL); correct &= reject_clean(FileDelete(nullptr), EINVAL); correct &= reject_clean(DirDelete(nullptr), EINVAL); }
            else { correct &= reject_clean(FileLoadConfig(nullptr, payload, 8), EINVAL); correct &= reject_clean(FileSaveConfig(nullptr, payload, 8), EINVAL); correct &= reject_clean(FileDeleteConfig(nullptr), EINVAL); }
            check(correct, "synthetic NULL name controls reject before all endpoints");
        } else if (name == "config-null-root") {
            bool correct = true;
            for (unsigned root_kind = 0; root_kind < 3; root_kind++) {
                null_root = root_kind == 0;
                storage_root = root_kind == 1 ? "relative-root" : "";
                std::array<char, sizeof full_path> prior_path;
                std::memcpy(prior_path.data(), full_path, sizeof full_path);
                std::array<uint8_t, sizeof payload> data, prior_data;
                std::memcpy(data.data(), payload, sizeof payload);
                prior_data = data;
                const int load_status = FileLoadConfig("Game.cfg", data.data(), static_cast<int>(data.size()));
                correct &= load_status == 0 && errno == EINVAL && events.empty() &&
                           !std::memcmp(data.data(), prior_data.data(), data.size()) &&
                           !std::memcmp(full_path, prior_path.data(), sizeof full_path);
                const int save_status = FileSaveConfig("inputs/Game.cfg", data.data(), static_cast<int>(data.size()));
                correct &= save_status == 0 && errno == EINVAL && events.empty() &&
                           !std::memcmp(data.data(), prior_data.data(), data.size()) &&
                           !std::memcmp(full_path, prior_path.data(), sizeof full_path);
                const int delete_status = FileDeleteConfig("Game.cfg");
                correct &= delete_status == 0 && errno == EINVAL && events.empty() &&
                           !std::memcmp(data.data(), prior_data.data(), data.size()) &&
                           !std::memcmp(full_path, prior_path.data(), sizeof full_path);
                GuardedOutput<64> out;
                const auto prior_output = out.bytes;
                correct &= !resolve_config_path("Game.cfg", out.data(), out.bytes.size(), nullptr) &&
                           errno == EINVAL && out.unchanged(prior_output) && events.empty() &&
                           !std::memcmp(full_path, prior_path.data(), sizeof full_path);
            }
            check(correct, "all three config wrappers/private builder reject NULL, relative and empty roots with EINVAL and full unchanged output");
        } else if (name == "mutation-root-reject" || name == "mutation-null-root" || name == "config-root-reject") {
            null_root = name.find("null-root") != std::string::npos;
            if (!null_root) storage_root = "/" + std::string(2098, 'R');
            const int result = observe(name.find("config-") == 0 ? FileSaveConfig("inputs/Game.cfg", payload, 8) : FileSave("Game.bin", payload, 8));
            check(reject_clean(result, null_root ? EINVAL : ENAMETOOLONG), "synthetic invalid root rejected before directory or mutation endpoints");
        } else if (name == "mutation-allocation" || name == "config-allocation") {
            const std::string input(200, 'G');
            int result;
            { AllocationFailure failure; result = name == "config-allocation" ? FileSaveConfig(input.c_str(), payload, 8) : FileSave(input.c_str(), payload, 8); }
            observe(result);
            check(reject_clean(result, ENOMEM), "actual std::string allocation failure rejects before side effects");
        } else if (name == "recovery") {
            const std::string oversized = "inputs/" + std::string(2075, 'G');
            bool correct = reject_clean(FileSaveConfig(oversized.c_str(), payload, 8), ENAMETOOLONG);
            correct &= FileSaveConfig("inputs/Recovered.cfg", payload, 8) == 8 && renamed == "/media/fat/config/inputs/Recovered.cfg" && data_at(renamed, payload, 8);
            events.clear();
            std::memset(switches.name, 'G', sizeof switches.name - 1); switches.name[sizeof switches.name - 1] = 0;
            switches.dip_num = 1; switches.dip_cur = 0x12345678; switches.dip_saved = 0x87654321;
            arcade_sw_load(); correct &= events.empty() && switches.dip_cur == 0x12345678;
            arcade_sw_save(); correct &= events.empty() && switches.dip_saved == 0x87654321;
            select_arcade_dip_name("folder/Recovered.mra");
            arcade_sw_save(); correct &= renamed == "/media/fat/config/dips/Recovered.dip" && switches.dip_saved == switches.dip_cur && data_at(renamed, &switches.dip_cur, 8);
            check(correct, "path/DIP rejection can recover later using accepted complete targets and unchanged payload");
        } else if (name == "config-builder") {
            GuardedOutput<64> out; out.set("Ordinary.cfg"); size_t offset = 9999;
            bool correct = resolve_config_path(out.data(), out.data(), out.bytes.size(), &offset) == 1 && !std::strcmp(out.data(), "/media/fat/config/Ordinary.cfg") && offset == 18 && out.intact();
            out.set("KEEP"); const auto before = out.bytes;
            correct &= !resolve_config_path(nullptr, out.data(), out.bytes.size(), &offset) && errno == EINVAL && out.unchanged(before);
            correct &= !resolve_config_path("Game", out.data(), 0, &offset) && errno == EINVAL && out.unchanged(before);
            correct &= !resolve_config_path("Game", nullptr, out.bytes.size(), &offset) && errno == EINVAL && out.unchanged(before);
            correct &= !resolve_config_path("Game", out.data(), 20, &offset) && errno == ENAMETOOLONG && out.unchanged(before);
            const std::string exact = "/media/fat/config/Game.cfg";
            correct &= resolve_config_path("Game.cfg", out.data(), exact.size() + 1, &offset) == 1 && !std::strcmp(out.data(), exact.c_str()) && out.intact();
            out.set("KEEP"); const auto bound_before = out.bytes;
            correct &= !resolve_config_path("Game.cfg", out.data(), exact.size(), &offset) && errno == ENAMETOOLONG && out.unchanged(bound_before);
            const std::string long_name(200, 'G'); GuardedOutput<512> large; const auto large_before = large.bytes;
            { AllocationFailure failure; correct &= !resolve_config_path(long_name.c_str(), large.data(), large.bytes.size(), &offset) && errno == ENOMEM; }
            correct &= large.unchanged(large_before) && events.empty();
            check(correct, "private config builder exact/one-short capacity, NULL/alias/allocation and full unchanged guarded output");
        } else if (name == "temp-builder") {
            const std::string target = "/" + std::string(2098, 'G'); GuardedOutput<2104> tmp;
            bool correct = prepare_save_temp(target.c_str(), tmp.data(), tmp.bytes.size()) == 1 && std::strlen(tmp.data()) == 2103 && std::string(tmp.data()) == target + ".tmp" && tmp.intact();
            tmp.set("KEEP"); const auto before = tmp.bytes;
            correct &= !prepare_save_temp(target.c_str(), tmp.data(), 2103) && errno == ENAMETOOLONG && tmp.unchanged(before);
            correct &= !prepare_save_temp("", tmp.data(), 4) && errno == ENAMETOOLONG && tmp.unchanged(before) && events.empty();
            check(correct, "private temp builder 2104 exact/2103 reject bound and full unchanged guarded output");
        } else if (name == "dip-builder") {
            GuardedOutput<1024> out; out.set("Game.dip");
            bool correct = arcade_dip_path(out.data(), out.data(), out.bytes.size()) == 1 && !std::strcmp(out.data(), "dips/Game.dip") && out.intact();
            out.set("KEEP"); const auto before = out.bytes;
            correct &= !arcade_dip_path(nullptr, out.data(), out.bytes.size()) && errno == EINVAL && out.unchanged(before);
            correct &= !arcade_dip_path("Game.dip", nullptr, out.bytes.size()) && errno == EINVAL && out.unchanged(before);
            correct &= !arcade_dip_path("Game.dip", out.data(), 0) && errno == EINVAL && out.unchanged(before);
            const std::string exact(1018, 'G'), excessive(1019, 'G');
            correct &= arcade_dip_path(exact.c_str(), out.data(), out.bytes.size()) == 1 && std::strlen(out.data()) == 1023 && out.intact();
            out.set("KEEP"); const auto bound_before = out.bytes;
            correct &= !arcade_dip_path(excessive.c_str(), out.data(), out.bytes.size()) && errno == ENAMETOOLONG && out.unchanged(bound_before);
            correct &= !arcade_dip_path(exact.c_str(), out.data(), 1023) && errno == ENAMETOOLONG && out.unchanged(bound_before);
            { AllocationFailure failure; correct &= !arcade_dip_path(exact.c_str(), out.data(), out.bytes.size()) && errno == ENOMEM; }
            correct &= out.unchanged(bound_before) && events.empty();
            check(correct, "private DIP builder aliases/exact capacity/NULL/allocation and full unchanged guarded output");
        } else throw std::runtime_error("Unknown scenario");
#else
        excluded = true;
        check(true, "New private-builder or invalid-input control explicitly excluded on prior source");
#endif
    }
}
static void json_string(const std::string &value)
{
    std::putchar('"');
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') std::putchar('\\');
        if (c >= 32) std::putchar(c); else std::printf("\\u%04x", static_cast<unsigned>(c));
    }
    std::putchar('"');
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    reset();
    try { run_case(argv[1]); }
    catch (const Boundary &error) { fail_new = false; trapped = true; boundary = error.kind; check(false, "Historical unsafe producer trapped before libc write; scenario ends here"); }
    catch (const std::exception &error) { fail_new = false; std::fprintf(stderr, "Unexpected fixture/source error: %s\n", error.what()); return 2; }
    const int result_errno = errno;
    std::printf("JSON {\"case\":"); json_string(argv[1]);
    std::printf(",\"checks\":%u,\"failures\":%u,\"excluded\":%s,\"trapped\":%s,\"boundary\":", checks, failures, excluded ? "true" : "false", trapped ? "true" : "false"); json_string(boundary);
    std::printf(",\"return_observed\":%s,\"return_value\":", observed_return ? "true" : "false");
    if (observed_return) std::printf("%d", observed_status); else std::printf("null");
    std::printf(",\"errno\":%d,\"root_calls\":%u,\"load_reads\":%u,\"load_closes\":%u,\"dip_cur\":%llu,\"dip_saved\":%llu,\"opened\":", result_errno, root_calls, load_reads, closes, static_cast<unsigned long long>(switches.dip_cur), static_cast<unsigned long long>(switches.dip_saved)); json_string(opened);
    std::printf(",\"renamed\":"); json_string(renamed); std::printf(",\"loaded\":"); json_string(loaded);
    std::printf(",\"load_argument\":"); json_string(load_argument); std::printf(",\"events\":[");
    for (size_t index = 0; index < events.size(); index++) { if (index) std::putchar(','); json_string(events[index]); }
    std::printf("],\"file_api_returns\":[");
    for (size_t i=0;i<file_api_returns.size();i++) {
        if(i) std::putchar(','); std::printf("{\"api\":"); json_string(file_api_returns[i].api);
        std::printf(",\"name\":"); json_string(file_api_returns[i].name);
        std::printf(",\"return_observed\":true,\"return_value\":%d}",file_api_returns[i].value);
    }
    std::printf("],\"dip_identity_completed\":%s",dip_identity_completed ? "true":"false");
    auto strings=[](const char *key,const std::vector<std::string> &v) {
        std::printf(",\"%s\":[",key); for(size_t i=0;i<v.size();i++) { if(i) std::putchar(','); json_string(v[i]); } std::putchar(']');
    };
    auto integers=[](const char *key,const std::vector<uint64_t> &v) {
        std::printf(",\"%s\":[",key); for(size_t i=0;i<v.size();i++) { if(i) std::putchar(','); std::printf("\"%llu\"",static_cast<unsigned long long>(v[i])); } std::putchar(']');
    };
    strings("dip_inputs",dip_inputs); strings("dip_names",dip_names); strings("dip_targets",dip_targets);
    integers("dip_loaded_values",dip_loaded_values); integers("dip_wanted_values",dip_wanted_values); integers("dip_saved_markers",dip_saved_markers);
    std::printf(",\"real_filesystem_operations\":0}\n");
    return failures ? 1 : 0;
}
