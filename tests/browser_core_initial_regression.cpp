#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>
#include <stdexcept>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>
#if defined(__MSYS__)
// Host ABI adapter only: this fixture supplies fitting names and no inode fields.
using fixture_dirent_entry = dirent;
static const char *fixture_strcasestr(const char *haystack, const char *needle)
{
    size_t length = std::strlen(needle);
    if (!length) return haystack;
    for (const char *p = haystack; *p; ++p) if (!strncasecmp(p, needle, length)) return p;
    return nullptr;
}
#else
using fixture_dirent_entry = dirent64;
#endif
#include "browser_core_initial_cfg.h"
#include "browser_core_initial_types.inc"

cfg_t cfg = {};
static std::vector<direntext_t> DirItem;
static std::set<std::string> DirNames;
static int iSelectedEntry = 0, iFirstEntry = 0, names_loaded = 0;
static char scanned_path[1024] = {};
static int scanned_opts = 0;
static char selPath[1024] = "_Arcade/_alternatives";
static char fs_pFileExt[16] = "RBFMRAMGL";
static int fs_Options = SCANO_CORES;
static char filter[256] = {};
static unsigned long filter_typing_timer = 0;
static const char *home_dir = nullptr;
static int menustate = 0;
static constexpr int MENU_FILE_SELECT1 = 77;
static constexpr uint32_t UPSTROKE = 0x400000;
static unsigned long now = 10000;
static std::vector<std::string> queries;
static std::vector<fixture_dirent_entry> fixture;
static size_t fixture_cursor = 0;
static fixture_dirent_entry returned_entry = {};
static bool archive_mode = false;
static std::vector<std::string> archive_members;
static std::vector<std::string> rows;
static std::vector<int> highlights;
static std::string scroll_name;
static int scroll_len = -1, scroll_max = -1;
static int osd_rows = 16;
static int checks = 0, failures = 0, case_checks = 0, case_failures = 0;
static int center_calls = 0;
static std::vector<std::string> failure_messages, setup_queries;
static std::string names_fixture;
static std::vector<std::string> observations;

static void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}
static void check(bool condition, const std::string &message)
{
    ++checks; ++case_checks;
    if (!condition) { ++failures; ++case_failures; failure_messages.push_back(message); std::fprintf(stderr, "FAIL: %s\n", message.c_str()); }
}
static std::string quote(const std::string &s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32 || c >= 127)
        {
            char hex[7]; std::snprintf(hex, sizeof(hex), "\\u%04x", c); out += hex;
        }
        else out += char(c);
    }
    return out + "\"";
}
static std::string array(const std::vector<std::string> &values)
{
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) { if (i) out += ','; out += quote(values[i]); }
    return out + ']';
}
int OsdGetSize() { return osd_rows; }
void ScrollReset() { rows.clear(); highlights.clear(); }
void OsdWriteOffset(unsigned char row, const char *text, unsigned char selected,
    unsigned char, char, char, char = 0, int = 32, int = 0)
{
    require(row == rows.size(), "renderer row order"); rows.emplace_back(text);
    if (selected) highlights.push_back(row);
}
void ScrollText(int, const char *text, int, int len, int maximum, int)
{
    scroll_name = text; scroll_len = len; scroll_max = maximum;
}
unsigned long GetTimer(unsigned long interval) { queries.push_back("GetTimer:" + std::to_string(interval)); return now + interval; }
int CheckTimer(unsigned long deadline)
{
    queries.push_back("CheckTimer:" + std::to_string(deadline) + ':' + std::to_string(now));
    return now >= deadline;
}
int get_amiga_code(int code) { queries.push_back("get_amiga_code:" + std::to_string(code)); return code; }
static int fixture_is_regular(const char *path) { queries.push_back("isPathRegularFile:" + std::string(path)); return 0; }
static int fixture_is_directory(const char *path) { queries.push_back("isPathDirectory:" + std::string(path)); return 1; }
static const char *fixture_root() { queries.push_back("getRootDir"); return "/media/fat"; }
static DIR *fixture_opendir(const char *path)
{
    queries.push_back("opendir:" + std::string(path)); fixture_cursor = 0;
    return reinterpret_cast<DIR*>(uintptr_t(1));
}
static fixture_dirent_entry *fixture_readdir(DIR *dir)
{
    require(dir == reinterpret_cast<DIR*>(uintptr_t(1)), "finite DIR handle");
    queries.push_back("readdir64:" + std::to_string(fixture_cursor));
    if (fixture_cursor >= fixture.size()) return nullptr;
    // A fresh lower directory read must not inherit ScanDirectory's prior local ZIP type mutation.
    returned_entry = fixture[fixture_cursor++];
    return &returned_entry;
}
static int fixture_closedir(DIR *dir)
{
    require(dir == reinterpret_cast<DIR*>(uintptr_t(1)), "finite close handle"); queries.push_back("closedir"); return 0;
}
static int fixture_stat(const char *path, struct stat *out)
{
    queries.push_back("stat:" + std::string(path)); std::memset(out, 0, sizeof(*out)); out->st_mode = S_IFREG; return 0;
}
int FileLoad(const char *name, void *out, int)
{
    require(std::string(name) == "names.txt", "only declared names.txt lower file");
    queries.push_back("FileLoad:" + std::string(name));
    if (out) std::memcpy(out, names_fixture.data(), names_fixture.size());
    return static_cast<int>(names_fixture.size());
}
int is_minimig() { return 0; }
int x2trd_ext_supp(const char *) { return 0; }
void neogeo_scan_xml(const char *) { throw std::runtime_error("unselected NeoGeo scan"); }
char *neogeo_get_altname(const char *, const char *, const char *) { throw std::runtime_error("unselected NeoGeo altname"); }
struct fixture_zip_archive {};
static fixture_zip_archive last_zip_archive;
int FileIsZipped(char *, char **zip, char **member)
{
    queries.push_back("FileIsZipped"); if (zip) *zip = const_cast<char*>("fixture.zip");
    if (member) *member = const_cast<char*>("");
    return archive_mode;
}
int OpenZipfileCached(char *path, int)
{
    require(archive_mode, "declared archive only"); queries.push_back("OpenZipfileCached:" + std::string(path)); return 1;
}
const char *mz_zip_get_error_string(int) { return "finite archive"; }
int mz_zip_get_last_error(fixture_zip_archive *) { return 0; }
size_t mz_zip_reader_get_num_files(fixture_zip_archive *) { return archive_members.size(); }
void mz_zip_reader_get_filename(fixture_zip_archive *, size_t index, char *out, size_t capacity)
{
    require(index < archive_members.size() && archive_members[index].size() < capacity, "bounded declared archive member");
    queries.push_back("zip-filename:" + archive_members[index]); std::strcpy(out, archive_members[index].c_str());
}
int mz_zip_reader_is_file_a_directory(fixture_zip_archive *, size_t index)
{
    return !archive_members[index].empty() && archive_members[index].back() == '/';
}
int flist_nDirEntries();
int ScanDirectory(char*, int, const char*, int, const char* = nullptr, const char* = nullptr);
#define isPathRegularFile fixture_is_regular
#define isPathDirectory fixture_is_directory
#define getRootDir fixture_root
#define opendir fixture_opendir
#define readdir64 fixture_readdir
#define closedir fixture_closedir
#define stat(...) fixture_stat(__VA_ARGS__)
#define mz_zip_archive fixture_zip_archive
#if defined(__MSYS__)
#define dirent64 dirent
#define strcasestr fixture_strcasestr
#endif
#include "browser_core_initial_production.inc"
#if defined(__MSYS__)
#undef strcasestr
#undef dirent64
#endif
#undef mz_zip_archive
#undef isPathRegularFile
#undef isPathDirectory
#undef getRootDir
#undef opendir
#undef readdir64
#undef closedir
#undef stat

using Entries = std::vector<std::pair<std::string, int>>;

static std::string hex(const char *text)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char *p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
    {
        out += digits[*p >> 4]; out += digits[*p & 15];
    }
    return out;
}
static std::string list_signature()
{
    std::string out;
    for (const auto &item : DirItem)
        out += hex(item.de.d_name) + '/' + hex(item.altname) + '/' + hex(item.datecode)
            + '/' + std::to_string(item.de.d_type) + '/' + std::to_string(item.flags)
            + '/' + std::to_string(item.cookie) + ';';
    return out;
}
static void reset(const Entries &entries, int options,
    const char *extensions = "RBFMRAMGL", bool zip = false,
    const std::string &name_map = "", int hide_date = 1, int expand = 0, int lookahead = 0)
{
    fixture.clear(); fixture_cursor = 0; archive_mode = zip; archive_members.clear();
    for (const auto &entry : entries)
    {
        require(!entry.first.empty() && entry.first.size() < 256 && (entry.second == DT_DIR || entry.first.size() >= 4), "fitting finite dirent name");
        fixture_dirent_entry de = {}; std::strcpy(de.d_name, entry.first.c_str()); de.d_type = entry.second; fixture.push_back(de);
        if (zip) archive_members.push_back(entry.first + (entry.second == DT_DIR ? "/nested.rbf" : ""));
    }
    std::strcpy(selPath, zip ? "_Arcade/fixture.zip" : "_Arcade/_alternatives");
    std::strcpy(fs_pFileExt, extensions); fs_Options = options;
    filter[0] = 0; filter_typing_timer = 0; names_loaded = 0; names_fixture = name_map; now = 10000;
    DirItem.clear(); DirNames.clear(); iSelectedEntry = 0; iFirstEntry = 0; queries.clear(); setup_queries.clear(); menustate = 0; center_calls = 0;
    cfg = {}; cfg.rbf_hide_datecode = hide_date; cfg.browse_expand = expand; cfg.lookahead = lookahead;
    int result = ScanDirectory(selPath, SCANF_INIT, fs_pFileExt, fs_Options);
    check(result == int(DirItem.size()), "actual init return count");
    require(result > 0, "declared nonempty admitted fixture");
}
static void type(uint32_t amiga_fixture_code, unsigned long advance = 0)
{
    now += advance; typed_event(amiga_fixture_code);
    check(menustate == MENU_FILE_SELECT1, "typed input redraw state");
}
static void navigate(int mode)
{
    const std::string before = list_signature();
    const cfg_t config_before = cfg;
    const std::string path_before = selPath, scanned_before = scanned_path;
    const int options_before = scanned_opts;
    const size_t queries_before = queries.size();
    int result = ScanDirectory(selPath, mode, fs_pFileExt, fs_Options);
    check(result == 0, "actual non-init public return");
    check(queries.size() == queries_before + 2, "non-init search/navigation has no lower I/O beyond passive begin/end observations");
    check(list_signature() == before, "complete admitted name/altname/date/type/flags/cookie records unchanged by non-init call");
    check(std::memcmp(&cfg, &config_before, sizeof(cfg)) == 0, "non-init cfg bytes unchanged");
    check(std::string(selPath) == path_before && std::string(scanned_path) == scanned_before && scanned_opts == options_before, "non-init paths and scanned option state unchanged");
}
static void position(const std::string &name)
{
    const std::string before = list_signature();
    auto wanted = std::find_if(DirItem.begin(), DirItem.end(), [&name](const direntext_t &item) { return name == item.de.d_name; });
    require(wanted != DirItem.end(), "declared setup item admitted by actual INIT");
    int result = 0;
    if (wanted->de.d_type == DT_DIR) result = ScanDirectory(selPath, SCANF_SET_ITEM, name.c_str(), fs_Options);
    else
    {
        size_t steps = 0;
        while (std::string(DirItem[iSelectedEntry].de.d_name) != name && steps++ < DirItem.size())
            result = ScanDirectory(selPath, SCANF_NEXT, fs_pFileExt, fs_Options);
    }
    check(result == 0, "actual SET_ITEM public return");
    require(std::string(DirItem[iSelectedEntry].de.d_name) == name, "declared setup item selected by actual SET_ITEM");
    check(list_signature() == before, "SET_ITEM retains admitted complete records");
    setup_queries = queries; queries.clear(); center_calls = 0;
}
static void observe(const std::string &name, const std::string &desired, int desired_first = 0,
    int desired_centers = 1, bool expanded_selection = false)
{
    require(iSelectedEntry >= 0 && iSelectedEntry < int(DirItem.size()), "actual observed selection is within admitted finite list");
    int desired_index = -1;
    for (size_t i = 0; i < DirItem.size(); ++i) if (desired == DirItem[i].de.d_name) desired_index = int(i);
    check(desired_index >= 0, name + ": desired admitted item exists");
    PrintDirectory(1); scroll_name.clear(); scroll_len = scroll_max = -1; ScrollLongName();
    const auto &selected = DirItem[iSelectedEntry];
    const bool matched = desired == selected.de.d_name;
    check(matched, name + ": desired selected complete raw name");
    check(iSelectedEntry == desired_index, name + ": desired selected index");
    check(iFirstEntry == desired_first, name + ": exact first-visible index");
    check(center_calls == desired_centers, name + ": exact production centering invocation count");
    std::vector<int> expected_highlights{desired_index - desired_first};
    if (expanded_selection) expected_highlights.push_back(desired_index - desired_first + 1);
    check(highlights == expected_highlights, name + ": exact desired highlighted renderer row(s)");
    check(rows.size() == size_t(osd_rows), name + ": full renderer row count");
    check(iFirstEntry >= 0 && iFirstEntry <= flist_last_first_entry(), name + ": viewport within admitted list");
    if (desired_index >= 0)
    {
        const auto &wanted = DirItem[size_t(desired_index)];
        const bool directory = wanted.de.d_type == DT_DIR;
        const bool hidden_prefix = directory && (fs_Options & SCANO_CORES) && wanted.altname[0] == '_';
        const std::string wanted_scroll = std::string(wanted.altname + (hidden_prefix ? 1 : 0));
        const bool short_expanded_file = !directory && (cfg.rbf_hide_datecode || !wanted.datecode[0]) && cfg.browse_expand && std::strlen(wanted.altname) < 55;
        const int wanted_maximum = directory ? 23 : ((!cfg.rbf_hide_datecode && wanted.datecode[0]) ? 20 : 30);
        check(scroll_name == (short_expanded_file ? "" : wanted_scroll), name + ": exact selected scrolling bytes");
        check(scroll_len == (short_expanded_file ? -1 : int(wanted_scroll.size())), name + ": exact selected scrolling byte length");
        check(scroll_max == (short_expanded_file ? -1 : wanted_maximum), name + ": exact selected scrolling span");
    }
    std::vector<std::string> names, alts, types, flags, dates, name_hex, alt_hex, date_hex, cookies;
    for (const auto &item : DirItem)
    {
        names.emplace_back(item.de.d_name); alts.emplace_back(item.altname); dates.emplace_back(item.datecode);
        types.push_back(std::to_string(item.de.d_type)); flags.push_back(std::to_string(item.flags)); cookies.push_back(std::to_string(item.cookie));
        name_hex.push_back(hex(item.de.d_name)); alt_hex.push_back(hex(item.altname)); date_hex.push_back(hex(item.datecode));
    }
    std::vector<std::string> highlighted_rows;
    for (int row : highlights) highlighted_rows.push_back(std::to_string(row));
    observations.push_back("{\"name\":" + quote(name) + ",\"desired_selected\":" + quote(desired)
        + ",\"actual_selected\":" + quote(selected.de.d_name) + ",\"matches_desired\":" + (matched ? "true" : "false")
        + ",\"selected_index\":" + std::to_string(iSelectedEntry) + ",\"first_index\":" + std::to_string(iFirstEntry) + ",\"osd_rows\":" + std::to_string(osd_rows)
        + ",\"filter\":" + quote(filter) + ",\"filter_deadline\":" + std::to_string(filter_typing_timer)
        + ",\"menu_state\":" + std::to_string(menustate) + ",\"entries\":" + array(names)
        + ",\"altnames\":" + array(alts) + ",\"types\":" + array(types) + ",\"flags\":" + array(flags)
        + ",\"datecodes\":" + array(dates) + ",\"cookies\":" + array(cookies)
        + ",\"raw_name_hex\":" + array(name_hex) + ",\"altname_hex\":" + array(alt_hex) + ",\"datecode_hex\":" + array(date_hex)
        + ",\"rows\":" + array(rows) + ",\"highlighted_rows\":" + array(highlighted_rows)
        + ",\"scroll_name\":" + quote(scroll_name) + ",\"scroll_length\":" + std::to_string(scroll_len) + ",\"scroll_maximum\":" + std::to_string(scroll_max)
        + ",\"queries\":" + array(queries) + ",\"setup_queries\":" + array(setup_queries)
        + ",\"centering_calls\":" + std::to_string(center_calls)
        + ",\"desired_first_index\":" + std::to_string(desired_first) + ",\"desired_centering_calls\":" + std::to_string(desired_centers)
        + ",\"assertions\":" + std::to_string(case_checks) + ",\"failed_assertions\":" + std::to_string(case_failures)
        + ",\"failure_messages\":" + array(failure_messages) + '}');
    queries.clear(); setup_queries.clear(); center_calls = 0; case_checks = case_failures = 0; failure_messages.clear();
}
static Entries hidden_triplet()
{
    return {{"_Ace Attacker", DT_DIR}, {"_Body Slam", DT_DIR}, {"_Cotton", DT_DIR}};
}
static Entries numbered_directories(bool cotton = false)
{
    Entries entries;
    for (int i = 0; i < (cotton ? 24 : 40); ++i)
    {
        char name[24]; std::snprintf(name, sizeof(name), "_A%04d", i); entries.emplace_back(name, DT_DIR);
    }
    if (cotton)
    {
        entries.emplace_back("_Cotton folder with an unusually long name", DT_DIR);
        for (int i = 25; i < 40; ++i)
        {
            char name[24]; std::snprintf(name, sizeof(name), "_Z%04d", i); entries.emplace_back(name, DT_DIR);
        }
    }
    return entries;
}
static Entries numbered_files(int count, bool long_names)
{
    Entries entries;
    for (int i = 0; i < count; ++i)
    {
        char start[24]; std::snprintf(start, sizeof(start), "I%04d ", i);
        std::string name(start); name.append((long_names ? 64 : 12) - name.size(), 'x'); name += ".bin";
        entries.emplace_back(name, DT_REG);
    }
    return entries;
}
static std::string file_name(int index, bool long_names)
{
    return numbered_files(index + 1, long_names).back().first;
}
static void original_observations()
{
    reset(hidden_triplet(), SCANO_CORES); type(0x33); observe("core_hidden_first_C", "_Cotton");
    type(0x18, 100); observe("core_hidden_fast_Co", "_Cotton", 0, 0);
    type(0x33, 2500); observe("core_hidden_expired_C", "_Cotton");
    reset(hidden_triplet(), SCANO_CORES); navigate(SCANF_NEXT_CHAR); observe("core_hidden_next_group_B", "_Body Slam");
    navigate(SCANF_NEXT_CHAR); observe("core_hidden_next_group_C", "_Cotton");
    navigate(SCANF_PREV_CHAR); observe("core_hidden_previous_group_B", "_Body Slam");
    reset({{"Ace Attacker", DT_DIR}, {"Body Slam", DT_DIR}, {"Cotton", DT_DIR}}, SCANO_DIR, "BIN");
    type(0x33); observe("ordinary_directory_C", "Cotton");
    reset({{"_Ace Attacker", DT_DIR}, {"_Cotton", DT_DIR}, {"_cOtTon 2", DT_DIR}, {"Cotton_20261014.rbf", DT_REG}, {"Body_20261014.rbf", DT_REG}}, SCANO_CORES);
    type(0x33); observe("core_with_matching_file_preserves_file_result", "Cotton_20261014.rbf");
    reset({{"_Ace Attacker", DT_DIR}, {"_cOtTon", DT_DIR}}, SCANO_CORES);
    type(0x33); observe("core_hidden_case_fold_C", "_cOtTon");
    reset({{"_1Adventure", DT_DIR}, {"_3D World", DT_DIR}, {"_Ace Attacker", DT_DIR}}, SCANO_CORES);
    type(0x03); observe("core_hidden_numeric_3", "_3D World");
    navigate(SCANF_END); navigate('3'); observe("core_hidden_numeric_3_from_later", "_3D World", 0, 2);
    reset({{"../.", DT_DIR}, {"..", DT_DIR}, {"_Ace Attacker", DT_DIR}, {"_Cotton", DT_DIR}}, SCANO_CORES);
    type(0x33); check(std::string(DirItem[0].de.d_name) == "..", "parent priority retained");
    observe("core_parent_remains_first_on_missed_C", "_Cotton");
    reset({{"_Cotton", DT_DIR}, {"Cotton.zip", DT_REG}, {"Cotton.bin", DT_REG}, {"Ace Attacker", DT_DIR}}, SCANO_DIR, "BIN");
    type(0x33); check(DirItem[iSelectedEntry].flags == DT_EXT_ZIP, "actual INIT marks fixture ZIP as directory");
    observe("ordinary_zip_directory_matching_and_order", "Cotton.zip");
    navigate('C'); observe("ordinary_matching_file_after_zip", "Cotton.bin");
    navigate('C'); observe("ordinary_C_wrap_returns_zip", "Cotton.zip");
    reset({{"_Ace Attacker", DT_DIR}, {"_Cotton", DT_DIR}, {"_Cotton.zip", DT_REG}}, SCANO_DIR, "BIN");
    type(0x33); observe("ordinary_literal_underscore_preserved", "_Ace Attacker", 0, 0);
    reset({{"_Ace Attacker", DT_DIR}, {"__Cotton", DT_DIR}}, SCANO_CORES);
    type(0x33); observe("double_underscore_visible_underscore_not_C", "__Cotton", 0, 0);
    reset({{"_Ace Attacker", DT_DIR}, {"_Cotton", DT_DIR}}, SCANO_CORES, "RBFMRAMGL", true);
    type(0x33); observe("archive_synthesized_core_hidden_directory_C", "_Cotton");
    type(0x18, 100); observe("archive_synthesized_directory_filter_current_behavior", "..", 0, 0);
    reset({{"_Ace Attacker", DT_DIR}, {"_Cotton.rbf", DT_REG}}, SCANO_CORES);
    type(0x33); observe("core_file_literal_underscore_preserved", "_Ace Attacker", 0, 0);
    reset({{"_", DT_DIR}, {"_Ace Attacker", DT_DIR}, {"_Cotton", DT_DIR}}, SCANO_CORES);
    type(0x33); observe("core_empty_visible_label_remains_on_missed_C", "_Cotton");
    osd_rows = 8;
    reset({{"_Ace Attacker", DT_DIR}, {"_Cotton folder with an unusually long name", DT_DIR}}, SCANO_CORES);
    type(0x33); observe("core_hidden_long_label_C_at_8_rows", "_Cotton folder with an unusually long name");
    reset({{"_Ace Attacker", DT_DIR}, {"_Cotton folder with an unusually long name", DT_DIR}}, SCANO_CORES);
    navigate(SCANF_NEXT_CHAR); observe("core_hidden_long_label_next_group_at_8_rows", "_Cotton folder with an unusually long name");
    osd_rows = 16;
    reset({{"Ace Attacker", DT_DIR}, {"Cotton", DT_DIR}}, SCANO_DIR, "BIN");
    const auto before = list_signature(); typed_event(UPSTROKE | 0x33);
    check(filter[0] == 0 && filter_typing_timer == 0 && menustate == 0 && before == list_signature(), "key release changes no filter/menu/list state");
    observe("key_release_produces_no_typed_jump", "Ace Attacker", 0, 0);
}
static void additional_contract_cases()
{
    const Entries same{{"_Ace", DT_DIR}, {"_Cotton", DT_DIR}, {"_Cypress", DT_DIR}, {"_Cz", DT_DIR}};
    for (const auto &transition : std::vector<std::pair<std::string, std::string>>{{"_Ace", "_Cotton"}, {"_Cotton", "_Cypress"}, {"_Cypress", "_Cz"}, {"_Cz", "_Cotton"}})
    {
        reset(same, SCANO_CORES); position(transition.first); navigate('C');
        observe("same_initial_" + transition.first + "_to_" + transition.second, transition.second);
    }
    reset(same, SCANO_CORES); position("_Cz"); navigate('c'); observe("lowercase_direct_ascii_wrap", "_Cotton");
    reset({{"_Ace", DT_DIR}, {"_Cotton", DT_DIR}}, SCANO_CORES); position("_Cotton"); navigate('C');
    observe("single_same_initial_wrap_to_current_item", "_Cotton");
    const Entries combined{{"Cotton plain", DT_DIR}, {"_Ace", DT_DIR}, {"_Cotton", DT_DIR}, {"Beta.rbf", DT_REG}, {"Cotton.rbf", DT_REG}};
    reset(combined, SCANO_CORES | SCANO_DIR); type(0x33); observe("combined_plain_hidden_file_existing_forward_choice", "Cotton plain");
    reset(combined, SCANO_CORES | SCANO_DIR); position("Cotton.rbf"); navigate('C');
    observe("combined_existing_wrapped_plain_directory_choice", "Cotton plain");
    reset({{"3D plain", DT_DIR}, {"_3D folder", DT_DIR}, {"_Ace", DT_DIR}}, SCANO_CORES | SCANO_DIR);
    position("3D plain"); navigate('3'); observe("wrapped_raw_plain_match_beats_forward_displayed_fallback", "3D plain");
    reset(combined, SCANO_CORES | SCANO_DIR); position("Cotton plain"); navigate('C');
    observe("raw_file_match_after_current_plain_directory", "Cotton.rbf");
    for (int hide : {0, 1})
    {
        reset({{"_Ace", DT_DIR}, {"_Cotton", DT_DIR}, {"Alpha_20261014.rbf", DT_REG}}, SCANO_CORES,
            "RBFMRAMGL", false, "Alpha:   Cotton translated\n", hide);
        type(0x33);
        check(std::string(DirItem.back().altname) == "Cotton translated" && std::string(DirItem.back().datecode) == "261014", "actual names.txt mapping and RBF datecode extraction");
        check(std::count(queries.begin(), queries.end(), "FileLoad:names.txt") == 2, "actual name map size and copy loads only once across menu INIT calls");
        observe("translated_file_retains_raw_match_priority_date_hidden_" + std::to_string(hide), "Alpha_20261014.rbf");
        const std::string expected = hide ? " Cotton translated              " : " Cotton translated  26.10.14    ";
        check(rows[size_t(iSelectedEntry - iFirstEntry)] == expected, "exact translated file static row bytes/date marker");
        // Attach the independent exact-byte check to its own completed case.
        observe("translated_file_exact_row_date_hidden_" + std::to_string(hide), "Alpha_20261014.rbf", 0, 0);
    }
    const std::string non_ascii = "_\xc3\xa9" "clair";
    for (const auto &start : std::vector<std::string>{"_", "__Cotton", non_ascii})
    {
        reset({{"_", DT_DIR}, {"_Ace", DT_DIR}, {"__Cotton", DT_DIR}, {non_ascii, DT_DIR}}, SCANO_CORES);
        position(start); navigate('C'); observe("excluded_visible_initial_" + hex(start.c_str()), start, 0, 0);
    }
    reset({{"_Ace", DT_DIR}, {"_Cotton", DT_DIR}}, SCANO_CORES); position("_Cotton"); navigate('_');
    observe("non_alphanumeric_mode_keeps_selection", "_Cotton", 0, 0);
    reset({{"_Ace", DT_DIR}, {"_Cotton", DT_DIR}, {"_Cotton.rbf", DT_REG}}, SCANO_DIR, "RBF");
    position("_Cotton"); navigate('C'); observe("ordinary_underscore_directory_remains_literal", "_Cotton", 0, 0);
    reset({{"_Ace", DT_DIR}, {"_Cotton.rbf", DT_REG}}, SCANO_CORES); position("_Ace"); navigate('C');
    observe("underscore_regular_file_not_displayed_directory_fallback", "_Ace", 0, 0);
    reset({{"_Ace", DT_DIR}, {"_Cotton.zip", DT_REG}}, SCANO_CORES | SCANO_DIR, "BIN");
    type(0x33); check(DirItem.back().de.d_type == DT_DIR && DirItem.back().flags == DT_EXT_ZIP, "combined core ZIP gets actual directory type/ZIP flag");
    observe("combined_core_zip_uses_existing_displayed_prefix_rule", "_Cotton.zip");
    reset(hidden_triplet(), SCANO_CORES); position("_Cotton"); navigate(SCANF_PREV_CHAR);
    observe("fresh_previous_group_from_C", "_Body Slam");
    reset(hidden_triplet(), SCANO_CORES); position("_Body Slam"); navigate(SCANF_NEXT_CHAR);
    observe("fresh_next_group_from_B", "_Cotton");
    for (int height : {8, 16}) for (int lookahead : {0, 1})
    {
        osd_rows = height;
        const std::string suffix = "_rows_" + std::to_string(height) + "_lookahead_" + std::to_string(lookahead);
        reset(numbered_directories(true), SCANO_CORES, "RBFMRAMGL", false, "", 1, 0, lookahead);
        navigate('C');
        const int forward_first = lookahead ? 24 - height / 2 : 25 - height;
        observe("late_hidden_forward_center" + suffix, "_Cotton folder with an unusually long name", forward_first);
        reset(numbered_directories(true), SCANO_CORES, "RBFMRAMGL", false, "", 1, 0, lookahead);
        position("_Z0039"); navigate('C');
        const int wrapped_first = lookahead ? 24 - height / 2 : 24;
        observe("late_hidden_wrapped_center" + suffix, "_Cotton folder with an unusually long name", wrapped_first);
        reset(numbered_directories(), SCANO_CORES, "RBFMRAMGL", false, "", 1, 0, lookahead);
        position("_A0025"); const int before_first = iFirstEntry; navigate('C');
        observe("missing_initial_nonzero_viewport" + suffix, "_A0025", before_first, 0);
        reset(numbered_files(2 * height + 3, true), SCANO_DIR, "BIN", false, "", 1, 1, lookahead);
        observe("expanded_file_initial" + suffix, file_name(0, true), 0, 0, true);
        navigate(SCANF_NEXT_PAGE);
        observe("expanded_file_forward_page" + suffix, file_name(height - 1, true), lookahead ? height - 1 - height / 2 : 1, 1, true);
        navigate(SCANF_PREV_PAGE);
        observe("expanded_file_reverse_page" + suffix, file_name(0, true), 0, 1, true);
        reset(numbered_files(2 * height + 3, false), SCANO_DIR, "BIN", false, "", 1, 0, lookahead);
        navigate(SCANF_NEXT_PAGE);
        observe("ordinary_file_forward_page" + suffix, file_name(height, false), lookahead ? height / 2 : 1);
        navigate(SCANF_PREV_PAGE);
        observe("ordinary_file_reverse_page" + suffix, file_name(0, false));
    }
}
int main()
{
    try
    {
        original_observations(); additional_contract_cases();
        require(case_checks == 0 && case_failures == 0, "every asserted transition included in a completed unique case");
        std::printf("CASE_JSON={\"summary\":{\"cases\":%zu,\"assertions\":%d,\"failed_assertions\":%d},\"cases\":[", observations.size(), checks, failures);
        for (size_t i = 0; i < observations.size(); ++i) { if (i) std::printf(","); std::printf("%s", observations[i].c_str()); }
        std::puts("]}");
        return failures ? 1 : 0;
    }
    catch (const std::exception &error) { std::fprintf(stderr, "FIXTURE ERROR: %s\n", error.what()); return 2; }
}
