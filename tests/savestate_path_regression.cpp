#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#ifndef __linux__
typedef off_t __off64_t;
#endif
#include "savestate_types.inc"

static bool fail_new;
// Matching replacement allocation/deallocation stay behind call boundaries.
__attribute__((noinline)) void *operator new(std::size_t size)
{ if (fail_new) throw std::bad_alloc(); if (void *p = std::malloc(size ? size : 1)) return p; throw std::bad_alloc(); }
__attribute__((noinline)) void *operator new[](std::size_t size) { return ::operator new(size); }
__attribute__((noinline)) void operator delete(void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void *p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

static char CoreName2[32] = "TestCore";
static bool arcade, apple, null_root;
static std::string storage_root = "/root-A";
bool is_arcade() { return arcade; }
bool is_apple2() { return apple; }
const char *user_io_get_core_name() { return "AppleCore"; }
const char *getRootDir() { return null_root ? nullptr : storage_root.c_str(); }
static uint32_t ss_base = 0x20000000, ss_size = 64;
static fileTYPE sd_image[16];
static std::vector<std::string> operations, opens, saves, exists_queries;
static std::vector<int> save_sizes;
static int generate_calls, resolve_calls, forced_generator_call;
static int map_calls, zero_calls, read_calls, close_calls, mkdir_calls, menu_calls, info_calls, timer_calls;
static int failed_map_slot = -1;
static bool fail_open, fail_mkdir, timer_ready = true;
struct Slot { uint32_t left = 0xBADC0FFE; std::array<uint32_t,16> words{}; uint32_t right = 0xC0FFEE11; };
static Slot slots[4];
static std::map<std::string, std::vector<uint32_t>> files;
static std::string opened_path;
static std::string absolute(const char *path)
{ return path[0] == '/' ? path : storage_root + "/" + path; }
void create_path(const char *base, const char *subdir)
{ operations.push_back("legacy-directory:" + absolute(base) + "/" + subdir); mkdir_calls += 2; }
static int fixture_mkdir(const char *path, mode_t mode)
{ if (mode != (S_IRWXU | S_IRWXG | S_IRWXO)) std::abort(); ++mkdir_calls; operations.push_back("mkdir:" + std::string(path)); return fail_mkdir ? -1 : 0; }

struct Boundary { const char *kind; };
static char *tracked_output;
static size_t tracked_capacity;
static int historical_boundaries, skipped_controls;
static bool excluded, terminated_unsafe_binding;
static size_t remaining(char *destination, const char *kind)
{
    if (!destination) throw Boundary{kind};
    const uintptr_t begin = reinterpret_cast<uintptr_t>(tracked_output), dest = reinterpret_cast<uintptr_t>(destination);
    if (dest < begin || dest - begin >= tracked_capacity) throw Boundary{"untracked-destination"};
    return tracked_capacity - (dest - begin);
}
static int bounded_sprintf(char *destination, const char *format, ...)
{
    const size_t room = remaining(destination, "NULL-sprintf");
    va_list args, copy; va_start(args, format); va_copy(copy, args);
    const int length = std::vsnprintf(nullptr, 0, format, copy); va_end(copy);
    if (length < 0 || size_t(length) >= room) { va_end(args); throw Boundary{"capacity-sprintf"}; }
    std::vector<char> text(size_t(length) + 1);
    std::vsnprintf(text.data(), text.size(), format, args); va_end(args);
    std::memcpy(destination, text.data(), text.size()); return length;
}
static char *bounded_strcat(char *destination, const char *text)
{
    const size_t room = remaining(destination, "NULL-strcat");
    const char *end = static_cast<const char *>(std::memchr(destination, 0, room));
    if (!end) throw Boundary{"unterminated-destination"};
    const size_t length = end - destination, extra = std::strlen(text);
    if (extra >= room - length) throw Boundary{"capacity-strcat"};
    const uintptr_t src = reinterpret_cast<uintptr_t>(text), dst = reinterpret_cast<uintptr_t>(destination + length);
    if (src < dst + extra + 1 && dst < src + extra + 1) throw Boundary{"overlapping-strcat"};
    std::memcpy(destination + length, text, extra + 1); return destination;
}
#include "savestate_paths.inc"

#if HAVE_CHECKED_SAVESTATE
int FileGenerateSavestatePath(const char *name, char *out, size_t capacity, int suffix)
{
    ++generate_calls; operations.push_back("generate");
    fail_new = generate_calls == forced_generator_call;
    const int result = production_generate(name, out, capacity, suffix);
    fail_new = false; return result;
}
#else
void FileGenerateSavestatePath(const char *name, char *out, int suffix)
{
    ++generate_calls; tracked_output = out; tracked_capacity = 1024;
    production_generate(name, out, suffix);
}
#endif
#if HAVE_RESOLVER
int FileResolveFullPath(const char *root, const char *name, char *out, size_t capacity, int mode)
{ ++resolve_calls; return production_resolve(root, name, out, capacity, mode); }
#endif
static int checked_generate(const char *name, char *out, size_t capacity, int suffix)
{
#if HAVE_CHECKED_SAVESTATE
    return FileGenerateSavestatePath(name, out, capacity, suffix);
#else
    if (!name) { ++skipped_controls; return -1; }
    tracked_output = out; tracked_capacity = capacity;
    // A completed void call is an observed path completion for the fixture,
    // not a historical API status. Real old process_ss keeps its void calls.
    try { production_generate(name, out, suffix); return 1; }
    catch (const Boundary &) { ++historical_boundaries; return 0; }
#endif
}
void *shmem_map(uint32_t address, uint32_t size)
{
    ++map_calls; operations.push_back("map");
    const int slot = int((address - ss_base) / ss_size);
    if (slot < 0 || slot > 3 || size != sizeof(slots[slot].words)) std::abort();
    return slot == failed_map_slot ? nullptr : slots[slot].words.data();
}
static void *trace_memset(void *memory, int value, size_t size)
{
    for (auto &slot : slots) if (memory == slot.words.data()) {
        if (size != sizeof(slot.words)) std::abort();
        ++zero_calls; operations.push_back("zero");
    }
    return std::memset(memory, value, size);
}
int FileExists(const char *name, int)
{ const std::string path = absolute(name); operations.push_back("exists"); exists_queries.push_back(path); return files.count(path); }
int FileOpen(fileTYPE *file, const char *name, char)
{
    opened_path = absolute(name); opens.push_back(opened_path); operations.push_back("open");
    if (fail_open) return 0;
    file->filp = reinterpret_cast<FILE *>(uintptr_t(1)); file->size = ss_size; return 1;
}
int FileReadAdv(fileTYPE *, void *data, int count, int)
{
    ++read_calls; operations.push_back("read");
    const auto found = files.find(opened_path);
    if (found == files.end() || count != int(ss_size)) std::abort();
    std::memcpy(data, found->second.data(), count); return count;
}
int FileClose(fileTYPE *file)
{ if (file->filp) { ++close_calls; file->filp = nullptr; } return 0; }
int FileSave(const char *name, void *data, int count)
{
    bool valid = false; for (auto &slot : slots) valid |= data == slot.words.data();
    if (!valid || count <= 0 || count > int(ss_size)) std::abort();
    operations.push_back("save"); saves.push_back(absolute(name)); save_sizes.push_back(count); return count;
}
void MenuHide() { ++menu_calls; }
void Info(const char *, unsigned long) { ++info_calls; }
unsigned long GetTimer(unsigned long delay) { if (delay != 1000) std::abort(); ++timer_calls; return 100; }
bool CheckTimer(unsigned long) { return timer_ready; }
#include "savestate_constructors.inc"
#include "savestate_binding.inc"

static int checks, failures;
static void check(bool condition, const char *message)
{ ++checks; if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); } }
static void clear_operations()
{ operations.clear(); opens.clear(); saves.clear(); save_sizes.clear(); exists_queries.clear(); generate_calls = resolve_calls = map_calls = zero_calls = read_calls = close_calls = mkdir_calls = menu_calls = info_calls = timer_calls = 0; }
static bool intact_maps()
{ for (auto &slot : slots) if (slot.left != 0xBADC0FFE || slot.right != 0xC0FFEE11) return false; return true; }
static void dirty_slots()
{ static uint32_t epoch=10; epoch+=4; for (int i = 0; i < 4; ++i) { slots[i].words[0] = epoch + i; slots[i].words[1] = 1; } }
static void seed_file(const std::string &name, uint32_t value)
{ files[name] = std::vector<uint32_t>(ss_size / 4, value); }
static std::string state(const char *stem, int slot, const char *core = "TestCore", const char *root = "/root-A")
{ return std::string(root) + "/savestates/" + core + "/" + stem + (slot ? "_" + std::to_string(slot) : "") + ".ss"; }

static void builder_cases()
{
    const std::vector<std::pair<std::string,std::string>> names = {
        {"Game.gba", "Game"}, {"folder.with.dot/Game.gba", "Game"}, {"Game", "Game"},
        {"folder.with.dot/Game", "Game"}, {"Game.rev.gba", "Game.rev"}, {"Game.", "Game"},
        {".gba", ""}, {"Unicode-\xC3\xA9.gba", "Unicode-\xC3\xA9"}};
    for (const auto &item : names) for (int suffix : {0, 1, 2, 3, 4, INT_MIN, INT_MAX}) {
        const std::string expected = "savestates/TestCore/" + item.second + (suffix ? "_" + std::to_string(suffix) : "") + ".ss";
        std::vector<char> output(expected.size() + 17, 'G'); char *out = output.data() + 8;
        clear_operations(); errno = 0;
        const int result = checked_generate(item.first.c_str(), out, expected.size() + 1, suffix);
        check(result == 1 && std::string(out) == expected, "builder must preserve complete basename/stem/suffix identity");
        check(std::all_of(output.begin(),output.begin()+8,[](char c){return c=='G';}) &&
              std::all_of(output.begin()+8+expected.size()+1,output.end(),[](char c){return c=='G';}), "builder exact fit must retain every outer guard byte");
        check(!mkdir_calls, "name generation must make no directory requests");
        std::fill(output.begin(),output.end(),'G'); clear_operations(); errno=0;
        const int short_result=checked_generate(item.first.c_str(),out,expected.size(),suffix);
        check(!short_result && errno==ENAMETOOLONG, "one byte below the exact required capacity must reject the complete path");
        check(std::all_of(output.begin(),output.end(),[](char c){return c=='G';}), "one-byte-short rejection must preserve output and every guard");
        check(!mkdir_calls, "one-byte-short rejection must have no filesystem preparation");
    }
    for (const std::string &name : {std::string("Game.gba"), std::string("Game"), std::string("folder/"), std::string("")}) {
        char out[80], original[80]; std::memset(out, 'U', sizeof(out)); std::memcpy(original, out, sizeof(out));
        errno = 0; const int result = checked_generate(name.c_str(), out, 24, 1);
        check(!result && errno == (name.empty() || name.back() == '/' ? EINVAL : ENAMETOOLONG), "rejected builder input must return its exact error");
        check(!std::memcmp(out, original, sizeof(out)), "rejected generation must leave every destination byte unchanged");
    }
    char alias[160]; std::strcpy(alias, "folder/Game.gba");
    check(checked_generate(alias, alias, sizeof(alias), 3) == 1 && !std::strcmp(alias, "savestates/TestCore/Game_3.ss"), "complete input/output alias must be supported");
    std::memset(alias, 'Z', sizeof(alias)); std::strcpy(alias + 20, "folder/Game.gba");
    check(checked_generate(alias + 20, alias + 2, 100, 0) == 1 && !std::strcmp(alias + 2, "savestates/TestCore/Game.ss") && alias[1] == 'Z' && alias[102] == 'Z', "substring alias must retain correct output and guards");
    std::strcpy(alias,"Game.gba"); char alias_before[160]; std::memcpy(alias_before,alias,sizeof(alias)); errno=0;
    check(!checked_generate(alias,alias,10,1) && errno==ENAMETOOLONG && !std::memcmp(alias,alias_before,sizeof(alias)), "aliased rejection must preserve the original input and destination");
    const std::string discarded = "Game." + std::string(3000, 'x'); char out[80] = {};
    check(checked_generate(discarded.c_str(), out, sizeof(out), 1) == 1 && !std::strcmp(out, "savestates/TestCore/Game_1.ss"), "a discarded long extension must not cause a transient capacity overflow");
    arcade = true;
    check(checked_generate("Game.mra", out, sizeof(out), 1) == 1 && !std::strcmp(out, "savestates/Arcade/Game_1.ss"), "Arcade directory selection must remain exact");
    arcade = false;
#if HAVE_CHECKED_SAVESTATE
    for (int kind = 0; kind < 3; ++kind) {
        char unchanged[80]; std::memset(unchanged, 'Q', sizeof(unchanged)); errno = 0;
        const int result = kind == 0 ? checked_generate(nullptr, unchanged, sizeof(unchanged), 1) : kind == 1 ? checked_generate("Game.gba", nullptr, 80, 1) : checked_generate("Game.gba", unchanged, 0, 1);
        check(!result && errno == EINVAL && std::all_of(unchanged, unchanged + 80, [](char c) { return c == 'Q'; }), "invalid builder pointers/capacity must preserve destination bytes");
    }
    clear_operations(); forced_generator_call = 1; std::memset(out, 'A', sizeof(out)); errno = 0;
    const int result = checked_generate("Game.gba", out, sizeof(out), 1); forced_generator_call = 0;
    check(!result && errno == ENOMEM && std::all_of(out, out + sizeof(out), [](char c) { return c == 'A'; }), "owned builder allocation failure must be transactional");
#else
    skipped_controls += 4;
#endif
}

static void lifecycle(const std::string &scenario)
{
#if !HAVE_CHECKED_SAVESTATE
    if (scenario == "allocation-reject" || scenario == "root-reject" || scenario == "null-root") { excluded = true; return; }
#endif
    if (scenario == "no-base") { ss_base = 0; check(!process_ss("Game.gba") && operations.empty(), "absent savestate base must perform no binding work"); return; }
    if (scenario == "arcade") arcade = true;
    if (scenario.compare(0, 6, "apple-") == 0) {
        apple = true;
        if (scenario == "apple-priority") { sd_image[0].size = 1; sd_image[2].size = 1; std::strcpy(sd_image[0].name, "folder/Primary.dsk"); std::strcpy(sd_image[2].name, "Secondary.dsk"); }
        if (scenario == "apple-dotless") { sd_image[0].size = 1; std::strcpy(sd_image[0].name, "NoExtension"); }
    }
    if (scenario == "map-fail") failed_map_slot = 2;
    if (scenario == "open-fail") fail_open = true;
    if (scenario == "mkdir-fail") fail_mkdir = true;
    const char *stem = arcade ? "Game" : apple ? scenario == "apple-priority" ? "Primary" : "AppleCore" : "Game";
    const char *core = arcade ? "Arcade" : "TestCore";
    const bool rejects = scenario == "empty-reject" || scenario == "long-reject" || scenario == "allocation-reject" || scenario == "root-reject" || scenario == "null-root" || scenario == "recovery";
    for (int slot = 1; slot <= 4; ++slot) seed_file(state(stem, slot, core), 0x100 + slot);
    if (scenario == "legacy") { files.erase(state(stem,1,core)); seed_file(state(stem,0,core),0x999); }
    check(process_ss("folder/Game.gba") == 1, "ordinary media must establish a complete savestate binding");
    check(intact_maps(), "binding reads/zeroing must retain map canaries");
    if (rejects || scenario == "disable") {
        std::array<std::array<uint32_t,16>,4> before; for(int i=0;i<4;++i) before[i]=slots[i].words;
        clear_operations();
        if (scenario == "allocation-reject") forced_generator_call = 3;
        if (scenario == "root-reject") { storage_root.assign(2090,'r'); storage_root[0]='/'; }
        if (scenario == "null-root") null_root = true;
        const std::string rejected = scenario == "long-reject" ? std::string(1020,'N') : "";
        const char *name = scenario == "disable" ? "Game.gba" : scenario == "allocation-reject" ? "NewGame.gba" : scenario == "root-reject" || scenario == "null-root" ? "NewGame.gba" : rejected.c_str();
        const int result = process_ss(name, scenario == "disable" ? 0 : 1);
        check(!result, "rejected or disabled media binding must report unavailable savestates");
        check(!map_calls && !zero_calls && !mkdir_calls && exists_queries.empty() && opens.empty() && !read_calls, "rejection must precede all map/memory/directory/file side effects");
        bool unchanged=true; for(int i=0;i<4;++i) unchanged &= before[i] == slots[i].words;
        check(unchanged, "rejected binding must preserve already owned map contents");
        forced_generator_call = 0; null_root = false; storage_root = "/root-A";
        clear_operations(); dirty_slots();
        check(!process_ss(nullptr) && saves.empty() && !menu_calls && !info_calls && !timer_calls, "later polling must not mutate suffix or write the previous game after rejection");
        if (scenario == "recovery") {
            check(process_ss("NewGame.gba") == 1, "a fitting later binding must recover after rejection"); clear_operations(); dirty_slots();
            check(process_ss(nullptr) == 1 && saves.size()==4 && saves[0]==state("NewGame",1), "recovered persistence must use only the newly accepted identity");
        }
        check(intact_maps(), "rejection/recovery must retain mapped buffer guards"); return;
    }
    check(mkdir_calls == (HAVE_CHECKED_SAVESTATE ? 2 : scenario == "legacy" ? 12 : scenario == "map-fail" ? 8 : 10), "directory preparation must retain existing modes and accepted binding scope");
    check(map_calls == 4 && zero_calls == (scenario == "map-fail" ? 3 : 4), "accepted binding must map/zero exactly available slots");
    check(opens.size() == size_t(scenario == "map-fail" ? 3 : 4), "all available existing slots must reach their read-open boundary");
    check(read_calls == (scenario == "open-fail" ? 0 : scenario == "map-fail" ? 3 : 4), "failed opens must preserve normal read suppression");
    check(opens.empty() || opens[0] == state(stem, scenario == "legacy" ? 0 : 1, core), "slot1 must prefer _1 or existing legacy fallback");
    check(close_calls==read_calls, "every successfully opened state file must be retired once");
    for(int i=0;i<4;++i) if(i!=failed_map_slot) check(slots[i].words[2]==(fail_open ? 0U : i==0 && scenario=="legacy" ? 0x999U : uint32_t(0x101+i)), "state reads must retain the selected slot file's actual payload after counter reset");
#if HAVE_CHECKED_SAVESTATE
    check(generate_calls==5 && resolve_calls==6, "binding must validate all four suffixed names, legacy fallback and captured-root paths");
    size_t last_generation=0,first_effect=operations.size();
    for(size_t i=0;i<operations.size();++i) {
        if(operations[i]=="generate") last_generation=i;
        if(operations[i].compare(0,6,"mkdir:")==0 || operations[i]=="map" || operations[i]=="zero" || operations[i]=="exists" || operations[i]=="open" || operations[i]=="read") first_effect=std::min(first_effect,i);
    }
    check(last_generation<first_effect, "all slot/fallback construction must finish before the first filesystem or map side effect");
#endif
    clear_operations(); dirty_slots();
    if (scenario == "size") { slots[0].words[1]=0; slots[1].words[1]=UINT_MAX; slots[2].words[1]=100; }
    if (scenario == "root-change") storage_root = "/root-B";
    check(process_ss(nullptr) == 1, "valid persistence poll must remain enabled");
    const size_t expected_saves = scenario == "map-fail" ? 3 : scenario == "size" ? 1 : 4;
    check(saves.size() == expected_saves && menu_calls==int(expected_saves) && info_calls==int(expected_saves), "only changed counters with an allowed declared state size may persist");
    size_t saved_index=0;
    for(int i=0;i<4;++i) if(i!=failed_map_slot && (scenario!="size" || i==3)) {
        check(saved_index<saves.size() && saves[saved_index]==state(stem,i+1,core), "poll writes must retain captured root and the exact corresponding slot identity"); ++saved_index;
    }
    if (scenario == "timer") {
        clear_operations(); dirty_slots(); timer_ready=false;
        check(!process_ss(nullptr) && saves.empty(), "unexpired timer must suppress persistence work"); timer_ready=true;
        check(process_ss(nullptr)==1 && saves.size()==4, "expired timer must allow later changed counters");
    }
    if (scenario == "counter") { clear_operations(); check(process_ss(nullptr)==1 && saves.empty(), "unchanged counters must not produce another save"); }
    if (scenario == "no-extension" || scenario == "dotted-parent") {
        clear_operations();
        check(process_ss(scenario=="dotted-parent" ? "folder.with.dot/Plain" : "Plain")==1, "extensionless media must bind through its actual basename");
        clear_operations(); dirty_slots();
        check(process_ss(nullptr)==1 && saves.size()==4 && saves[0]==state("Plain",1), "extensionless media must persist under complete suffixed names");
    }
    check(intact_maps(), "all lifecycle operations must preserve map canaries");
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    try { if (std::string(argv[1]) == "builder") builder_cases(); else lifecycle(argv[1]); }
    catch (const Boundary &boundary) {
        ++historical_boundaries; terminated_unsafe_binding=true;
        check(false, "historical unsafe binding terminated before libc write; no subsequent poll executed");
        std::fprintf(stderr,"BOUNDARY: %s\n",boundary.kind);
    }
    fail_new=false;
    std::string sizes="["; for(size_t i=0;i<save_sizes.size();++i) sizes+=(i?",":"")+std::to_string(save_sizes[i]); sizes+="]";
    std::printf("JSON {\"checks\":%d,\"failures\":%d,\"excluded\":%s,\"checked_status_available\":%s,\"historical_boundaries\":%d,\"skipped_builder_controls\":%d,\"unsafe_binding_terminated\":%s,\"saved_bytes\":%s}\n",
        checks,failures,excluded?"true":"false",HAVE_CHECKED_SAVESTATE?"true":"false",historical_boundaries,skipped_controls,terminated_unsafe_binding?"true":"false",sizes.c_str());
    return failures?1:0;
}
