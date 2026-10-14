#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "cfg.h"

#ifdef __linux__
#include <linux/input.h>
#else
// Stable Linux event ABI constants for POSIX hosts without Linux headers.
// The host event record only needs the fields used by this capture path.
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define KEY_ESC 1
#define KEY_Q 16
#define KEY_W 17
#define KEY_E 18
#define KEY_R 19
#define KEY_T 20
#define KEY_Y 21
#define KEY_U 22
#define KEY_O 24
#define KEY_P 25
#define KEY_ENTER 28
#define KEY_A 30
#define KEY_B 48
#define KEY_SPACE 57
#define KEY_F12 88
#define KEY_ALTERASE 222
#define KEY_UNKNOWN 240
#define KEY_MAX 0x2ff
#define BTN_MOUSE 0x110
#define BTN_LEFT 0x110
#define BTN_JOYSTICK 0x120
#define BTN_SOUTH 0x130
#define BTN_EAST 0x131
#define BTN_NORTH 0x133
#define BTN_WEST 0x134
#define BTN_MODE 0x13c
struct input_event { uint16_t type; uint16_t code; int32_t value; };
#endif

#include "input_remap_types.inc"

struct DeviceFixture
{
	int bind;
	QUIRK quirk;
	uint32_t misc_flags;
	int timeout;
	bool ds_mouse_emu;
	bool force_joy;
	int has_kbdmap;
	uint16_t kbdmap[256];
	uint32_t mmap[NUMBUTTONS];
	int osd_combo;
	int has_map;
	int has_mmap;
	uint32_t map[NUMBUTTONS];
	advancedButtonMap advanced_map[ADVANCED_MAP_MAX];
};

struct DeviceFixtures
{
	DeviceFixture records[NUMDEV];
	DeviceFixture &operator[](int index)
	{
		if (index < 0 || index >= NUMDEV)
		{
			std::fprintf(stderr, "invalid production device index: %d\n", index);
			std::abort();
		}
		return records[index];
	}
};

static DeviceFixtures input;
cfg_t cfg = {};
static unsigned long now_ms = 1000;
static int mice_btn = 0;
static int mouse_requests = 0;
static int released_enter = 0;
static bool menu_core = false;
struct SavedEntry { int device; advancedButtonMap entry; };
static std::vector<SavedEntry> saved;
static std::vector<int> cleared;

bool is_menu() { return menu_core; }
unsigned long GetTimer(unsigned long offset) { return now_ms + offset; }
void user_io_kbd(uint16_t key, int press) { if (key == KEY_ENTER && !press) released_enter++; }
void mouse_btn_req() { mouse_requests++; }
const char *get_kbdmap_name(int) { return "fixture-kbd.map"; }
const char *get_map_name(int, int) { return "fixture-pad.map"; }
int FileLoadConfig(const char *, void *, int) { return 0; }
int FileSaveConfig(const char *, void *, int) { return 1; }
int FileDeleteConfig(const char *) { return 1; }
int save_map(const char *, void *, int) { return 1; }
void delete_map(const char *) {}
void input_advanced_save_entry(advancedButtonMap *entry, int device) { saved.push_back({device, *entry}); }
void input_advanced_save(int device, bool do_delete)
{
	if (!do_delete) std::abort();
	cleared.push_back(device);
}

#include "input_remap_production.inc"

static int failures = 0;
static unsigned int checks = 0;
static void check(bool condition, const char *message)
{
	checks++;
	if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); failures++; }
}

struct GuardedEntry
{
	uint64_t before;
	advancedButtonMap entry;
	uint64_t after;
};
static GuardedEntry store;
static const uint64_t sentinel = UINT64_C(0x125113051245abcd);

static void begin()
{
	std::memset(&input.records, 0, sizeof(input.records));
	for (int i = 0; i < NUMDEV; i++)
	{
		input[i].bind = i;
		input[i].has_kbdmap = input[i].has_mmap = 1;
		input[i].mmap[SYS_BTN_MENU_FUNC] = BTN_SOUTH;
		input[i].advanced_map[0].input_codes[0] = 500 + i;
	}
	std::memset(&store, 0, sizeof(store));
	store.before = store.after = sentinel;
	saved.clear(); cleared.clear();
	cfg = cfg_t{};
	now_ms = 1000;
	mice_btn = mouse_requests = released_enter = 0;
	menu_core = false;
	start_map_setting(3, 0, &store.entry);
	check(get_map_dev() == -1 && get_map_set() == 1 && get_map_button() == 0,
		"fresh capture must reset target and input phase");
	check(released_enter == 1, "initialization must retain Enter un-stick output");
}

static input_event event(int device, int code, int value, int type = EV_KEY)
{
	input_event ev = {};
	ev.type = type; ev.code = code; ev.value = value;
	capture_event(&ev, device);
	check(store.before == sentinel && store.after == sentinel, "capture must not overwrite chord storage bounds");
	return ev;
}

static void tap(int device, int code) { event(device, code, 1); event(device, code, 0); }
static void map_to_keyboard(int source_device = 1, int source = BTN_EAST, int target = KEY_O)
{
	tap(source_device, source);
	check(get_map_set() == 2, "source release must begin output capture");
	tap(0, target);
}

static void pending_events()
{
	begin();
	for (int code : {KEY_ENTER, KEY_F12, BTN_EAST})
	for (int value : {0, 2})
	{
		event(0, code, value);
		check(get_map_dev() == -1, "opener release/repeat must not select a target");
		check(get_map_button() == 0 && !get_map_finish() && !get_map_cancel() && !get_map_advance(),
			"pending events must not capture codes or invoke commands");
	}
	map_to_keyboard();
	check(saved.size() == 1 && saved[0].device == 1, "first pad press after pending events must own the saved mapping");
	check(!saved.empty() && saved[0].entry.input_codes[0] == BTN_EAST && saved[0].entry.output_codes[0] == KEY_O,
		"a different keyboard must remain available for output capture");
}

static void combo_opener_release()
{
	begin();
	input[1].mmap[SYS_BTN_OSD_KTGL + 1] = input[1].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	input[1].osd_combo = 3;
	input_event release = event(1, BTN_MODE, 0);
	check(input[1].osd_combo == 0 && release.code == KEY_ESC && release.value == 0,
		"real prefix must process combo release and CheckTimer(0) conversion");
	check(get_map_dev() == -1 && !get_map_cancel(), "rewritten opener release must leave target unselected");
	map_to_keyboard();
	check(saved.size() == 1 && saved[0].device == 1, "new source press must work after combo opener release");
}

static void abort_and_clear()
{
	begin();
	event(0, KEY_ENTER, 0);
	finish_map_setting(1);
	check(cleared.empty() && saved.empty(), "abort before any source press must not clear or save a device");
	check(input[0].advanced_map[0].input_codes[0] == 500 && input[1].advanced_map[0].input_codes[0] == 501,
		"unselected abort must preserve all existing device mappings");
	begin();
	event(1, BTN_EAST, 1);
	finish_map_setting(1);
	check(cleared == std::vector<int>{1}, "selected clear must target only that pad");
	check(!input[1].advanced_map[0].input_codes[0] && input[0].advanced_map[0].input_codes[0] == 500,
		"actual input_advanced_clear must preserve other devices");
}

static void normalization_and_binding()
{
	begin();
	event(0, 0, 1, EV_SYN); event(0, 0, 1); event(0, KEY_UNKNOWN, 1);
	event(0, 0, 1, EV_ABS); event(0, 0, 1, EV_REL); event(0, BTN_LEFT, 1);
	check(get_map_dev() == -1 && mouse_requests == 1, "filtered/non-key/mouse events must not select a remap target");
	input[2].bind = 1;
	cfg.bt_auto_disconnect = 6;
	input[1].timeout = 1;
	input[0].kbdmap[KEY_Q] = KEY_O;
	map_to_keyboard(2, BTN_EAST, KEY_Q);
	check(saved.size() == 1 && saved[0].device == 1 && saved[0].entry.output_codes[0] == KEY_O,
		"binding and keyboard normalization must precede capture");
	check(input[1].timeout == 60, "bound device activity must retain timeout refresh");
	begin();
	input[1].force_joy = true;
	input[1].kbdmap[KEY_A] = KEY_B;
	map_to_keyboard(1, KEY_A);
	check(saved.size() == 1 && saved[0].entry.input_codes[0] == KEY_A, "forced joystick keys must retain keyboard-remap bypass");
}

static void continued_mapping_and_restart()
{
	begin();
	tap(1, BTN_EAST); tap(1, BTN_WEST);
	check(saved.size() == 1 && saved[0].device == 1 && saved[0].entry.output_codes[0] == BTN_WEST,
		"same-device source/output must still save");
	check(get_map_set() == 1 && get_map_dev() == 1 && !store.entry.input_codes[0] && !store.entry.output_codes[0],
		"completion must clear temporary codes while preserving target");
	map_to_keyboard(1, BTN_NORTH, KEY_P);
	check(saved.size() == 2 && saved[1].device == 1, "subsequent mappings must keep the selected device");
	start_map_setting(3, 0, &store.entry);
	check(get_map_dev() == -1, "fresh dialog restart must reset the target");
	map_to_keyboard(3);
	check(saved.size() == 3 && saved[2].device == 3, "a new dialog must permit a different source device");
}

static void advanced_edits()
{
	// Advanced edits capture one half of an existing row. Persistence belongs
	// to the Advanced dialog caller, so this path must not save/clear a device.
	begin();
	store.entry.input_codes[0] = BTN_WEST;
	store.entry.output_codes[0] = KEY_O;
	store.entry.output_codes[1] = KEY_P;
	start_map_setting(1, 1, &store.entry);
	check(get_map_count() == 1 && get_map_set() == 1 && get_map_dev() == -1
		&& !store.entry.input_codes[0] && store.entry.output_codes[0] == KEY_O,
		"Advanced input edit must clear only input and reset target");
	event(0, KEY_ENTER, 0); event(0, KEY_ENTER, 2);
	check(get_map_dev() == -1 && !get_map_finish(), "pending Advanced input events must not choose the source");
	event(1, BTN_EAST, 1); event(1, BTN_NORTH, 1); event(1, BTN_EAST, 0);
	check(get_map_dev() == 1 && get_map_finish() && get_map_set() == 1
		&& store.entry.input_codes[0] == BTN_EAST && store.entry.input_codes[1] == BTN_NORTH
		&& store.entry.output_codes[0] == KEY_O && store.entry.output_codes[1] == KEY_P,
		"Advanced input chord must finish without altering the output half");
	finish_map_setting(0);
	check(saved.empty() && cleared.empty() && store.entry.input_codes[0] == BTN_EAST,
		"accepted Advanced input edit must retain captured codes without direct persistence");
	start_map_setting(1, 1, &store.entry);
	event(1, BTN_WEST, 1); finish_map_setting(1);
	check(!store.entry.input_codes[0] && store.entry.output_codes[0] == KEY_O && cleared.empty(),
		"dismissed Advanced input edit must clear only edited input codes");

	begin();
	store.entry.input_codes[0] = BTN_EAST;
	store.entry.input_codes[1] = BTN_NORTH;
	store.entry.output_codes[0] = KEY_P;
	start_map_setting(1, 2, &store.entry);
	check(get_map_count() == 1 && get_map_set() == 2 && get_map_dev() == -1
		&& store.entry.input_codes[0] == BTN_EAST && !store.entry.output_codes[0],
		"Advanced output edit must clear only output and reset target");
	event(1, BTN_EAST, 0); event(1, BTN_EAST, 2);
	check(get_map_dev() == -1 && !get_map_finish(), "pending Advanced output events must not choose a target");
	event(0, KEY_O, 1); event(0, KEY_P, 1); event(0, KEY_O, 0);
	check(get_map_dev() == 0 && get_map_finish() && get_map_set() == 2
		&& store.entry.output_codes[0] == KEY_O && store.entry.output_codes[1] == KEY_P
		&& store.entry.input_codes[0] == BTN_EAST && store.entry.input_codes[1] == BTN_NORTH,
		"Advanced output edit must accept the keyboard and preserve source codes");
	finish_map_setting(0);
	check(saved.empty() && cleared.empty() && store.entry.output_codes[0] == KEY_O,
		"accepted Advanced output edit must retain captured codes without direct persistence");
	start_map_setting(1, 2, &store.entry);
	event(0, KEY_ENTER, 1);
	check(!get_map_finish() && store.entry.output_codes[0] == KEY_ENTER,
		"Enter must remain output data in a one-half Advanced edit");
	finish_map_setting(1);
	check(!store.entry.output_codes[0] && store.entry.input_codes[0] == BTN_EAST && cleared.empty(),
		"dismissed Advanced output edit must clear only edited output codes");
}

static void keyboard_and_synthesized_direction()
{
	begin();
	tap(0, KEY_A);
	check(get_map_dev() == 0 && get_map_set() == 2, "a fresh keyboard source must select its own device");
	tap(2, KEY_B);
	check(saved.size() == 1 && saved[0].device == 0
		&& saved[0].entry.input_codes[0] == KEY_A && saved[0].entry.output_codes[0] == KEY_B,
		"keyboard-to-keyboard capture must permit a different output keyboard");
	begin();
	// input_poll synthesizes EV_KEY at KEY_EMU + axis*2 for the low edge,
	// and code+1 for the high edge. Exercise a reversal's release then press
	// at the callback boundary, without claiming to test evdev axis decoding.
	const int low_direction = KEY_EMU + 2;
	const int high_direction = low_direction + 1;
	input[2].bind = 1;
	event(0, low_direction, 0);
	check(get_map_dev() == -1, "a stale synthesized direction release must leave the source unselected");
	event(2, high_direction, 1);
	check(get_map_dev() == 1 && store.entry.input_codes[0] == high_direction,
		"a synthesized direction press must choose the bound source and retain its code");
	event(2, high_direction, 0); tap(0, KEY_O);
	check(saved.size() == 1 && saved[0].device == 1
		&& saved[0].entry.input_codes[0] == high_direction && saved[0].entry.output_codes[0] == KEY_O,
		"synthesized direction capture must save against the actual first-press source");
}

static void chords_and_bounds()
{
	begin();
	event(1, BTN_EAST, 1); event(1, BTN_NORTH, 1); event(1, BTN_NORTH, 2);
	check(get_map_button() == 2, "selected repeats must not add chord members");
	event(1, BTN_EAST, 0); event(1, BTN_NORTH, 0);
	event(0, KEY_O, 1); event(0, KEY_P, 1); event(0, KEY_O, 0); event(0, KEY_P, 0);
	check(saved.size() == 1 && saved[0].device == 1
		&& saved[0].entry.input_codes[0] == BTN_EAST && saved[0].entry.input_codes[1] == BTN_NORTH
		&& saved[0].entry.output_codes[0] == KEY_O && saved[0].entry.output_codes[1] == KEY_P,
		"source/output chords and their releases must preserve payload and target");
	begin();
	for (int code = BTN_SOUTH; code < BTN_SOUTH + 7; code++) event(1, code, 1);
	event(1, BTN_SOUTH, 0);
	for (int code = KEY_Q; code <= KEY_U; code++) event(0, code, 1);
	event(0, KEY_Q, 0);
	check(saved.size() == 1 && saved[0].entry.input_codes[0] == BTN_SOUTH
		&& saved[0].entry.input_codes[2] == BTN_SOUTH + 2 && saved[0].entry.input_codes[3] == BTN_SOUTH + 6
		&& saved[0].entry.output_codes[0] == KEY_Q && saved[0].entry.output_codes[3] == KEY_U,
		"overflow must retain first three codes and replace only the final slot");
}

static void chord_control_keys()
{
	begin();
	event(1, BTN_EAST, 1); event(0, KEY_ENTER, 1);
	check(get_map_dev() == 1 && get_map_finish(),
		"Enter after one source member must retain the existing Finish gate and selected device");
	finish_map_setting(0);

	begin();
	event(1, BTN_EAST, 1); event(1, BTN_NORTH, 1);
	// Interrupts are allowed while the source has zero/one existing members.
	// Once two ordinary members exist, Enter/Esc must remain chord data.
	event(1, KEY_ENTER, 1); event(1, KEY_ESC, 1);
	check(!get_map_finish() && !get_map_cancel() && !get_map_advance()
		&& store.entry.input_codes[2] == KEY_ENTER && store.entry.input_codes[3] == KEY_ESC,
		"later Enter/Esc source members must preserve the existing interrupt gate");
	event(1, KEY_ESC, 0);
	check(get_map_set() == 2 && !get_map_cancel(), "Esc release within a source chord must advance to output capture");
	tap(0, KEY_O);
	check(saved.size() == 1 && saved[0].device == 1
		&& saved[0].entry.input_codes[2] == KEY_ENTER && saved[0].entry.input_codes[3] == KEY_ESC,
		"source chord control codes must persist as data");
	begin();
	tap(1, BTN_EAST);
	event(0, KEY_A, 1); event(0, KEY_B, 1);
	event(0, KEY_ENTER, 1); event(0, KEY_ESC, 1); event(0, KEY_ESC, 0);
	check(saved.size() == 1 && saved[0].device == 1
		&& saved[0].entry.output_codes[0] == KEY_A && saved[0].entry.output_codes[1] == KEY_B
		&& saved[0].entry.output_codes[2] == KEY_ENTER && saved[0].entry.output_codes[3] == KEY_ESC
		&& !get_map_finish() && !get_map_cancel(),
		"Enter/Esc within an output chord must save as data against the source device");
}

static void commands_and_timers()
{
	begin();
	event(0, KEY_ENTER, 1);
	check(get_map_dev() == 0 && get_map_finish(), "first Enter press must retain existing finish/device-choice semantics");
	finish_map_setting(0);
	check(cleared.empty(), "Finish must not clear selected mappings");
	begin();
	event(0, KEY_F12, 1);
	check(get_map_dev() == 0 && get_map_advance(), "first F12 press must retain existing Advanced semantics");
	finish_map_setting(0);
	begin();
	event(0, KEY_ESC, 1); event(0, KEY_ESC, 0);
	check(get_map_dev() == 0 && get_map_cancel(), "first actual Esc press/release must retain selected clear semantics");
	finish_map_setting(1);
	check(cleared == std::vector<int>{0}, "first actual Esc control must clear the device it selected");
	begin();
	map_to_keyboard();
	event(0, KEY_F12, 1);
	check(get_map_dev() == 1 && get_map_advance(), "Advanced from a different keyboard must retain the selected pad");
	finish_map_setting(0);
	check(cleared.empty(), "Advanced transition must not clear mappings");
	begin();
	map_to_keyboard();
	event(1, BTN_SOUTH, 1);
	now_ms += 4999;
	check(!get_map_advance(), "mapped OK hold must not advance before five seconds");
	now_ms++;
	check(get_map_advance(), "mapped OK hold must advance at its deadline");
	event(1, BTN_SOUTH, 0);
	check(!get_map_advance() && get_map_set() == 2, "selected release must clear Advanced timer and preserve capture transition");
	begin();
	event(1, BTN_SOUTH, 1); event(1, BTN_EAST, 1);
	now_ms += 6000;
	check(!get_map_advance() && !get_map_cancel(), "multi-button chord must suppress single-button control timers");
	begin();
	tap(1, BTN_EAST); tap(0, KEY_ENTER);
	check(saved.size() == 1 && saved[0].entry.output_codes[0] == KEY_ENTER && !get_map_finish(),
		"Enter must remain capturable as output without finishing the source stage");
}

static void selected_combo_controls()
{
	begin();
	map_to_keyboard();
	input[1].mmap[SYS_BTN_OSD_KTGL + 1] = input[1].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	event(1, BTN_MODE, 1);
	now_ms += 4999;
	check(!get_map_cancel(), "selected Menu hold must not clear early");
	now_ms++;
	check(get_map_cancel(), "selected Menu hold must retain the clear deadline");
	finish_map_setting(1);
	check(cleared == std::vector<int>{1}, "held Menu clear must target the selected device");
	begin();
	map_to_keyboard();
	input[1].mmap[SYS_BTN_OSD_KTGL + 1] = input[1].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	event(1, BTN_MODE, 1);
	now_ms += 100;
	event(1, BTN_MODE, 0);
	check(get_map_finish() && !get_map_cancel(), "quick selected Menu release must retain finish rather than clear");
	finish_map_setting(0);
	check(cleared.empty(), "quick Menu finish must preserve saved mappings");
}

int main()
{
	pending_events(); combo_opener_release(); abort_and_clear(); normalization_and_binding();
	continued_mapping_and_restart(); advanced_edits(); keyboard_and_synthesized_direction();
	chords_and_bounds(); chord_control_keys(); commands_and_timers(); selected_combo_controls();
	if (failures) std::fprintf(stderr, "%d remap checks failed\n", failures);
	else std::printf("PASS: production remap source selection, events, chords, commands and clear scope (%u checks)\n", checks);
	return failures ? 1 : 0;
}
