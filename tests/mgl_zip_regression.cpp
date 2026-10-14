#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "cfg.h"
#include "miniz.h"
#ifndef __linux__
typedef off_t __off64_t;
#define BLKGETSIZE64 0x80081272UL
#endif
#include "mgl_zip_types.inc"

static char CoreName2[32] = "TestCore";
static char last_filename[1024];
static std::string storage_root;
const char *getRootDir() { return storage_root.c_str(); }
bool is_arcade() { return false; }
int FileClose(fileTYPE *file);
#include "mgl_zip_production.inc"

struct Call { std::string name, path; int index; };
static std::vector<Call> calls;
static int resolver_calls = 0;
MglZipFallbackResult FileResolveMglZipFallback(char *path, size_t size)
{
    ++resolver_calls;
    calls.push_back({"resolve", path ? path : "", 0});
    return production_FileResolveMglZipFallback(path, size);
}

enum { MENU_GENERIC_FILE_SELECTED, MENU_GENERIC_IMAGE_SELECTED, MENU_NONE1, MENU_GENERIC_MAIN1, MENU_ATARI8BIT_CART1 };
static int menustate, menusub;
static mgl_struct script;
static mgl_struct *mgl = &script;
cfg_t cfg;
static char selPath[1024], Selected_F[16][1024], Selected_S[16][1024];
static char SelectedDir[1024], SelectedLabel[256], addon[256], fs_pFileExt[256];
static int fs_Options, ioctl_index;
static bool store_name;
static char opensave;
static uint32_t load_addr, file_crc;
static std::string home, loaded_payload, load_path, mounted_path;
static char save_path[1024], state_path[1024];
static int tx_index, tx_calls, mount_calls;
static fileTYPE mounted[16];
const char *HomeDir(const char * = nullptr) { return home.c_str(); }
const char *user_io_get_core_name() { return CoreName2; }
void MenuHide() { calls.push_back({"hide", "", 0}); menustate = MENU_NONE1; }
void MakeFile(const char *name, const char *value) { calls.push_back({name, value, 0}); }
void recent_update(const char *, const char *name, const char *, int index) { calls.push_back({"recent", name, index}); }
int FileSaveConfig(const char *name, const void *value, int) { calls.push_back({name, (const char *)value, 0}); return 1; }
void process_addon(const char *, int index) { calls.push_back({"addon", "", index}); }
void game_docs_init(const char *name, uint32_t) { calls.push_back({"docs", name, 0}); }
void cheats_init(const char *name, uint32_t) { calls.push_back({"cheats", name, 0}); }
bool user_io_use_cheats() { return true; }
uint32_t user_io_get_file_crc() { return file_crc; }
int user_io_file_tx(const char *name, unsigned char index, char save, char mute, char, uint32_t)
{
    ++tx_calls; tx_index = index; load_path = name;
    calls.push_back({"tx", name, index});
    fileTYPE file;
    if (!FileOpen(&file, name, mute)) return 0;
    std::vector<unsigned char> bytes((size_t)file.size);
    if (!bytes.empty() && FileReadAdv(&file, bytes.data(), bytes.size(), -1) != (int)bytes.size()) return 0;
    loaded_payload.assign(bytes.begin(), bytes.end());
    file_crc = mz_crc32(0, bytes.data(), bytes.size());
    if (save) {
        FileGenerateSavePath(name, save_path, 1);
        FileGenerateSavestatePath(name, state_path, 1);
    }
    return 1;
}
int user_io_file_mount(const char *name, unsigned char index)
{
    ++mount_calls; mounted_path = name;
    return FileOpenEx(&mounted[index], name, FileCanWrite(name) ? O_RDWR : O_RDONLY, 1, 1);
}
void user_io_set_index(int index) { calls.push_back({"index", "", index}); }
void Info(const char *, int = 0) {}
bool is_pce() { return false; }
bool is_n64() { return false; }
bool is_c64() { return false; }
bool is_c128() { return false; }
bool is_atari800() { return false; }
bool is_atari5200() { return false; }
bool is_x86() { return false; }
bool is_pcxt() { return false; }
bool is_uneon() { return false; }
bool is_megacd() { return false; }
bool is_psx() { return false; }
bool is_cdi() { return false; }
bool is_saturn() { return false; }
bool is_neogeo() { return false; }
bool is_3do() { return false; }
bool is_marty() { return false; }
#define PCECD_DIR "PCECD"
#define MARTY_SLOT_CD 0
void neocd_set_en(int) {}
void neogeo_romset_tx(const char *, int) {}
void pcecd_set_image(int, const char *) {}
void pcecd_reset() {}
bool n64_rom_tx(const char *, unsigned char, uint32_t, uint32_t &) { return false; }
void c64_open_file(const char *, int) {}
void atari800_open_bios_file(const char *, int) {}
int atari5200_check_cartridge_file(const char *, int) { return 0; }
int atari800_check_cartridge_file(const char *, int) { return 0; }
void atari5200_open_cartridge_file(const char *, int) {}
void atari800_open_cartridge_file(const char *, int) {}
void atari800_umount_cartridge(bool) {}
void atari5200_umount_cartridge() {}
void x86_set_image(int, const char *) {}
void mcd_set_image(int, const char *) {}
void psx_mount_cd(int, int, const char *) {}
void cdi_mount_cd(int, const char *) {}
void saturn_set_image(int, const char *) {}
void saturn_mount_save(const char *) {}
void neocd_set_image(const char *) {}
void atari800_set_image(int, int, const char *) {}
void p3do_set_image(int, const char *) {}
void marty_set_image(int, const char *) {}
#include "mgl_zip_dispatch.inc"

static int failures = 0, checks = 0;
static void check(bool ok, const char *message)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
static std::string path(const char *name) { return storage_root + "/" + name; }
static int descriptor_count()
{
    int count = 0;
    for (int fd = 0; fd < 256; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}
static std::string resolve(const char *name, MglZipFallbackResult expected, const char *suffix = nullptr)
{
    struct Buffer { unsigned char before; char value[1024]; unsigned char after; } buffer;
    std::memset(&buffer, 0x55, sizeof(buffer));
    const std::string original = path(name);
    std::snprintf(buffer.value, sizeof(buffer.value), "%s", original.c_str());
    const int before = descriptor_count();
    MglZipFallbackResult result = FileResolveMglZipFallback(buffer.value, sizeof(buffer.value));
    check(result == expected, name);
    check(descriptor_count() == before, "resolver must close archive/preflight descriptors on every result");
    check(buffer.before == 0x55 && buffer.after == 0x55, "resolver must preserve destination guard bytes");
    if (expected != MGL_ZIP_RESOLVED) check(buffer.value == original, "unsuccessful resolution must preserve the literal path");
    else if (suffix) check(buffer.value == path(suffix), "resolver must use the exact full inner member name");
    return buffer.value;
}
static void helper_cases()
{
    resolve("Present.gba", MGL_ZIP_UNCHANGED);
    resolve("Missing.gba", MGL_ZIP_RESOLVED, "Missing.zip/Renamed.gba");
    resolve("Nested.gba", MGL_ZIP_RESOLVED, "Nested.zip/folder/deep/Inner.GBA");
    resolve("Mixed.gba", MGL_ZIP_RESOLVED, "Mixed.zip/Only.gba");
    resolve("Sonic.Advance (USA).gba", MGL_ZIP_RESOLVED, "Sonic.Advance (USA).zip/Old.Name.GBA");
    resolve("Multi.gba", MGL_ZIP_AMBIGUOUS); resolve("Duplicate.gba", MGL_ZIP_AMBIGUOUS);
    for (const char *name : {"Mismatch.gba", "DirDot.gba", "Absent.gba", "Empty.gba", "Dirs.gba"}) resolve(name, MGL_ZIP_NO_CANDIDATE);
    resolve("Malformed.gba", MGL_ZIP_INVALID_ARCHIVE);
    const std::string bad_local = path("BadLocal.zip/Inner.gba");
    check(FileExists(bad_local.c_str(), 1), "bad local header must retain readable supported member metadata");
    const int before_bad_open = descriptor_count();
    fileTYPE bad_file;
    check(!FileOpenEx(&bad_file, bad_local.c_str(), O_RDONLY, 1, 1), "actual backend must reject the corrupted local header at iterator creation");
    check(!bad_file.opened(), "failed member iterator must leave no open file state");
    check(descriptor_count() == before_bad_open, "failed member iterator must close its archive descriptor");
    resolve("BadLocal.gba", MGL_ZIP_INVALID_ARCHIVE);
    resolve("Unsupported.gba", MGL_ZIP_UNSUPPORTED); resolve("Encrypted.gba", MGL_ZIP_UNSUPPORTED);
    resolve("SupportedAndUnsupported.gba", MGL_ZIP_RESOLVED, "SupportedAndUnsupported.zip/Supported.gba");
    resolve("Nul.gba", MGL_ZIP_INVALID_ARCHIVE);
    for (const char *name : {"Missing.zip/Renamed.gba", "Missing.zip/Absent.gba", "Missing.zip", "Directory.gba", "NoExtension", "Name.", ".gba"}) resolve(name, MGL_ZIP_UNCHANGED);
    const std::string long_nested = resolve("LongNested.gba", MGL_ZIP_RESOLVED);
    check(long_nested.size() > 512 && long_nested.find("Renamed.gba") != std::string::npos, "full ZIP member names beyond miniz stat buffer must resolve");
    fileTYPE long_file;
    check(FileOpenEx(&long_file, long_nested.c_str(), O_RDONLY, 1, 1), "full nested resolved member must open in the actual backend");
    resolve("BaseFit.gba", MGL_ZIP_RESOLVED); resolve("BaseOver.gba", MGL_ZIP_PATH_TOO_LONG);
    const std::string fit = resolve("Fit.gba", MGL_ZIP_RESOLVED);
    check(fit.size() == 1023, "virtual path may fill destination through final NUL");
    resolve("Over.gba", MGL_ZIP_PATH_TOO_LONG);
    char small[8]; std::memset(small, 'X', sizeof(small));
    check(FileResolveMglZipFallback(nullptr, 10) == MGL_ZIP_INVALID_PATH, "null destination must be rejected");
    check(FileResolveMglZipFallback(small, 0) == MGL_ZIP_INVALID_PATH, "zero capacity must be rejected");
    check(FileResolveMglZipFallback(small, 5) == MGL_ZIP_INVALID_PATH, "unterminated input must be rejected before legacy path calls");
    check(std::all_of(small, small + sizeof(small), [](char c) { return c == 'X'; }), "invalid arguments must preserve the entire buffer");
    char relative[] = "Game.gba";
    check(FileResolveMglZipFallback(relative, sizeof(relative)) == MGL_ZIP_INVALID_PATH, "helper requires caller's absolute HomeDir-expanded path");
    std::vector<char> oversized(sizeof(full_path) + 20, 'x'); oversized[0] = '/'; oversized.back() = 0;
    const std::vector<char> original = oversized;
    check(FileResolveMglZipFallback(oversized.data(), oversized.size()) == MGL_ZIP_PATH_TOO_LONG,
          "larger callers must be checked against the existing loader path buffer before stat/open");
    check(oversized == original, "oversized input rejection must remain transactional");
    const std::string raw = path("RawDisk.img"), zip = path("RawDisk.zip/Renamed.img");
    fileTYPE disk;
    check(FileOpenEx(&disk, raw.c_str(), O_RDWR, 1, 1), "ordinary writable disk API remains supported");
    check(!FileOpenEx(&disk, zip.c_str(), O_RDWR, 1, 1), "ZIP member must not become writable");
    check(FileOpenEx(&disk, zip.c_str(), O_RDONLY, 1, 1), "explicit ZIP disk read behavior remains supported");
}
static void reset_menu(const char *name, int state = MENU_GENERIC_FILE_SELECTED)
{
    calls.clear(); resolver_calls = tx_calls = mount_calls = 0;
    loaded_payload.clear(); load_path.clear(); mounted_path.clear();
    std::memset(&script, 0, sizeof(script)); script.count = 1; script.state = 2;
    script.item[0].type = state == MENU_GENERIC_FILE_SELECTED ? 'F' : 'S';
    script.item[0].action = MGL_ACTION_LOAD;
    std::snprintf(script.item[0].path, sizeof(script.item[0].path), "%s", name);
    cfg = cfg_t{}; cfg.log_file_entry = 1;
    menustate = state; menusub = 0; fs_Options = SCANO_DIR; ioctl_index = 1;
    store_name = false; opensave = 1; load_addr = 0;
    home = storage_root;
    std::memset(selPath, 0, sizeof(selPath)); std::memset(Selected_F, 0, sizeof(Selected_F));
    std::memset(Selected_S, 0, sizeof(Selected_S)); std::memset(addon, 0, sizeof(addon));
    std::strcpy(fs_pFileExt, "GBAGB GBC");
    last_filename[0] = save_path[0] = state_path[0] = 0;
}
static void successful_menu(const char *requested, const char *expected, const char *payload)
{
    reset_menu(requested); selected_case();
    const std::string resolved = path(expected);
    check(loaded_payload == payload, "production MGL case must read the expected real raw/ZIP payload");
    check(load_path == resolved && std::string(Selected_F[1]) == resolved, "MGL transfer and selected state must use one resolved path");
    check(tx_index == 1 && script.state == 3, "matching extension must preserve index and existing MGL sequencing");
    check(!calls.empty() && calls[0].name == "resolve", "MGL resolution must precede logging/loading side effects");
    for (const char *name : {"/tmp/FULLPATH", "docs", "cheats"}) {
        const auto item = std::find_if(calls.begin(), calls.end(), [&](const Call &call) { return call.name == name; });
        check(item != calls.end() && item->path == resolved, "MGL log/docs/cheat callers must receive the actual loaded path");
    }
}
static void menu_cases()
{
    successful_menu("Present.gba", "Present.gba", "RAW_ORIGINAL");
    successful_menu("Missing.gba", "Missing.zip/Renamed.gba", "RENAMED_PAYLOAD");
    check(std::string(save_path) == "saves/TestCore/Renamed.sav" && std::string(state_path) == "savestates/TestCore/Renamed_1.ss"
          && std::string(last_filename) == "Renamed", "renamed ZIP fallback must retain existing member-based save/state/screenshot identity");
    successful_menu(path("Nested.gba").c_str(), "Nested.zip/folder/deep/Inner.GBA", "NESTED_PAYLOAD");
    reset_menu("Missing.gba"); store_name = true; selected_case();
    const auto config = std::find_if(calls.begin(), calls.end(), [](const Call &call) { return call.name == "TestCore.f1"; });
    check(config != calls.end() && config->path == path("Missing.zip/Renamed.gba"), "stored-name configuration must contain resolved virtual path");
    for (int option : {SCANO_NOZIP, SCANO_NEOGEO}) {
        reset_menu("Missing.gba"); fs_Options |= option; selected_case();
        check(!resolver_calls && std::string(selPath) == path("Missing.gba"), "NOZIP and directory ROM-set selection must bypass resolver");
    }
    reset_menu("Missing.gba"); script.done = 1; std::snprintf(selPath, sizeof(selPath), "%s", path("Missing.gba").c_str()); selected_case();
    check(!resolver_calls && loaded_payload.empty() && load_path == path("Missing.gba"), "manual non-MGL selection must retain missing literal path behavior");
    reset_menu("MissingDisk.img", MENU_GENERIC_IMAGE_SELECTED); selected_case();
    check(!resolver_calls && mounted_path == path("MissingDisk.img") && !mounted[1].opened(), "MGL S image path must never use ROM ZIP fallback");
    reset_menu("Missing.gba"); home.assign(1018, 'x'); home[0] = '/'; selected_case();
    check(script.state == 3 && menustate == MENU_NONE1 && !resolver_calls && !tx_calls && calls.empty(),
          "truncated HomeDir+item path must skip before every file-load side effect");
    reset_menu("Missing.gba"); script.item[0].action = MGL_ACTION_RESET; selected_case();
    check(!resolver_calls, "non-load action must not invoke ROM fallback");
    reset_menu("Missing.gba"); script.item[0].type = 'S'; selected_case();
    check(!resolver_calls, "non-F action must not invoke ROM fallback from a stale selected state");
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    storage_root = argv[1];
    helper_cases(); menu_cases();
    for (auto &file : mounted) FileClose(&file);
    mz_zip_reader_end(&last_zip_archive);
    if (last_zip_cfile) std::fclose(last_zip_cfile);
    std::printf("%s: production MGL ZIP fallback/file backend/selected cases (%d checks, %d failures; helper=%d)\n",
                failures ? "FAIL" : "PASS", checks, failures, HAVE_MGL_ZIP_HELPER);
    return failures ? 1 : 0;
}
