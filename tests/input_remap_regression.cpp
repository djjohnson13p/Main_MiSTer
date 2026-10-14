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
struct input_absinfo { int value; };
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
	operator DeviceFixture *() { return records; }
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
static int state = 2, timeout = 0;
struct PoolFixture { int fd, revents, events; };
static PoolFixture pool[NUMDEV + 1];
#ifndef POLLIN
#define POLLIN 1
#endif
#ifndef EVIOCGRAB
#define EVIOCGRAB 0x40044590
#endif
static int key_states[16];
static bool changed_devices = true, changed_joycon = true;
static int closed_devices = 0, ungrabbed_devices = 0, cleared_autofire = 0;
bool check_devs() { return changed_devices; }
bool process_joycon(int, input_event *, input_absinfo *) { return changed_joycon; }
void clear_autofire(int) { cleared_autofire++; }
int fixture_close(int) { closed_devices++; return 0; }
int fixture_ioctl(int, int, int) { ungrabbed_devices++; return 0; }

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

#define close fixture_close
#define ioctl fixture_ioctl
#include "input_remap_production.inc"
#undef ioctl
#undef close

#if !HAVE_ADVANCED_HANDOFF
// Historical API compatibility only: the old production finish still runs.
void finish_map_setting(int dismiss, bool) { finish_map_setting(dismiss); }
#endif

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
	check(input[1].osd_combo == 0 && release.code == BTN_MODE && release.value == 0,
		"pending combo release must update real combo state without rewriting a clear command");
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

static void unmatched_releases()
{
	for (int opener : {KEY_ENTER, KEY_F12})
	{
		begin();
		// The opener was held before initialization, then released after a source press.
		event(1, BTN_EAST, 1); event(0, opener, 0);
		check(get_map_dev() == 1 && get_map_set() == 1 && get_map_button() == 1
			&& !get_map_finish() && !get_map_cancel() && !get_map_advance(),
			"delayed Enter/F12 opener release must not close a held source capture or invoke controls");
		event(1, BTN_NORTH, 1); event(1, BTN_EAST, 0);
		check(get_map_set() == 2 && store.entry.input_codes[1] == BTN_NORTH,
			"first captured chord member release must advance after a rejected opener release");
		tap(0, KEY_O);
		check(saved.size() == 1 && saved[0].device == 1 && saved[0].entry.input_codes[1] == BTN_NORTH,
			"delayed opener release must not truncate the source chord saved later");
	}

	begin();
	input[0].mmap[SYS_BTN_OSD_KTGL + 1] = input[0].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	input[0].osd_combo = 3;
	event(1, BTN_EAST, 1);
	input_event opener = event(0, BTN_MODE, 0);
	check(input[0].osd_combo == 0 && opener.code == BTN_MODE && get_map_dev() == 1
		&& get_map_set() == 1 && get_map_button() == 1 && !get_map_cancel() && !get_map_finish(),
		"delayed foreign Menu opener release must clear combo bits without clearing selected remaps");
	event(1, BTN_EAST, 0); tap(0, KEY_O);
	check(saved.size() == 1 && saved[0].device == 1 && cleared.empty(),
		"capture must remain usable after delayed Menu opener release");

	begin();
	input[0].mmap[SYS_BTN_OSD_KTGL + 1] = input[0].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	event(0, BTN_MODE, 2);
	now_ms += 5000;
	check(input[0].osd_combo == 3 && get_map_dev() == -1 && !get_map_cancel() && !get_map_advance(),
		"pre-target Menu repeat must retain combo bookkeeping without arming a clear timer");
	event(0, BTN_MODE, 0);
	check(input[0].osd_combo == 0 && get_map_dev() == -1 && !get_map_cancel(),
		"release after pre-target repeat must not create a source or clear command");
	map_to_keyboard();
	check(saved.size() == 1 && saved[0].device == 1,
		"first fresh source must remain available after pre-target Menu repeat");

	begin(); event(1, BTN_EAST, 1);
	event(1, BTN_WEST, 0); event(2, BTN_EAST, 0);
	check(get_map_set() == 1 && get_map_button() == 1 && !get_map_finish(),
		"source capture must reject unmatched codes and matching codes from a different device");
	event(1, BTN_EAST, 0);
	check(get_map_set() == 2, "matching source code/device release must still advance");
	event(0, KEY_O, 1); event(2, KEY_P, 0); event(2, KEY_O, 0);
	check(get_map_set() == 2 && get_map_button() == 1 && saved.empty(),
		"output capture must reject foreign unmatched and same-code releases");
	event(0, KEY_P, 1); event(0, KEY_O, 0);
	check(saved.size() == 1 && saved[0].device == 1 && saved[0].entry.output_codes[0] == KEY_O
		&& saved[0].entry.output_codes[1] == KEY_P,
		"first captured output member release must save the full chord against source identity");

	begin();
	const int low_direction = KEY_EMU + 2;
	const int high_direction = low_direction + 1;
	event(1, BTN_EAST, 1); event(1, low_direction, 0);
	check(get_map_set() == 1 && get_map_button() == 1,
		"unpressed synthesized direction release must not close an existing source capture");
	event(1, high_direction, 1); event(1, high_direction, 0); tap(0, KEY_O);
	check(saved.size() == 1 && saved[0].entry.input_codes[1] == high_direction,
		"accepted synthesized direction code above KEY_MAX must own its release and persist in the chord");

	begin(); event(1, BTN_EAST, 1); event(1, BTN_NORTH, 1); event(1, BTN_EAST, 0);
	check(get_map_set() == 2 && !get_map_button(), "first source member release must clear phase capture count");
	event(0, KEY_O, 1); event(1, BTN_NORTH, 0);
	check(get_map_set() == 2 && get_map_button() == 1 && saved.empty(),
		"leftover source chord release must not save a held output key");
	event(2, KEY_P, 1); event(2, KEY_P, 0);
	check(saved.size() == 1 && saved[0].entry.output_codes[0] == KEY_O && saved[0].entry.output_codes[1] == KEY_P,
		"a captured output member on another keyboard must still finish a multi-keyboard output chord");
	event(1, BTN_WEST, 1); event(0, KEY_O, 0);
	check(get_map_set() == 1 && get_map_button() == 1,
		"previous output member release must not advance the next source phase");
}

static void release_identity_and_reset()
{
	begin();
	input[2].bind = 1;
	input[1].kbdmap[KEY_A] = KEY_B;
	input[0].kbdmap[KEY_Q] = KEY_O;
	event(2, KEY_A, 1); event(3, KEY_B, 0);
	check(get_map_dev() == 1 && get_map_set() == 1 && store.entry.input_codes[0] == KEY_B,
		"source ownership must use canonical bound identity and normalized code");
	event(1, KEY_A, 0);
	check(get_map_set() == 2, "a normalized release through the canonical device must match a bound node press");
	input[3].bind = 0;
	event(3, KEY_Q, 1); event(1, KEY_O, 0);
	check(get_map_set() == 2 && saved.empty() && store.entry.output_codes[0] == KEY_O,
		"normalized output must retain the output event device rather than the mapping source device");
	event(0, KEY_Q, 0);
	check(saved.size() == 1 && saved[0].device == 1 && saved[0].entry.input_codes[0] == KEY_B
		&& saved[0].entry.output_codes[0] == KEY_O,
		"normalized output release through another bound node must preserve saved source identity");

	begin(); event(1, BTN_EAST, 1);
	start_map_setting(3, 0, &store.entry);
	event(1, BTN_EAST, 0);
	check(get_map_dev() == -1 && get_map_set() == 1 && !get_map_button(),
		"dialog restart must reject ownership from the previous capture");
	event(0, KEY_A, 1); event(1, BTN_EAST, 0);
	check(get_map_dev() == 0 && get_map_set() == 1 && get_map_button() == 1,
		"previous dialog release must stay unowned after a different fresh source press");
	event(0, KEY_A, 0); tap(2, KEY_O);
	check(saved.size() == 1 && saved[0].device == 0 && saved[0].entry.output_codes[0] == KEY_O,
		"foreign keyboard output and its own release must remain supported after restart");
	finish_map_setting(0);
	start_map_setting(3, 0, &store.entry);
	event(2, KEY_O, 0); event(3, KEY_B, 1); event(2, KEY_O, 0);
	check(get_map_dev() == 3 && get_map_set() == 1 && get_map_button() == 1,
		"finish and restart must not reuse previous output press ownership");

	begin();
	for (int code : {BTN_EAST, BTN_NORTH, BTN_WEST, KEY_A, KEY_B}) event(1, code, 1);
	event(1, KEY_A, 0);
	check(get_map_set() == 2 && store.entry.input_codes[3] == KEY_B,
		"release of an accepted source member displaced by four-code overflow must still advance");
	for (int code : {KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T}) event(0, code, 1);
	event(0, KEY_R, 0);
	check(saved.size() == 1 && saved[0].entry.input_codes[3] == KEY_B && saved[0].entry.output_codes[3] == KEY_T,
		"release of an accepted overflow-displaced output member must save existing overwrite semantics");
}

static void edited_half_releases_and_timers()
{
	begin();
	store.entry.input_codes[0] = BTN_EAST; store.entry.output_codes[0] = KEY_O;
	start_map_setting(1, 1, &store.entry);
	event(1, BTN_NORTH, 1); event(0, KEY_P, 0); event(2, BTN_NORTH, 0);
	check(!get_map_finish() && !get_map_cancel() && store.entry.input_codes[0] == BTN_NORTH
		&& store.entry.output_codes[0] == KEY_O,
		"count-one input edit must reject foreign releases while retaining the other half");
	event(1, BTN_NORTH, 0);
	check(get_map_finish(), "count-one input edit must finish on its matching captured release");
	finish_map_setting(0);
	start_map_setting(1, 2, &store.entry);
	event(0, KEY_P, 1); event(2, KEY_P, 0);
	check(!get_map_finish() && store.entry.input_codes[0] == BTN_NORTH,
		"count-one output edit must retain source codes and reject wrong-device same-code releases");
	event(0, KEY_P, 0); finish_map_setting(0);
	check(store.entry.output_codes[0] == KEY_P && saved.empty() && cleared.empty(),
		"accepted count-one output must preserve captured data without direct persistence");
	start_map_setting(1, 2, &store.entry);
	event(2, KEY_ESC, 1); event(2, KEY_ESC, 0);
	check(get_map_cancel(), "count-one fresh Esc press/release must retain Clear");
	finish_map_setting(1);
	check(store.entry.input_codes[0] == BTN_NORTH && !store.entry.output_codes[0] && cleared.empty(),
		"count-one Esc Clear must clear only edited output codes");

	begin(); store.entry.input_codes[0] = BTN_EAST; store.entry.output_codes[0] = KEY_O;
	start_map_setting(1, 1, &store.entry);
	input[1].mmap[SYS_BTN_OSD_KTGL + 1] = input[1].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	event(1, BTN_MODE, 1); now_ms += 5000;
	input_event editor_clear = event(1, BTN_MODE, 0);
	check(editor_clear.code == KEY_ESC && get_map_cancel(),
		"count-one owned Menu release must retain its Escape rewrite and clear command");
	finish_map_setting(1);
	check(!store.entry.input_codes[0] && store.entry.output_codes[0] == KEY_O && cleared.empty(),
		"count-one Menu Clear must clear only edited input codes");

	begin(); map_to_keyboard();
	input[1].mmap[SYS_BTN_OSD_KTGL + 1] = input[1].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	input[2].mmap[SYS_BTN_OSD_KTGL + 1] = input[2].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	input[2].osd_combo = 3;
	event(1, BTN_MODE, 1); event(2, BTN_MODE, 0);
	check(input[2].osd_combo == 0 && !get_map_finish() && !get_map_cancel(),
		"foreign unowned Menu release must update its combo without resetting a valid selected Menu timer");
	now_ms += 4999;
	check(!get_map_cancel(), "retained Menu clear timer must not fire early");
	now_ms++;
	check(get_map_cancel(), "rejected foreign release must preserve the genuine five-second Menu clear deadline");
	input_event valid_clear = event(1, BTN_MODE, 0);
	check(valid_clear.code == KEY_ESC && get_map_cancel(),
		"owned Menu release must retain the real prefix Escape rewrite and Clear command");
	finish_map_setting(1);
	check(cleared == std::vector<int>{1}, "owned Menu Clear must preserve the selected target");
}

static void terminal_capture_events()
{
	// input_poll can deliver several queued events before HandleUI calls finish.
	for (int half : {1, 2})
	{
		begin(); store.entry.input_codes[0] = BTN_EAST; store.entry.output_codes[0] = KEY_O;
		start_map_setting(1, half, &store.entry);
		int dev = half == 1 ? 1 : 0;
		int code = half == 1 ? BTN_NORTH : KEY_P;
		tap(dev, code);
		check(get_map_finish() && !get_map_cancel(), "matching count-one release must finish the edited half");
		advancedButtonMap completed = store.entry;
		int completed_count = get_map_button();
		input[2].mmap[SYS_BTN_OSD_KTGL + 1] = input[2].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
		input[2].osd_combo = 3;
		event(2, BTN_MODE, 0); event(2, KEY_A, 1); event(2, KEY_B, 1); event(dev, code, 0);
		check(input[2].osd_combo == 0 && get_map_button() == completed_count
			&& get_map_finish() && !get_map_cancel() && !get_map_advance()
			&& !std::memcmp(&completed, &store.entry, sizeof(completed)),
			"queued events after count-one completion must update combo bits but preserve completed data and Finish");
		finish_map_setting(0);
		start_map_setting(1, half, &store.entry);
		tap(dev, code);
		check(get_map_finish() && (half == 1 ? store.entry.input_codes[0] : store.entry.output_codes[0]) == code,
			"a new count-one capture must reset terminal state after finish");
	}

	begin(); tap(0, KEY_ESC);
	advancedButtonMap canceled = store.entry;
	event(1, BTN_EAST, 1); event(1, BTN_NORTH, 1); event(0, KEY_ESC, 0);
	check(get_map_cancel() && !get_map_finish() && !get_map_advance()
		&& !std::memcmp(&canceled, &store.entry, sizeof(canceled)),
		"queued fresh presses must not append chord data or erase accepted Escape Clear");
	finish_map_setting(1);
	check(cleared == std::vector<int>{0}, "accepted Escape Clear must retain its source target through queued input");

	begin(); event(1, BTN_EAST, 1); event(0, KEY_ENTER, 1);
	advancedButtonMap finished = store.entry;
	input[2].mmap[SYS_BTN_OSD_KTGL + 1] = input[2].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	input[2].osd_combo = 3;
	event(2, BTN_MODE, 0); event(2, KEY_P, 1); event(0, KEY_ENTER, 0);
	check(get_map_finish() && !get_map_cancel() && input[2].osd_combo == 0
		&& !std::memcmp(&finished, &store.entry, sizeof(finished)),
		"queued events after Enter Finish must retain command meaning and captured data");
	finish_map_setting(0);
	check(cleared.empty(), "terminal Finish must retain non-clearing semantics");
}

static void menu_core_escape_compatibility()
{
	// The Menu core exposes Button/Key remap, but its Clear/Advanced getters
	// are disabled. An Esc release must not terminally freeze that dialog.
	begin(); menu_core = true; tap(0, KEY_ESC);
	check(!get_map_cancel() && !get_map_finish() && !get_map_advance(),
		"Menu-core Escape must retain disabled Clear and Advanced commands");
	event(0, KEY_ENTER, 1);
	check(get_map_finish() && !get_map_cancel() && get_map_dev() == 0,
		"Menu-core Escape must leave subsequent Enter Finish available");
	finish_map_setting(0);
	check(cleared.empty(), "Menu-core Enter Finish after Escape must preserve mappings");

	begin(); menu_core = true; tap(0, KEY_ESC); event(0, KEY_F12, 1);
	check(get_map_button() == 2 && store.entry.input_codes[1] == KEY_F12 && !get_map_advance(),
		"Menu-core Escape must permit subsequent chord data without enabling Advanced");
	event(0, KEY_F12, 0); tap(0, KEY_O);
	check(saved.size() == 1 && saved[0].entry.input_codes[0] == KEY_ESC
		&& saved[0].entry.input_codes[1] == KEY_F12,
		"Menu-core capture must remain usable after an Escape release");

	// Branch compatibility probe: normal Menu-core entry cannot open the
	// Advanced editor because get_map_advance is disabled there.
	for (int half : {1, 2})
	{
		begin(); menu_core = true;
		store.entry.input_codes[0] = KEY_A; store.entry.output_codes[0] = KEY_O;
		start_map_setting(1, half, &store.entry); tap(0, KEY_ESC); tap(0, KEY_ENTER);
		check(get_map_finish() && !get_map_cancel() && !get_map_advance()
			&& (half == 1 ? store.entry.input_codes[1] : store.entry.output_codes[1]) == KEY_ENTER,
			"Menu-core count-one branch must keep Escape nonterminal until a subsequent captured release finishes");
		finish_map_setting(0);
	}

	begin(); menu_core = true;
	input[1].mmap[SYS_BTN_OSD_KTGL + 1] = input[1].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	tap(1, BTN_MODE); tap(0, KEY_O);
	check(saved.size() == 1 && saved[0].entry.input_codes[0] == BTN_MODE
		&& !get_map_cancel() && !get_map_advance(),
		"Menu-core matching Menu button release must retain capture data and disabled commands");
}

static void advanced_command_capture()
{
	begin(); event(0, KEY_F12, 1); event(0, KEY_F12, 0);
	check(get_map_advance() && get_map_advance() && get_map_dev() == 0
		&& get_map_button() == 0 && get_map_set() == 1 && !store.entry.input_codes[0],
		"first F12 tap must remain an unconsumed Advanced request, without becoming chord data");
	input[2].mmap[SYS_BTN_OSD_KTGL + 1] = input[2].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
	input[2].osd_combo = 3;
	event(2, BTN_MODE, 0); event(2, KEY_A, 1); event(2, KEY_B, 2); event(2, KEY_B, 0);
	event(0, KEY_F12, 2);
	check(get_map_advance() && !get_map_finish() && !get_map_cancel() && input[2].osd_combo == 0
		&& get_map_dev() == 0 && get_map_button() == 0 && !store.entry.input_codes[0],
		"queued foreign input and released-command repeats must preserve pending Advanced and real combo bookkeeping");
	event(0, KEY_F12, 1); // A second fresh press before UI must rearm the owned release.
	check(get_map_advance() && get_map_button() == 0 && get_map_dev() == 0,
		"second fresh owned F12 before UI must retain the request and selected source");
	finish_map_setting(0, true);
	check(!get_map_advance() && !mapping && saved.empty() && cleared.empty(),
		"explicit Advanced handoff must consume the request without saving or clearing remaps");

	begin(); map_to_keyboard();
	event(2, KEY_F12, 2);
	check(!get_map_advance() && get_map_dev() == 1 && !get_map_button(),
		"selected next-source-phase F12 repeat cannot create an unowned Advanced request");
	input[1].mmap[SYS_BTN_MENU_FUNC] = KEY_F12;
	event(2, KEY_F12, 2); now_ms += 5000;
	check(!get_map_advance(), "F12 repeat must not arm mapped-OK fallback timing even if OK is mapped to F12");
	input[2].mmap[SYS_BTN_OSD_KTGL + 1] = input[2].mmap[SYS_BTN_OSD_KTGL + 2] = KEY_F12;
	event(2, KEY_F12, 1); event(2, KEY_F12, 0);
	check(get_map_dev() == 1 && get_map_advance() && get_map_button() == 0
		&& get_map_set() == 1 && input[2].osd_combo == 0 && saved.size() == 1,
		"Advanced from another keyboard in the next source phase must retain the existing selected pad");
	finish_map_setting(0, true);
	check(cleared.empty() && saved.size() == 1, "Advanced handoff must preserve earlier saved source/output entries");

	begin(); event(0, KEY_F12, 2); event(0, KEY_F12, 0);
	check(get_map_dev() == -1 && !get_map_advance(), "held opener repeat/release cannot create an Advanced request");
	event(0, KEY_A, 1); event(0, KEY_F12, 1); event(0, KEY_F12, 0); tap(2, KEY_O);
	check(saved.size() == 1 && saved[0].entry.input_codes[0] == KEY_A
		&& saved[0].entry.input_codes[1] == KEY_F12 && saved[0].entry.output_codes[0] == KEY_O,
		"second source-member F12 and foreign keyboard output must remain chord data");
	begin(); event(0, KEY_A, 1); event(0, KEY_B, 1); event(0, KEY_F12, 1); event(0, KEY_F12, 0);
	tap(2, KEY_F12);
	check(saved.size() == 1 && saved[0].entry.input_codes[2] == KEY_F12
		&& saved[0].entry.output_codes[0] == KEY_F12 && !get_map_advance(),
		"third source-member and first output-member F12 must remain data");
	for (int half : {1, 2})
	{
		begin(); store.entry.input_codes[0] = KEY_A; store.entry.output_codes[0] = KEY_O;
		start_map_setting(1, half, &store.entry); tap(2, KEY_F12);
		check(get_map_finish() && !get_map_advance()
			&& (half == 1 ? store.entry.input_codes[0] : store.entry.output_codes[0]) == KEY_F12,
			"count-one edited input/output F12 must retain data and normal completion");
		finish_map_setting(0);
	}
	begin(); menu_core = true; tap(0, KEY_F12); tap(2, KEY_O);
	check(saved.size() == 1 && saved[0].entry.input_codes[0] == KEY_F12
		&& saved[0].entry.output_codes[0] == KEY_O && !get_map_advance(),
		"Menu-core first F12 must remain mapping data with Advanced disabled");
}

static void prepare_inventory_fixture()
{
	for (auto &item : pool) item = {-1, 0, 0};
	pool[NUMDEV].revents = POLLIN;
	pool[0].fd = 42;
	changed_devices = changed_joycon = true;
	closed_devices = ungrabbed_devices = cleared_autofire = 0;
	state = 2; timeout = 9;
}

static void advanced_command_lifecycle()
{
	for (int transition : {0, 1, 2})
	for (bool release_first : {false, true})
	{
		begin(); map_to_keyboard(); event(0, KEY_F12, 1);
		if (release_first) event(0, KEY_F12, 0);
		prepare_inventory_fixture();
		if (transition == 0)
		{
			check(inventory_changed() == 0 && state == 1
				&& closed_devices == 1 && ungrabbed_devices == 1,
				"real device-change branch must retain close/ungrab/rebuild flow");
		}
		else if (transition == 1)
		{
			input[1].quirk = QUIRK_JOYCON;
			check(joycon_changed(1) == 0 && state == 1, "real JoyCon branch must retain rebuild return flow");
		}
		else { state = 1; inventory_initialize(); }
		check(get_map_finish() && get_map_dev() == -1 && !get_map_advance() && !get_map_cancel(),
			"actual inventory invalidation must cancel pending Advanced into harmless unselected Finish");
		advancedButtonMap original = store.entry;
		event(2, KEY_A, 1); event(2, KEY_A, 0);
		check(get_map_finish() && !std::memcmp(&original, &store.entry, sizeof(original)),
			"events after inventory invalidation must not restart terminal capture");
		state = 1; inventory_initialize();
		check(get_map_finish() && get_map_dev() == -1 && !get_map_advance(),
			"repeated state-one initialization must preserve harmless Finish without restoring the request");
		check(timeout == 0 && cleared_autofire >= NUMPLAYERS && input[0].osd_combo == 0,
			"real initialization prefix must retain timeout/autofire/device-state reset");
		finish_map_setting(0);
		check(!mapping && cleared.empty() && saved.size() == 1,
			"UI Finish after invalidation must not save or clear a reused device index");
		begin(); tap(1, BTN_NORTH); tap(0, KEY_P);
		check(saved.size() == 1 && saved[0].device == 1,
			"new capture after rebuild must accept normal data without stale terminal state");
	}

	begin(); event(0, KEY_F12, 1); prepare_inventory_fixture(); changed_devices = false;
	check(inventory_changed() == 2 && get_map_advance() && !get_map_finish(),
		"poll notification without a real inventory change must not invalidate Advanced");
	changed_joycon = false; input[1].quirk = QUIRK_JOYCON;
	check(joycon_changed(1) == 2 && get_map_advance(),
		"JoyCon event without a rebuild must not invalidate Advanced");
	state = 2; inventory_initialize();
	check(get_map_advance(), "state-two initialization wrapper must not run a state-one reset");
	finish_map_setting(0);

	for (int dismiss : {0, 1, 2})
	for (bool handoff : {false, true})
	{
		begin(); event(0, KEY_F12, 1); finish_map_setting(dismiss, handoff);
		check(!mapping && !get_map_advance() && (dismiss ? cleared == std::vector<int>{0} : cleared.empty()),
			"default/dismiss finish and explicit handoff must preserve existing source clear semantics");
		start_map_setting(3, 0, &store.entry);
		check(get_map_dev() == -1 && !get_map_advance() && !get_map_finish(),
			"every new capture must clear previous Advanced request and command identity");
	}
	begin(); event(0, KEY_F12, 1); start_map_setting(3, 0, &store.entry);
	event(0, KEY_F12, 0); check(get_map_dev() == -1 && !get_map_advance(),
		"restart before UI must reset pending command without treating its old release as a new target");
	begin(); finish_map_setting(0, true);
	check(!mapping && get_map_dev() == -1 && cleared.empty(), "unselected explicit finish must remain harmless");

	// The new invalidation hooks own only F12 state. Unrelated legacy holds
	// retain their deadlines here; this is not a general hotplug-safety claim.
	for (int transition : {0, 1, 2})
	for (bool menu_hold : {false, true})
	{
		begin(); prepare_inventory_fixture();
		if (menu_hold)
		{
			input[1].mmap[SYS_BTN_OSD_KTGL + 1] = input[1].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
			event(1, BTN_MODE, 1);
		}
		else event(1, BTN_SOUTH, 1);
		now_ms += 4999;
		check(!get_map_advance() && !get_map_cancel(), "unrelated real hold must remain before its deadline");
		if (transition == 0) inventory_changed();
		else if (transition == 1) { input[1].quirk = QUIRK_JOYCON; joycon_changed(1); }
		else { state = 1; inventory_initialize(); }
		check(get_map_dev() == 1 && !get_map_finish() && !get_map_advance() && !get_map_cancel(),
			"inventory hook without F12 state must preserve unrelated capture and pending hold deadline");
		now_ms++;
		check(menu_hold ? get_map_cancel() : get_map_advance(),
			"inventory hook without F12 state must not erase unrelated Menu or mapped-OK elapsed hold");
		finish_map_setting(0);
	}
}

int main()
{
	pending_events(); combo_opener_release(); abort_and_clear(); normalization_and_binding();
	continued_mapping_and_restart(); advanced_edits(); keyboard_and_synthesized_direction();
	chords_and_bounds(); chord_control_keys(); commands_and_timers(); selected_combo_controls();
	unmatched_releases(); release_identity_and_reset(); edited_half_releases_and_timers();
	terminal_capture_events();
	menu_core_escape_compatibility();
	advanced_command_capture(); advanced_command_lifecycle();
	if (failures) std::fprintf(stderr, "%d remap checks failed\n", failures);
	else std::printf("PASS: production remap source selection, release ownership, chords, commands and clear scope (%u checks)\n", checks);
	return failures ? 1 : 0;
}
