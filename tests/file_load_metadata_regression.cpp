// Included after the common MGL file/backend fixture declarations. All file,
// transfer and metadata functions in the generated includes are production.
static bool forced_gba = false, truncate_on_download = false;
static int special_core = 0, use_cheats = 1, transfer_return = -1;
static uint32_t ss_base = 1;
static int spi_index = 0, state_calls = 0, gameid_calls = 0, data_calls = 0;
static int cheat_reset_calls = 0, download_calls = 0, doc_calls = 0, cheat_calls = 0;
static int special_calls = 0, pce_resets = 0, pce_images = 0, addon_calls = 0;
static int last_read = -99, mount_result = -1, last_mount_index = -1;
static bool n64_accept = false, last_mount_writable = false;
static uint32_t last_doc_crc = 0, last_cheat_crc = 0;
static std::string gameid_name, state_name, truncate_path, last_doc_name, last_cheat_name;
static std::string special_screenshot, addon_screenshot, pce_screenshot, tx_entry_screenshot;
static std::vector<unsigned char> transfer_bytes, read_before, read_after;

bool is_snes() { return false; }
bool is_gba() { return forced_gba; }
bool is_sgb() { return false; }
bool is_electron() { return false; }
bool is_zx81() { return false; }
bool is_pce() { return special_core == 6; }
bool is_n64() { return special_core == 1; }
bool is_c64() { return special_core == 2; }
bool is_c128() { return special_core == 3; }
bool is_atari800() { return special_core == 4; }
bool is_atari5200() { return special_core == 5; }
bool pcecd_using_cd() { return false; }
const char *psx_get_game_id() { return nullptr; }
const char *user_io_get_core_name2() { return CoreName2; }
int findDocsDir(char *directory, size_t capacity)
{ std::snprintf(directory, capacity, "%s/docs/%s", storage_root.c_str(), CoreName2); return 1; }
int OsdGetSize() { return 16; }
void user_io_set_index(int index) { spi_index = index; calls.push_back({"index", "", index}); }
void user_io_set_download(unsigned char enable, int = 0)
{
    download_calls++;
    if (enable && truncate_on_download && spi_index != 255) {
        FILE *f = std::fopen(truncate_path.c_str(), "wb");
        if (!f) std::abort();
        std::fclose(f);
        truncate_on_download = false;
    }
}
void user_io_file_tx_data(const uint8_t *data, uint32_t count)
{
    if (spi_index == 255) { cheat_reset_calls++; return; }
    data_calls++;
    transfer_bytes.insert(transfer_bytes.end(), data, data + count);
}
void user_io_file_info(const char *) {}
void ProgressMessage(const char *, const char *, int, int) {}
void check_status_change() {}
void user_io_write_gameid(const char *name, uint32_t)
{ gameid_calls++; gameid_name = name; }
int process_ss(const char *name, int = 1)
{ state_calls++; state_name = name; FileGenerateSavestatePath(name, state_path, 1); return 1; }
int user_io_file_mount(const char *name, unsigned char index, char pre, int)
{
    mount_calls++; mounted_path = name; last_mount_index = index;
    if (pre) return 1; // Save mount is a peripheral; no save file is created.
    last_mount_writable = FileCanWrite(name);
    mount_result = FileOpenEx(&mounted[index], name, last_mount_writable ? O_RDWR : O_RDONLY, 1, 1);
    return mount_result;
}
void *shmem_map(uint32_t, uint32_t) { return nullptr; }
void shmem_unmap(void *, uint32_t) {}
uint32_t fpga_mem(uint32_t address) { return address; }
char *user_io_make_filepath(const char *directory, const char *name)
{ static char output[2100]; std::snprintf(output, sizeof(output), "%s/%s", directory, name); return output; }
uint8_t *snes_get_header(fileTYPE *) { static uint8_t bytes[512]; return bytes; }
uint8_t *snes_get_mirrored_rom(fileTYPE *, uint32_t *) { return nullptr; }
void snes_patch_bs_header(fileTYPE *, uint8_t *) {}
void snes_msu_init(const char *) {}
void mdplus_init(const char *) {}
void UEF_FileSend(fileTYPE *, int) {}
void send_pcolchr(const char *, unsigned char, int) {}
void hexdump(const void *, int, int) {}
int trace_FileReadAdv(fileTYPE *file, void *data, int count, int failres = 0)
{
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    read_before.assign(bytes, bytes + count);
    last_read = FileReadAdv(file, data, count, failres);
    read_after.assign(bytes, bytes + count);
    return last_read;
}
int production_user_io_file_tx(const char *, unsigned char, char, char, char, uint32_t);
void production_game_docs_init(const char *, uint32_t);
void production_cheats_init(const char *, uint32_t);
int user_io_file_tx(const char *name, unsigned char index, char save, char mute, char composite, uint32_t address)
{
    tx_calls++; tx_index = index; load_path = name;
    calls.push_back({"tx", name, index}); tx_entry_screenshot = last_filename;
    transfer_return = production_user_io_file_tx(name, index, save, mute, composite, address);
    return transfer_return;
}
void game_docs_init(const char *name, uint32_t crc)
{ doc_calls++; last_doc_name = name; last_doc_crc = crc; production_game_docs_init(name, crc); }
void cheats_init(const char *name, uint32_t crc)
{ cheat_calls++; last_cheat_name = name; last_cheat_crc = crc; production_cheats_init(name, crc); }
void special_entry()
{ special_calls++; special_screenshot = last_filename; }
bool n64_rom_tx(const char *, unsigned char, uint32_t, uint32_t &crc)
{ special_entry(); crc = 0x11223344; return n64_accept; }
void c64_open_file(const char *, int) { special_entry(); }
void atari800_open_bios_file(const char *, int) { special_entry(); }
int atari5200_check_cartridge_file(const char *, int) { special_entry(); return 1; }
int atari800_check_cartridge_file(const char *, int) { special_entry(); return 1; }
void atari5200_open_cartridge_file(const char *, int) { special_entry(); }
void atari800_open_cartridge_file(const char *, int) { special_entry(); }
void pcecd_set_image(int, const char *) { pce_images++; pce_screenshot = last_filename; }
void pcecd_reset() { pce_resets++; pce_screenshot = last_filename; }
void process_addon(const char *, int index)
{ addon_calls++; addon_screenshot = last_filename; calls.push_back({"addon", "", index}); }

#include "file_load_transfer.inc"
#include "mgl_zip_dispatch.inc"

static int failures = 0, checks = 0;
static std::string case_context;
static void check(bool ok, const char *message)
{ checks++; if (!ok) { failures++; std::fprintf(stderr, "FAIL: [%s] %s\n", case_context.c_str(), message); } }
static std::string path(const char *name) { return storage_root + "/" + name; }
static void prepare(const char *name, bool manual = true, bool stored = false)
{
    case_context = std::string(name) + (manual ? "/manual" : "/MGL") + (stored ? "/store_name" : "");
    std::memset(&script, 0, sizeof(script)); script.count = 1; script.state = 2; script.done = manual;
    script.item[0].type = 'F'; script.item[0].action = MGL_ACTION_LOAD;
    std::snprintf(script.item[0].path, sizeof(script.item[0].path), "%s", name);
    std::snprintf(selPath, sizeof(selPath), "%s", name[0] ? path(name).c_str() : "");
    cfg = cfg_t{}; cfg.log_file_entry = 1;
    menustate = MENU_GENERIC_FILE_SELECTED; fs_Options = SCANO_DIR; ioctl_index = 1;
    store_name = stored; opensave = 1; load_addr = 0; addon[0] = 0;
    special_core = 0; forced_gba = false; use_cheats = 1;
    std::strcpy(fs_pFileExt, "GBAGB GBC");
}
static std::string cheat_entries()
{
    std::string result;
    for (const auto &cheat : cheats) result += std::string(cheat.name) + (cheat.enabled ? ":enabled;" : ":disabled;");
    return result;
}
struct Snapshot {
    std::string screenshot, manual, archive, entries;
    int loaded_count, docs, cheats, resets, mounts, states, gameids, data, downloads;
    uint32_t crc;
    size_t bytes;
};
static Snapshot snapshot()
{
    return {last_filename, game_docs_get_manual(), cheat_zip, cheat_entries(), loaded, doc_calls, cheat_calls,
            cheat_reset_calls, mount_calls, state_calls, gameid_calls, data_calls, download_calls,
            file_crc, transfer_bytes.size()};
}
static void retained_metadata(const Snapshot &old)
{
    check(last_filename == old.screenshot, "rejected generic load must retain the active screenshot basename");
    check(game_docs_get_manual() == old.manual, "rejected generic load must retain the active actual manual");
    check(cheat_zip == old.archive && cheat_entries() == old.entries && loaded == old.loaded_count,
          "rejected generic load must retain the active actual cheat archive, entries and enabled/loaded state");
    check(doc_calls == old.docs && cheat_calls == old.cheats,
          "rejected generic load must not reinitialize docs or search assets using stale CRC");
    check(cheat_reset_calls == old.resets && download_calls == old.downloads,
          "rejected generic load must not issue a cheat reset or begin download");
    check(file_crc == old.crc && mount_calls == old.mounts && state_calls == old.states && gameid_calls == old.gameids,
          "actual early rejection must retain previous CRC, save mount, savestate and GAMEID setup");
    check(data_calls == old.data && transfer_bytes.size() == old.bytes,
          "actual early rejection must send no new ROM data");
}
static void seed()
{
    prepare("Old.gba"); selected_case();
    check(transfer_return == 1 && file_crc == 0xFD7BB204 && !cheats.empty(), "seed must execute actual accepted ROM and cheat ZIP load");
    check(std::string(game_docs_get_manual()) == path("Old.pdf"), "seed must execute actual same-name manual lookup");
    if (!cheats.empty()) cheats[0].enabled = true;
    loaded = 1; // Existing active cheats fixture; resets and parsing are real.
}
static void rejected(const char *name, bool manual, int options = SCANO_DIR)
{
    seed(); const Snapshot old = snapshot();
    prepare(name, manual); fs_Options = options;
    const size_t call_start = calls.size();
    selected_case();
    check(transfer_return == 0, "rejection fixture must reach the actual file-open zero return");
    retained_metadata(old);
    check(std::string(Selected_F[1]) == path(name) && script.state == 3 && menustate == MENU_NONE1,
          "rejected selection must retain attempted path and normal MGL/UI progress");
    bool recent = false, logged = false;
    for (size_t i = call_start; i < calls.size(); i++) {
        const auto &call = calls[i];
        if (call.name == "recent" && call.path == path(name)) recent = true;
        if (call.name == "/tmp/FULLPATH" && call.path == path(name)) logged = true;
    }
    check(manual ? recent : logged, "rejected manual recent or MGL log must retain the attempted selection");
}
static void accepted(const char *name, const char *basename, bool manual, uint32_t crc)
{
    seed(); const Snapshot old = snapshot();
    prepare(name, manual); selected_case();
    check(transfer_return == 1 && file_crc == crc, "accepted fixture must execute actual transfer and expected CRC");
    check(last_filename == std::string(basename), "accepted generic load must commit the resolved screenshot basename");
    check(doc_calls == old.docs + 1 && cheat_calls == old.cheats + 1 && cheat_reset_calls == old.resets + 1,
          "accepted generic load must initialize actual docs and cheats once");
    check(last_doc_name == selPath && last_cheat_name == selPath && last_doc_crc == crc && last_cheat_crc == crc,
          "accepted metadata must use actual selected/resolved member path and its CRC");
    check(mount_calls == old.mounts + 1 && state_calls == old.states + 1 && gameid_calls == old.gameids + 1,
          "accepted transfer must retain save, state and GAMEID setup");
    check(script.state == 3 && std::string(Selected_F[1]) == selPath, "accepted selection must retain normal state progress");
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    storage_root = argv[1]; home = storage_root;
    rejected("Missing.gba", true);
    rejected("Missing.gba", false);
    rejected("MissingNoAssets.gba", false);
    rejected("NoSuchFile.gba", false);
    rejected("ZipGame.zip/nested/Absent.gba", true);
    rejected("ZipGame.zip/nested/Absent.gba", false);
    rejected("Invalid.zip/Entry.gba", true);
    rejected("Unsupported.gba", false);
    rejected("ZipGame.gba", false, SCANO_DIR | SCANO_NOZIP);

    unsigned char expected[4096] = {};
    check(FileLoad(path("New.gba").c_str(), expected, sizeof(expected)) == sizeof(expected), "accepted fixture bytes must come from actual FileLoad");
    const uint32_t actual_new_crc = mz_crc32(0, expected, sizeof(expected));
    accepted("New.gba", "New", true, actual_new_crc);
    check(std::string(game_docs_get_manual()) == path("New.pdf") && std::string(cheat_zip) == path("New.zip"),
          "accepted ordinary file must replace active assets by actual same-name lookup");
    accepted("ZipGame.zip/nested/Renamed.gba", "Renamed", true, actual_new_crc);
    accepted("ZipGame.gba", "Renamed", false, actual_new_crc);
    check(std::string(selPath) == path("ZipGame.zip/nested/Renamed.gba") && tx_index == 1,
          "MGL accepted fallback must retain renamed compatible-member identity and extension index");
    check(std::string(game_docs_get_manual()).find("Zip Manual [") != std::string::npos
          && std::string(cheat_zip).find("Zip Cheats [") != std::string::npos,
          "accepted ZIP metadata must use actual new-CRC asset lookup");
    accepted("Original.gba", "Original", false, 0xFD7BB204);
    check(std::string(selPath) == path("Original.gba"), "present original file must retain priority over ZIP alternative");

    seed(); Snapshot old = snapshot();
    prepare("Missing.gba", true, true); const size_t config_start = calls.size(); selected_case();
    check(transfer_return == 0, "stored-name missing file still uses actual rejected transfer");
    retained_metadata(old);
    bool stored = false;
    for (size_t i = config_start; i < calls.size(); i++)
        if (calls[i].name == "TestCore.f1" && calls[i].path == path("Missing.gba")) stored = true;
    check(stored, "stored-name failed attempt must retain existing config recording policy");
    seed(); old = snapshot(); prepare("New.gba", true, true); selected_case();
    check(transfer_return == 1 && file_crc == actual_new_crc && last_filename == old.screenshot
          && game_docs_get_manual() == old.manual && cheat_entries() == old.entries && loaded == old.loaded_count,
          "stored-name accepted transfer must retain existing metadata suppression and positive CRC behavior");

    seed(); old = snapshot(); prepare("New.gba"); use_cheats = 0; opensave = 0; selected_case();
    check(transfer_return == 1 && last_filename == std::string("New") && doc_calls == old.docs + 1,
          "cheats-disabled accepted transfer must still commit screenshot and docs");
    check(cheat_calls == old.cheats && cheat_reset_calls == old.resets && loaded == old.loaded_count,
          "cheats-disabled transfer must retain active cheats without initialization/reset");
    check(mount_calls == old.mounts && state_calls == old.states, "opensave zero must retain absence of save/state setup");

    seed(); old = snapshot(); prepare("Empty.gba"); selected_case();
    check(transfer_return == 1 && !file_crc && last_filename == std::string("Empty") && doc_calls == old.docs + 1,
          "valid empty file remains accepted and commits metadata under existing API semantics");
    check(transfer_bytes.size() == old.bytes, "valid empty file must send no ROM payload");
    fileTYPE empty;
    check(FileOpen(&empty, path("Empty.gba").c_str(), 1) && FileLoad(path("Empty.gba").c_str(), nullptr, 0) == 0
          && FileLoad(path("NoSuchFile.gba").c_str(), nullptr, 0) == 0,
          "FileLoad byte-count zero must remain shared by valid empty and missing files");
    FileClose(&empty);

    // Positive completion does not guarantee successful hardware/read delivery.
    // These controls deliberately retain the existing positive API policy.
    seed(); old = snapshot(); prepare("MapFailure.gba"); load_addr = 0x20000000; selected_case();
    check(transfer_return == 1 && file_crc == 0 && transfer_bytes.size() == old.bytes,
          "unavailable mapped-memory peripheral retains existing positive completion without data");
    check(last_filename == std::string("MapFailure") && doc_calls == old.docs + 1,
          "late mapped-memory failure remains outside initial-open rejection guard");
    seed(); old = snapshot(); prepare("GoombaTest.gb"); forced_gba = true; selected_case(); forced_gba = false;
    check(transfer_return == 1 && file_crc == 0 && transfer_bytes.size() == old.bytes,
          "actual missing goomba prerequisite retains existing positive completion without data");
    check(last_filename == std::string("GoombaTest") && doc_calls == old.docs + 1,
          "late prerequisite failure remains outside initial-open rejection guard");
    seed(); old = snapshot(); prepare("Truncated.gba"); truncate_path = path("Truncated.gba"); truncate_on_download = true;
    selected_case();
    bool unchanged = last_read == 0 && read_before.size() == 4096 && read_before == read_after && transfer_bytes.size() == old.bytes + 4096;
    for (size_t i = 0; unchanged && i < 4096; i++) unchanged = transfer_bytes[old.bytes + i] == read_before[i];
    check(transfer_return == 1 && unchanged, "actual zero read after open retains existing positive completion and buffer send");
    check(last_filename == std::string("Truncated") && doc_calls == old.docs + 1,
          "late zero-read handling remains outside initial-open rejection guard");

    // Specialized API contracts/timing remain outside this generic caller fix.
    for (int core = 1; core <= 5; core++) {
        seed(); old = snapshot(); const int old_tx = tx_calls, old_special = special_calls;
        prepare("Special.gba"); special_core = core; n64_accept = false; selected_case();
        check(tx_calls == old_tx && special_calls > old_special, "specialized branch must retain its own loader dispatch");
        check(special_screenshot == "Special" && last_filename == std::string("Special"),
              "specialized branch must retain screenshot store before its loader/checker");
        check(doc_calls == old.docs && cheat_calls == old.cheats, "specialized failure/checker controls must retain existing metadata policy");
    }
    seed(); old = snapshot(); prepare("Special.gba"); special_core = 1; n64_accept = true; selected_case();
    check(special_screenshot == "Special" && doc_calls == old.docs + 1 && last_doc_crc == 0x11223344
          && cheat_calls == old.cheats + 1 && last_cheat_crc == 0x11223344,
          "accepted N64 peripheral must retain its existing dedicated CRC metadata contract");

    seed(); old = snapshot(); prepare("Missing.gba"); special_core = 6;
    const int images = pce_images, resets = pce_resets; selected_case();
    check(pce_images == images + 1 && pce_resets == resets + 1 && pce_screenshot == "Old",
          "PCE preprocessing must retain order and run even before rejected generic open");
    retained_metadata(old);
    for (const char *add : {"f0", "f1"}) {
        seed(); old = snapshot(); prepare("Missing.gba"); std::strcpy(addon, add);
        const int count = addon_calls; selected_case();
        check(addon_calls == count + 1 && addon_screenshot == "Old", "addon dispatch must retain execution/order while rejected metadata remains active");
        retained_metadata(old);
    }

    seed(); old = snapshot(); prepare("Disk.img", false); script.item[0].type = 'S';
    menustate = MENU_GENERIC_IMAGE_SELECTED; ioctl_index = 2; std::strcpy(fs_pFileExt, "IMG");
    const int resolvers = resolver_calls, old_tx = tx_calls; selected_case();
    check(resolver_calls == resolvers && tx_calls == old_tx && mount_result == 1 && last_mount_index == 2 && last_mount_writable,
          "S writable mount must retain actual backend open and exclude F resolver/transfer");
    check(last_filename == old.screenshot && game_docs_get_manual() == old.manual && loaded == old.loaded_count,
          "ordinary S writable mount must retain existing metadata policy");
    check(script.state == 3 && std::string(Selected_S[2]) == path("Disk.img"), "S writable mount retains selected path/progress");
    FileClose(&mounted[2]);
    prepare("ZipDisk.img", false); script.item[0].type = 'S';
    menustate = MENU_GENERIC_IMAGE_SELECTED; ioctl_index = 2; std::strcpy(fs_pFileExt, "IMG"); selected_case();
    check(mount_result == 0 && resolver_calls == resolvers && std::string(Selected_S[2]) == path("ZipDisk.img"),
          "missing S disk must retain literal failure despite compatible ZIP sibling");

    seed(); old = snapshot(); prepare(""); const int empty_tx = tx_calls; selected_case();
    check(tx_calls == empty_tx && last_filename == old.screenshot && script.state == 3,
          "empty selected F path must retain bypass/progress and active metadata");
    seed(); old = snapshot(); prepare("Missing.gba"); fs_Options |= SCANO_NEOGEO; const int neo_tx = tx_calls; selected_case();
    check(tx_calls == neo_tx && last_filename == old.screenshot && doc_calls == old.docs && cheat_calls == old.cheats,
          "NeoGeo ROMset dispatch must remain outside generic filename/metadata transaction");

    mz_zip_reader_end(&last_zip_archive);
    if (last_zip_cfile) { std::fclose(last_zip_cfile); last_zip_cfile = nullptr; }
    std::printf("JSON {\"checks\":%d,\"failures\":%d}\n", checks, failures);
    std::printf("%s: production generic file-load metadata (%d checks)\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
