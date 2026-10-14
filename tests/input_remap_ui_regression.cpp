// Host fixtures execute the intact remapping accessor, source row helper,
// chord renderer, boxed renderer and both production MENU_JOYKBDMAP cases.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "str_util.h"
#include "input_remap_ui_declarations.inc"

static int mapping = 0;
static int mapping_dev = -1;
static int mapping_set = 1;
static int cancel_result = 0;
static int finish_result = 0;
static int advance_result = 0;
static int finish_calls = 0;
static int finish_dismiss = -1;
static bool finish_handoff = false;
static int update_calls = 0;
static int sleep_calls = 0;
static int start_calls = 0;
static int start_count = 0;
static int start_set = -1;
static advancedButtonMap *start_store = nullptr;
static uint16_t entry_capture_code = 0;

enum { MENU_JOYKBDMAP, MENU_JOYKBDMAP1, MENU_COMMON1, MENU_ADVANCED_MAP_LIST1 };
static int menustate = MENU_JOYKBDMAP;
static int parentstate = MENU_COMMON1;
static int menusub = 0;
static int helptext_idx = 0;
static int menumask = 0;
static advancedButtonMap abm_edit_map = {};
static int abm_dev_num = -1;
static const char *info_top = "fixture top";
static const char *info_bottom = "fixture bottom";

struct RowWrite { int row; std::string text; };
static std::array<std::string, 16> rows;
static std::vector<RowWrite> writes;
static std::vector<RowWrite> boxed_labels;
static std::array<std::string, 16> updated_rows;
static std::string title;
static int checks = 0;
static int failures = 0;

static void check(bool condition, const char *description)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", description);
    }
}

static void OsdWrite(int row, const char *text = "", int invert = 0, int stipple = 0)
{
    (void)invert;
    (void)stipple;
    if (row < 0 || row >= static_cast<int>(rows.size())) {
        check(false, "OSD row in fixture range");
        return;
    }
    rows[row] = text;
    writes.push_back({row, text});
}

static void OsdSetTitle(const char *value, int arrow) { (void)arrow; title = value; }
static int OsdGetSize() { return static_cast<int>(rows.size()); }
static void OsdUpdate() { ++update_calls; updated_rows = rows; }
static void sleep(unsigned int seconds) { check(seconds == 1, "completion wait remains one second"); ++sleep_calls; }
static void start_map_setting(int count, int set, advancedButtonMap *store)
{
    ++start_calls;
    start_count = count;
    start_set = set;
    start_store = store;
    mapping_dev = -1;
    mapping_set = 1;
    mapping = 1;
    // The fixture can populate a captured key at the start/render boundary to
    // cover the entry help's keyboard branch as well as its gamepad branch.
    store->input_codes[0] = entry_capture_code;
}
static int get_map_cancel() { return cancel_result; }
static int get_map_finish() { return finish_result; }
static int get_map_advance() { return advance_result; }
static int get_map_set() { return mapping_set; }
static void finish_map_setting(int dismiss, bool advanced_handoff)
{
    ++finish_calls;
    finish_dismiss = dismiss;
    finish_handoff = advanced_handoff;
    mapping = 0;
    // Production mapping completion retains the chosen mapping_dev. Persistence
    // is outside this renderer test; the fixture records both finish arguments.
}

static void production_infowrite(int pos, const char *text);
static void infowrite(int pos, const char *text)
{
    boxed_labels.push_back({pos, text});
    production_infowrite(pos, text);
}

// Only device-dependent human button labels are fixtures. The complete actual
// chord formatter below still walks and formats the production code arrays.
static void get_button_name_for_code(uint16_t code, int dev, char *dest, size_t size)
{
    check(dev == abm_dev_num, "button name uses selected remap device");
    std::snprintf(dest, size, "B%u", code - 256);
}

#include "input_remap_ui_production.inc"

static const char *no_source = "No device: press button/key";

static void reset()
{
    std::memset(input, 0, sizeof(input));
    std::memset(&abm_edit_map, 0, sizeof(abm_edit_map));
    mapping = 1;
    mapping_dev = -1;
    mapping_set = 1;
    cancel_result = finish_result = advance_result = 0;
    finish_calls = update_calls = sleep_calls = start_calls = 0;
    finish_dismiss = -1;
    finish_handoff = false;
    start_store = nullptr;
    entry_capture_code = 0;
    menustate = MENU_JOYKBDMAP1;
    parentstate = MENU_COMMON1;
    menusub = 0;
    abm_dev_num = -1;
    for (auto &row : rows) row.clear();
    for (auto &row : updated_rows) row.clear();
    writes.clear();
    boxed_labels.clear();
    title.clear();
}

static std::string trim(const std::string &value)
{
    const size_t start = value.find_first_not_of(' ');
    if (start == std::string::npos) return "";
    return value.substr(start, value.find_last_not_of(' ') - start + 1);
}

static void select_device(int dev, const char *name)
{
    mapping_dev = dev;
    input[dev].vid = 0x1234;
    input[dev].pid = 0xabcd;
    std::snprintf(input[dev].name, sizeof(input[dev].name), "%s", name);
}

static void check_source(const std::string &expected, const char *description)
{
    check(trim(rows[0]) == expected, description);
    check(rows[0].size() <= 29, "source prompt fits ordinary 29-column OSD row");
}

static void check_command_widths()
{
    for (const auto &label : boxed_labels) {
        check(label.text.size() <= 27, "raw boxed help label fits 27-column interior");
        if (label.text.find("Clear") != std::string::npos)
            check(label.text.find("Clear remaps") != std::string::npos,
                  "boxed clear help explicitly identifies remap scope");
        check(rows[label.row].size() == 29, "boxed production renderer retains 29-column borders");
    }
    for (const auto &write : writes) {
        if (write.row == 15)
            check(write.text.size() <= 29, "ordinary command label fits 29-column OSD row");
    }
}

static void accessor_cases()
{
#if HAVE_SOURCE_ACCESSOR
    reset();
    char result[256] = "unchanged";
    get_map_source_name(result, sizeof(result));
    check(result[0] == 0, "unselected source returns empty name");
    for (int dev : {-100, -1, NUMDEV, NUMDEV + 1, 1000000}) {
        mapping_dev = dev;
        std::strcpy(result, "unchanged");
        get_map_source_name(result, sizeof(result));
        check(result[0] == 0, "out-of-range source returns empty name");
    }
    for (int dev : {0, NUMDEV - 1}) {
        select_device(dev, "8BitDo Pro 2");
        get_map_source_name(result, sizeof(result));
        check(std::strcmp(result, "8BitDo Pro 2") == 0, "first and last valid cached devices resolve");
    }
    select_device(2, "Keyboard source");
    std::memset(input[2].name, 'L', sizeof(input[2].name) - 1);
    input[2].name[sizeof(input[2].name) - 1] = 0;
    get_map_source_name(result, sizeof(result));
    check(std::strlen(result) == sizeof(input[2].name) - 1 && result[126] == 'L',
          "getter preserves a maximum-length terminated cached name");
    select_device(2, "Keyboard source");
    get_map_source_name(nullptr, 0);
    get_map_source_name(nullptr, 5);
    char zero[3] = {'A', 'B', 'C'};
    get_map_source_name(zero + 1, 0);
    check(zero[0] == 'A' && zero[1] == 'B' && zero[2] == 'C', "zero-size getter leaves buffer untouched");
    char one[3] = {'A', 'B', 'C'};
    get_map_source_name(one + 1, 1);
    check(one[0] == 'A' && one[1] == 0 && one[2] == 'C', "one-byte getter terminates without overwriting guards");
    char small[8];
    std::memset(small, 'X', sizeof(small));
    get_map_source_name(small + 1, 5);
    check(std::strcmp(small + 1, "Keyb") == 0 && small[0] == 'X' && small[6] == 'X' && small[7] == 'X',
          "small getter truncates and preserves guard bytes");
    input[2].name[0] = 0;
    get_map_source_name(result, sizeof(result));
    check(trim(result) == "Device 1234:ABCD" || trim(result) == "Device 1234:abcd",
          "empty cached name falls back to device VID:PID");
    std::memset(small, 'X', sizeof(small));
    get_map_source_name(small + 1, 5);
    check(std::strcmp(small + 1, "Devi") == 0 && small[0] == 'X' && small[6] == 'X',
          "small getter bounds VID:PID fallback too");
    std::memset(input[2].name, 'N', sizeof(input[2].name));
    std::memset(input[2].guard, 'G', sizeof(input[2].guard));
    get_map_source_name(result, sizeof(result));
    check(std::strlen(result) == sizeof(input[2].name), "nonterminated cached name bounded to its 128-byte field");
    check(std::all_of(result, result + sizeof(input[2].name), [](char c) { return c == 'N'; }),
          "nonterminated cached name never includes adjacent fields");
    select_device(2, "After completion");
    mapping = 0;
    get_map_source_name(result, sizeof(result));
    check(std::strcmp(result, "After completion") == 0, "cached selected source remains available after completion");
#else
    check(false, "source-name accessor missing; historical renderer tests still execute");
#endif
}

static void helper_cases()
{
#if HAVE_SOURCE_HELPER
    reset();
    for (int i = 1; i <= 4; ++i) rows[i] = "preserve chord and instructions";
    check(!menu_show_map_source(), "helper reports no selected source");
    check_source(no_source, "helper prompts for first device when unselected");
    for (int i = 1; i <= 4; ++i)
        check(rows[i] == "preserve chord and instructions", "source helper preserves instruction/chord rows 1..4");
    select_device(1, "Keyboard");
    check(menu_show_map_source(), "helper reports selected source");
    check_source("Source: Keyboard", "helper identifies a selected ordinary cached name");
    select_device(1, "12345678901234567890 and trailing text");
    check(menu_show_map_source(), "helper reports a long selected source");
    check_source("Source: 12345678901234567890", "helper limits display name to twenty columns");
    select_device(1, "");
    check(menu_show_map_source(), "helper reports fallback identifier as selected source");
    check(trim(rows[0]) == "Source: Device 1234:ABCD" || trim(rows[0]) == "Source: Device 1234:abcd",
          "helper displays VID:PID fallback");
    mapping_dev = NUMDEV;
    check(!menu_show_map_source(), "helper reports invalid source as unselected");
    check_source(no_source, "helper handles invalid source selection");
#endif
}

static void renderer_cases()
{
    reset();
    menustate = MENU_JOYKBDMAP;
    abm_edit_map.input_codes[0] = 30;
    render_remap();
    check(menustate == MENU_JOYKBDMAP1 && parentstate == MENU_JOYKBDMAP,
          "entry retains original remap state transition");
    check(start_calls == 1 && start_count == 3 && start_set == 0 && start_store == &abm_edit_map,
          "entry starts original advanced remap capture");
    check(abm_edit_map.input_codes[0] == 0 && title == "Button/Key remap", "entry resets remap chord and sets title");
    check_source(no_source, "actual entry renders no-device prompt");
    check_command_widths();

    reset();
    menustate = MENU_JOYKBDMAP;
    // Inject a code only to check the dormant keyboard-help branch widths;
    // actual start_map_setting clears the entry before its first render.
    entry_capture_code = 29;
    render_remap();
    check_source(no_source, "synthetic keyboard-help width probe retains no-device prompt");
    check_command_widths();
    render_remap();
    check_source(no_source, "actual unselected capture retains no-device prompt");
    check(trim(rows[1]) == "Press button(s)" && trim(rows[2]) == "or key(s) to change",
          "unselected capture retains original instructions");
    check_command_widths();

    for (bool keyboard : {true, false}) {
        reset();
        select_device(3, keyboard ? "USB Keyboard" : "Gamepad source");
        abm_edit_map.input_codes[0] = keyboard ? 29 : 257;
        abm_edit_map.input_codes[1] = keyboard ? 42 : 258;
        render_remap();
        check_source(keyboard ? "Source: USB Keyboard" : "Source: Gamepad source", "actual source capture identifies selected device");
        check(trim(rows[3]) == (keyboard ? "[k1D,k2A]" : "[B1,B2]"), "actual source chord remains on row 3");
        check(rows[4].empty(), "source capture clears original output row 4");
        check(abm_dev_num == 3, "actual renderer retains selected device for advanced mapping");
        check_command_widths();

        mapping_set = 2;
        abm_edit_map.output_codes[0] = 30;
        abm_edit_map.output_codes[1] = 31;
        render_remap();
        check_source(keyboard ? "Source: USB Keyboard" : "Source: Gamepad source", "actual output capture retains source device");
        check(trim(rows[keyboard ? 3 : 4]) == "[k1E,k1F]", "actual output chord remains on its original row");
        if (keyboard) {
            check(trim(rows[1]) == "Press key(s) to map to" && trim(rows[2]) == "on a keyboard",
                  "keyboard output instructions unchanged");
        } else {
            check(trim(rows[1]) == "Press button(s) to map to" && trim(rows[2]) == "on the same pad" &&
                  trim(rows[3]) == "or key(s) on a keyboard", "gamepad output instructions unchanged");
        }
    }

    reset();
    select_device(3, "An extremely long device name that must fit");
    render_remap();
    check_source("Source: An extremely long de", "actual capture bounds a long device label");

    reset();
    select_device(3, "");
    render_remap();
    check(trim(rows[0]) == "Source: Device 1234:ABCD" || trim(rows[0]) == "Source: Device 1234:abcd",
          "actual capture displays empty-name VID:PID fallback");
    check(rows[0].size() <= 29, "actual capture fallback fits ordinary OSD row");

    reset();
    select_device(3, "");
    std::memset(input[3].name, 'N', sizeof(input[3].name));
    std::memset(input[3].guard, 'G', sizeof(input[3].guard));
    render_remap();
    check_source("Source: NNNNNNNNNNNNNNNNNNNN", "actual capture safely bounds nonterminated cached name");

    for (int dev : {-1, NUMDEV}) {
        reset();
        mapping_dev = dev;
        render_remap();
        check_source(no_source, "actual capture safely prompts for unselected/invalid source");
    }

    for (bool chosen : {false, true}) {
        for (int dismiss : {0, 1}) {
            reset();
            if (chosen) select_device(4, "Chosen source");
            cancel_result = dismiss;
            finish_result = !dismiss;
            render_remap();
            check_source(chosen ? "Source: Chosen source" : no_source,
                         "actual completion retains source identity or no-device prompt");
            check(updated_rows[0] == rows[0] && update_calls == 1, "completion flushes source row with status");
            check(trim(rows[2]) == (dismiss ? (chosen ? "Clearing source remaps" : "No device chosen") : "Finishing"),
                  "completion status distinguishes selected clear from no-device clear");
            check(rows[1].empty() && rows[3].empty() && rows[15].empty(), "completion keeps original instruction clears");
            check(finish_calls == 1 && finish_dismiss == dismiss, "completion preserves exact finish dismiss argument");
            check(!finish_handoff, "ordinary Clear and Finish must not retain Advanced release ownership");
            check(menustate == MENU_COMMON1 && menusub == 3 && sleep_calls == 1, "completion retains original menu return");
            check(mapping_dev == (chosen ? 4 : -1), "completion preserves selected source slot");
        }
    }

    reset();
    mapping_dev = NUMDEV;
    cancel_result = 1;
    render_remap();
    check_source(no_source, "actual clear with invalid selected index retains no-device prompt");
    check(trim(rows[2]) == "No device chosen", "invalid index never claims a device remap clear");
    check(finish_calls == 1 && finish_dismiss == 1, "invalid source still preserves original finish dismiss argument");
    check(!finish_handoff, "invalid-source Clear must use the default non-handoff finish");

    reset();
    select_device(5, "Advanced source");
    advance_result = 1;
    render_remap();
    check_source("Source: Advanced source", "actual advance transition displays chosen source");
    check(menustate == MENU_ADVANCED_MAP_LIST1 && menusub == 0 && finish_calls == 1 && finish_dismiss == 0 && abm_dev_num == 5,
          "advance retains selected device and original transition/finish argument");
    check(finish_handoff, "actual Advance branch must explicitly retain consumed-command release ownership");
}

int main()
{
    accessor_cases();
    helper_cases();
    renderer_cases();
    std::printf("%s: remap UI %d checks, %d failures; production accessor/helper/chord/boxed renderer and both menu cases\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
