#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <pthread.h>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>
#include "scaler.h"
#include "offload.h"
#include "profiling.h"
#include "cfg.h"
#include "Imlib2.h"
#include "screenshot_types.inc"

static thread_local bool resolver_fail_new;
// Keep the matching malloc/free-backed replacement pair behind real call
// boundaries; inlining only one side makes GCC assume its default allocation.
__attribute__((noinline)) void *operator new(std::size_t size)
{
    if (resolver_fail_new) throw std::bad_alloc();
    if (void *memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
__attribute__((noinline)) void *operator new[](std::size_t size) { return ::operator new(size); }
__attribute__((noinline)) void operator delete(void *memory) noexcept { std::free(memory); }
__attribute__((noinline)) void operator delete[](void *memory) noexcept { std::free(memory); }
__attribute__((noinline)) void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
__attribute__((noinline)) void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

static std::thread::id main_thread;
static thread_local bool foreign_resolver;
static std::string storage_root = "/root-A";
static std::atomic<int> worker_root_calls, worker_path_calls, foreign_path_calls;
const char *getRootDir()
{
    if (std::this_thread::get_id() != main_thread && !foreign_resolver) ++worker_root_calls;
    return storage_root.c_str();
}
static char CoreName2[32] = "TestCore";
char last_filename[1024] = "Active Game";
cfg_t cfg{};
static int create_calls, type_calls;
void create_path(const char *, const char *) { ++create_calls; }
uint32_t getFileType(const char *) { return ++type_calls == 1 ? 1 : 0; }
static std::tm fixed_time;
static time_t fixture_time(time_t *) { return 1; }
static std::tm *fixture_localtime(const time_t *) { return &fixed_time; }
#include "screenshot_paths.inc"
const char *getFullPath(const char *name)
{
    if (std::this_thread::get_id() != main_thread) {
        if (foreign_resolver) ++foreign_path_calls; else ++worker_path_calls;
    }
    return production_getFullPath(name);
}

static std::mutex memory_mutex;
static std::map<void *, std::size_t> owned_memory;
static int strdup_calls, malloc_calls, fail_strdup_at, fail_malloc_at;
static std::atomic<int> invalid_frees;
static void *tracked_malloc(std::size_t size)
{
    if (++malloc_calls == fail_malloc_at) return nullptr;
    void *result = std::malloc(size);
    if (result) { std::lock_guard<std::mutex> lock(memory_mutex); owned_memory[result] = size; }
    return result;
}
static char *tracked_strdup(const char *value)
{
    if (++strdup_calls == fail_strdup_at) return nullptr;
    char *result = static_cast<char *>(std::malloc(std::strlen(value) + 1));
    if (result) {
        std::memcpy(result, value, std::strlen(value) + 1);
        std::lock_guard<std::mutex> lock(memory_mutex); owned_memory[result] = std::strlen(value) + 1;
    }
    return result;
}
static void tracked_free(void *value)
{
    if (!value) return;
    std::lock_guard<std::mutex> lock(memory_mutex);
    if (!owned_memory.erase(value)) { ++invalid_frees; return; }
    std::free(value);
}
static std::size_t live_memory() { std::lock_guard<std::mutex> lock(memory_mutex); return owned_memory.size(); }

static std::vector<std::string> notices;
static bool wrong_notice_thread;
void Info(const char *message)
{
    wrong_notice_thread |= std::this_thread::get_id() != main_thread;
    notices.emplace_back(message);
}
static mister_scaler scaler_fixture{};
static bool fail_scaler_init, rotated;
static int scaler_reads, scaler_frees;
mister_scaler *mister_scaler_init() { return fail_scaler_init ? nullptr : &scaler_fixture; }
int mister_scaler_read(mister_scaler *scaler, unsigned char *data, mister_scaler_format_t format)
{
    ++scaler_reads;
    if (format != ARGB32) std::abort();
    std::memset(data, 0x7B, scaler->width * scaler->height * 4);
    return 1;
}
void mister_scaler_free(mister_scaler *scaler) { if (scaler != &scaler_fixture) std::abort(); ++scaler_frees; }
int video_get_rotated() { return rotated; }

static int base_image_token, scaled_image_token;
static Imlib_Image active_image;
static bool fail_image, fail_scale, fail_save, mutate_at_save;
static int base_creates, scaled_creates, image_frees, save_calls, scaled_width_seen, scaled_height_seen;
static int anti_alias_calls, dither_calls, blend_calls;
static std::string saved_path;
extern "C" {
Imlib_Image imlib_create_image_using_data(int width, int height, DATA32 *data)
{
    ++base_creates;
    if (width != scaler_fixture.width || height != scaler_fixture.height || !data) std::abort();
    return fail_image ? nullptr : &base_image_token;
}
void imlib_context_set_image(Imlib_Image image) { active_image = image; }
void imlib_context_set_anti_alias(char value) { if (value) std::abort(); ++anti_alias_calls; }
void imlib_context_set_dither(char value) { if (value) std::abort(); ++dither_calls; }
void imlib_context_set_blend(char value) { if (value) std::abort(); ++blend_calls; }
Imlib_Image imlib_create_cropped_scaled_image(int x, int y, int width, int height, int out_width, int out_height)
{
    ++scaled_creates;
    if (x || y || width != scaler_fixture.width || height != scaler_fixture.height || active_image != &base_image_token) std::abort();
    scaled_width_seen = out_width; scaled_height_seen = out_height;
    return fail_scale ? nullptr : &scaled_image_token;
}
void imlib_free_image_and_decache()
{
    if (active_image != &base_image_token && active_image != &scaled_image_token) std::abort();
    ++image_frees; active_image = nullptr;
}
void imlib_save_image_with_error_return(const char *filename, Imlib_Load_Error *error)
{
    ++save_calls;
    // Same worker thread, before the foreign library consumes its path. No race.
    if (mutate_at_save) {
        foreign_resolver = true;
        storage_root = "/root-C";
        getFullPath("Interfering.dat");
        foreign_resolver = false;
    }
    saved_path = filename;
    *error = fail_save ? IMLIB_LOAD_ERROR_PERMISSION_DENIED_TO_WRITE : IMLIB_LOAD_ERROR_NONE;
}
}

// Only platform affinity setup is substituted; queue and worker are intact.
struct FixtureCpuSet { unsigned int bits; };
static int fixture_affinity(pthread_attr_t *, std::size_t, FixtureCpuSet *) { return 0; }
#ifdef CPU_ZERO
#undef CPU_ZERO
#endif
#ifdef CPU_SET
#undef CPU_SET
#endif
#define cpu_set_t FixtureCpuSet
#define CPU_ZERO(set) ((set)->bits = 0)
#define CPU_SET(cpu, set) ((set)->bits |= 1U << (cpu))
#define pthread_attr_setaffinity_np fixture_affinity
#include "offload_production.inc"
#undef pthread_attr_setaffinity_np
#undef CPU_SET
#undef CPU_ZERO
#undef cpu_set_t

#define malloc tracked_malloc
#define strdup tracked_strdup
#define free tracked_free
#include "screenshot_production.inc"
#undef free
#undef strdup
#undef malloc

static int checks, failures;
static void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
static bool excluded;
static void resolver_cases()
{
#if HAVE_OWNED_RESOLVER
    auto resolve = [](const char *root, const char *name, const char *expected, int mode = 0) {
        char buffer[128]; std::memset(buffer, 'G', sizeof(buffer));
        const std::size_t capacity = std::strlen(expected) + 1;
        check(FileResolveFullPath(root, name, buffer, capacity, mode) == 1, "complete resolver path must fit exactly through its NUL");
        check(!std::strcmp(buffer, expected) && buffer[capacity] == 'G', "resolved text and right guard must remain exact");
    };
    resolve("/root", "File.bin", "/root/File.bin"); resolve(nullptr, "/absolute/File.bin", "/absolute/File.bin");
    resolve("/unused", "Disk", "/Disk", -1); resolve(nullptr, "Disk", "/Disk", -1);
    resolve(nullptr, "/Disk", "/Disk", -1); resolve("", "", "/"); resolve("/root/", "Name", "/root//Name");
    char default_mode[32]; check(FileResolveFullPath("/root", "Name", default_mode, sizeof(default_mode)) && !std::strcmp(default_mode, "/root/Name"), "actual header default must select ordinary resolution");
    for (int kind = 0; kind < 5; ++kind) {
        char buffer[64]; std::memset(buffer, 'U', sizeof(buffer));
        char original[64]; std::memcpy(original, buffer, sizeof(buffer));
        errno = 0;
        const int result = kind == 0 ? FileResolveFullPath("/root", "Name", buffer, 10) :
                           kind == 1 ? FileResolveFullPath(nullptr, "Name", buffer, sizeof(buffer)) :
                           kind == 2 ? FileResolveFullPath("/root", nullptr, buffer, sizeof(buffer)) :
                           kind == 3 ? FileResolveFullPath("/root", "Name", buffer, 0) :
                                       FileResolveFullPath("/root", "Name", nullptr, sizeof(buffer));
        check(!result && errno == (kind == 0 ? ENAMETOOLONG : EINVAL), "bounds or invalid input must return the proper error");
        check(!std::memcmp(buffer, original, sizeof(buffer)), "failed resolver must preserve the entire destination and guards");
    }
    char alias[128];
    std::strcpy(alias, "/Root"); check(FileResolveFullPath(alias, "Name", alias, sizeof(alias)) && !std::strcmp(alias, "/Root/Name"), "root may completely alias output");
    std::strcpy(alias, "Name"); check(FileResolveFullPath("/Root", alias, alias, sizeof(alias)) && !std::strcmp(alias, "/Root/Name"), "name may completely alias output");
    std::strcpy(alias, "Same"); check(FileResolveFullPath(alias, alias, alias, sizeof(alias)) && !std::strcmp(alias, "Same/Same"), "both relative inputs may alias output");
    std::memset(alias, 'Z', sizeof(alias)); std::strcpy(alias + 3, "/Root"); std::strcpy(alias + 24, "Name");
    check(FileResolveFullPath(alias + 3, alias + 24, alias + 2, 64) && !std::strcmp(alias + 2, "/Root/Name") && alias[1] == 'Z' && alias[66] == 'Z', "root/name substrings may overlap an offset destination with guards");
    std::strcpy(alias, "Header/Name"); check(FileResolveFullPath("/Root", alias + 7, alias, sizeof(alias)) && !std::strcmp(alias, "/Root/Name"), "name substring may lie inside the destination");
    std::strcpy(alias, "/Keep"); char original[128]; std::memcpy(original, alias, sizeof(alias));
    check(!FileResolveFullPath(alias, "LongName", alias, 6) && errno == ENAMETOOLONG && !std::memcmp(alias, original, sizeof(alias)), "aliased overflow must preserve all original bytes");
    const std::string long_root = "/" + std::string(300, 'r'), long_name(300, 'n');
    char alloc_buffer[1024]; std::memset(alloc_buffer, 'A', sizeof(alloc_buffer));
    resolver_fail_new = true;
    const int result = FileResolveFullPath(long_root.c_str(), long_name.c_str(), alloc_buffer, sizeof(alloc_buffer));
    const int error = errno; resolver_fail_new = false;
    check(!result && error == ENOMEM, "focused std::string allocation failure must report ENOMEM");
    check(std::all_of(alloc_buffer, alloc_buffer + sizeof(alloc_buffer), [](char c) { return c == 'A'; }), "allocation failure must leave output and canary bytes unchanged");
    check(!worker_root_calls && !worker_path_calls, "pure resolver must not consult storage or global path getters");
#else
    excluded = true; // No new API existed: do not turn missing API into a failure.
#endif
}

static void screenshot_case(const std::string &scenario)
{
#if !HAVE_OWNED_RESOLVER
    if (scenario == "savepath-alloc-fail" || scenario == "resolve-fail") { excluded = true; return; }
#endif
    fixed_time.tm_year = scenario == "nodate" ? 118 : 126;
    fixed_time.tm_mon = 0; fixed_time.tm_mday = 2; fixed_time.tm_hour = 3; fixed_time.tm_min = 4; fixed_time.tm_sec = 5;
    std::strcpy(cfg.screenshot_image_format, scenario == "bmp" ? "bmp" : scenario == "unknown-format" ? "wrong" : "png");
    scaler_fixture.width = 8; scaler_fixture.height = 6; scaler_fixture.output_width = 32; scaler_fixture.output_height = 18;
    const bool scaled = scenario == "scaled" || scenario == "rotated" || scenario == "scale-fail";
    rotated = scenario == "rotated"; fail_image = scenario == "image-fail"; fail_scale = scenario == "scale-fail";
    fail_save = scenario == "save-fail"; fail_scaler_init = scenario == "init-fail";
    if (scenario == "oversize") { scaler_fixture.width = MISTER_SCALER_BUFFERSIZE; scaler_fixture.height = 2; }
    if (scenario == "request-alloc-fail") fail_strdup_at = 1;
    if (scenario == "filename-alloc-fail") fail_strdup_at = 2;
    if (scenario == "savepath-alloc-fail") fail_strdup_at = 3;
    if (scenario == "result-alloc-fail") fail_malloc_at = 1;
    if (scenario == "resolve-fail") storage_root = "/" + std::string(2079, 'r'); // Directory-only legacy join still fits.
    const bool generated = scenario == "default-name" || scenario == "nodate" || scenario == "bmp" || scenario == "unknown-format";
    std::string command = generated ? "" : " \t\nUser.png";
    const int rescale = scenario == "init-fail" || scenario == "oversize" ? 1 : scaled;
    const std::string relative = scenario == "nodate" ? "screenshots/TestCore/NODATE-Active Game_0002.png" :
        scenario == "bmp" ? "screenshots/TestCore/20260102_030405-Active Game.bmp" :
        generated ? "screenshots/TestCore/20260102_030405-Active Game.png" : "screenshots/User.png";
    const std::string root_at_capture = storage_root;

    std::mutex gate_mutex; std::condition_variable gate_cv;
    bool parked = false, released = false;
    offload_start();
    offload_add_work([&]() {
        std::unique_lock<std::mutex> lock(gate_mutex); parked = true; gate_cv.notify_all();
        gate_cv.wait(lock, [&]() { return released; });
    });
    { std::unique_lock<std::mutex> lock(gate_mutex); gate_cv.wait(lock, [&]() { return parked; }); }
    request_screenshot(generated ? nullptr : command.data(), rescale);
    if (scenario != "request-alloc-fail") {
        const int copies = strdup_calls;
        request_screenshot(command.data(), 0);
        check(strdup_calls == copies && screenshot_requested, "already requested screenshot must ignore duplicate input");
    }
    screenshot_cb();
    const bool no_worker = scenario == "request-alloc-fail" || scenario == "filename-alloc-fail" || scenario == "savepath-alloc-fail" ||
                           scenario == "init-fail" || scenario == "oversize" || scenario == "resolve-fail";
    check(!screenshot_requested && !screenshot_filename, "capture must consume the requested name once");
    check(bool(screenshot_pending_atomic) == !no_worker, "pending flag must track whether image work was queued");
    if (!no_worker) {
        const int copies = strdup_calls; request_screenshot(command.data(), 0);
        check(strdup_calls == copies && !screenshot_requested, "pending worker must prevent a second screenshot request");
        check(live_memory() == (HAVE_OWNED_RESOLVER ? 2U : 1U), "queued operation must own its intended name/path allocations");
    }
    if (scenario == "root-change") storage_root = "/root-B";
    mutate_at_save = scenario == "foreign-path";
    { std::lock_guard<std::mutex> lock(gate_mutex); released = true; gate_cv.notify_all(); }
    offload_stop(); // The actual queue drains its screenshot before quit.
    check(!screenshot_pending_atomic, "worker or capture failure must retire the pending flag");
    check(!worker_root_calls && !worker_path_calls, "worker must not resolve a path or consult the storage root");
    if (no_worker) {
        check(!save_calls && !base_creates && !screenshot_result_data_atomic, "capture rejection must queue no image or result work");
        check(!live_memory() && !invalid_frees, "capture rejection must free transferred request and partial allocations");
        if (scenario != "request-alloc-fail") check(!screenshot_rescale, "failed capture must clear rescale state");
    } else {
        check(bool(screenshot_result_data_atomic) == (scenario != "result-alloc-fail"), "worker must publish exactly the expected result allocation");
        check(live_memory() == (scenario == "result-alloc-fail" ? 0U : 2U), "worker must free owned absolute path and retain only callback-owned result/name");
        if (!fail_image && !fail_scale) {
            check(save_calls == 1 && saved_path == root_at_capture + "/" + relative, "foreign save must consume the enqueue-time owned absolute path");
            check(image_frees == (scaled ? 2 : 1), "all created base/scaled images must be released after save");
        } else check(!save_calls && image_frees == (fail_scale ? 1 : 0), "image/scaling failure must release exactly the created images");
        if (scaled && !fail_image) check(scaled_width_seen == (rotated ? 24 : 32) && scaled_height_seen == 18, "native/rotated scaled dimensions must preserve capture aspect behavior");
    }
    screenshot_cb();
    const bool failed_image = fail_image || fail_scale || fail_save;
    if (!no_worker && scenario != "result-alloc-fail") {
        check(notices.size() == 1 && notices.back() == (failed_image ? "Screenshot failed" : "Screen saved to\n" + relative.substr(std::strlen(SCREENSHOT_DIR "/"))), "callback must retain relative successful notification or existing failure message");
    } else if (scenario == "init-fail") check(notices.size() == 1 && notices.back() == "Scaler not compatible", "scaler failure must retain its visible diagnostic");
    else if (scenario == "resolve-fail") check(notices.size() == 1 && notices.back() == "Screenshot failed", "path failure must report screenshot failure on capture thread");
    else check(notices.empty(), "silent allocation rejection must not invent a successful notification");
    check(!wrong_notice_thread && !screenshot_result_data_atomic, "only capture/callback thread may display and consume the result");
    check(!live_memory() && !invalid_frees, "callback must retire all result/name allocations without double frees");
    check(scaler_reads == (no_worker && (scenario == "init-fail" || scenario == "oversize" || scenario == "request-alloc-fail") ? 0 : 1), "framebuffer read must happen exactly when capture reached its read stage");
    if (scenario != "request-alloc-fail") check(scaler_frees == (fail_scaler_init ? 0 : 1), "capture must release its hardware scaler handle");
    if (scenario == "foreign-path") check(foreign_path_calls == 1, "safe foreign operation must actually mutate the historical global buffer");
    if (generated && scenario != "request-alloc-fail") check(create_calls == 1, "generated naming must preserve its directory creation boundary");
    // Historical cleanup leak is recorded, then its synthetic memory is retired.
    for (const auto &entry : owned_memory) std::free(entry.first);
    owned_memory.clear();
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    main_thread = std::this_thread::get_id();
    const std::string scenario = argv[1];
    if (scenario == "resolver") resolver_cases(); else screenshot_case(scenario);
    std::printf("JSON {\"checks\":%d,\"failures\":%d,\"excluded_absent_or_unsafe_api_case\":%s,\"resolver_available\":%s}\n",
                checks, failures, excluded ? "true" : "false", HAVE_OWNED_RESOLVER ? "true" : "false");
    return failures ? 1 : 0;
}
