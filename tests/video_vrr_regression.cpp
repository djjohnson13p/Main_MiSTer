// The Python runner extracts unchanged production functions into the includes.
// Only platform I/O, EDID input and unrelated video operations are simulated.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "cfg.h"
#include "video.h"

using uint = unsigned int;
#define PROFILE_FUNCTION() do {} while (0)
// The command value is irrelevant to this fake SPI bus.
#define UIO_SET_VIDEO 0x20

#include "video_vrr_state.inc"

cfg_t cfg = {};
VideoInfo current_video_info = {};
static std::array<uint8_t, 256> main_registers = {};
static std::array<uint8_t, 256> packet_registers = {};
static unsigned packet_writes = 0;
static bool menu_core = false;
static bool minimig_core = false;
static bool valid_edid = true;
static bool fresh_edid = true;
static std::vector<uint16_t> spi_words;

static bool is_menu() { return menu_core; }
static bool is_minimig() { return minimig_core; }
static int menu_present() { return menu_core; }
static bool hdmi_has_int() { return true; }
static int arcade_get_direction() { return 0; }
static char *user_io_get_core_name2() { static char name[] = "Regression"; return name; }
static bool is_edid_valid() { return valid_edid; }
static int read_edid(bool force = false)
{
	if (force && fresh_edid) ++edid_version;
	return valid_edid;
}
static void find_edid_vrr_capability() {}
static int i2c_open(uint8_t address, int, int, int *bus = nullptr)
{
	if (bus) *bus = 0;
	return address;
}
static int i2c_smbus_read_byte_data(int fd, uint8_t reg)
{
	return fd == hdmi_main_fd ? main_registers[reg] : packet_registers[reg];
}
static int i2c_smbus_write_byte_data(int fd, uint8_t reg, uint8_t data)
{
	if (fd == hdmi_main_fd) main_registers[reg] = data;
	else
	{
		packet_registers[reg] = data;
		++packet_writes;
	}
	return 0;
}
static void hdmi_config_audio() {}
static void hdmi_config_set_csc() {}
static void setGamma() {}
static void setScaler() {}
static void setShadowMask() {}
static void video_fb_config() {}
static void video_cfg_init() {}
static void video_mode_load(bool = false) {}
static void setPLL(double frequency, vmode_custom_t *mode) { mode->Fpix = frequency; }
static uint16_t spi_uio_cmd(uint16_t) { return 2; } // core supports VRR
static void spi_uio_cmd_cont(uint16_t) { spi_words.clear(); }
static void spi_w(uint16_t word) { spi_words.push_back(word); }
static void DisableIO() {}
static void spd_config_update();
// video_reinit's forced information refresh reaches the real SPD function.
void video_mode_adjust(bool force) { if (force) spd_config_update(); }
void video_menu_bg(int, int) {}
static void user_io_send_buttons(int) {}

#include "video_vrr_functions.inc"

static void require(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::exit(1);
	}
}

static void reset(uint8_t config_mode)
{
	cfg = {};
	cfg.vrr_mode = config_mode;
	cfg.refresh_min = 48;
	cfg.refresh_max = 60;
	cfg.hdr = 2;
	current_video_info = {};
	current_video_info.width = 1280;
	current_video_info.height = 720;
	current_video_info.ctime = 1650;
	current_video_info.vtimeh = 1666667;
	v_cur = {};
	v_def = {};
	v_def.param.hact = 1280;
	v_def.param.hfp = 110;
	v_def.param.hs = 40;
	v_def.param.hbp = 220;
	v_def.param.vact = 720;
	v_def.param.vfp = 5;
	v_def.param.vs = 5;
	v_def.param.vbp = 20;
	v_def.Fpix = 74.25;
	use_vrr = 0;
	use_freesync_spd = 0;
	last_vrr_mode = 0xFF;
	last_vrr_rate = 0;
	last_vrr_vfp = 0;
	last_sync_invert = 0xFF;
	last_pr_flags = 0xFF;
	last_vic_mode = 0xFF;
	menu_core = false;
	minimig_core = false;
	minimig_rtg_hdmi = false;
	valid_edid = true;
	fresh_edid = true;
	main_registers.fill(0);
	packet_registers.fill(0);
	packet_writes = 0;
	for (auto &mode : vrr_modes) mode.available = mode.active = 0;
}

static void start()
{
	hdmi_config_init();
	hdmi_config_set_hdr();
	video_set_mode(&v_def, 0);
	spd_config_update();
}

static void check_packet(uint8_t mask, uint8_t offset, const std::vector<uint8_t> &expected)
{
	require((main_registers[0x40] & mask) != 0, "VRR packet disabled after HDMI re-init");
	require(std::memcmp(packet_registers.data() + offset, expected.data(), expected.size()) == 0,
	        "VRR packet data changed or was replaced by ordinary SPD");
	require((main_registers[0x40] & 2) != 0, "HDR packet was disabled by VRR restore");
	require(!spi_words.empty() && (spi_words[0] & (1 << 14)), "FPGA VRR flag was lost");
}

static void test_reinit(uint8_t config_mode, uint8_t vrr_kind, uint8_t mask, uint8_t offset, size_t length)
{
	reset(config_mode);
	if (config_mode == 1) vrr_modes[vrr_kind].available = 1;
	start();
	require(use_vrr == vrr_kind, "initial VRR selection differs from configured mode");
	const std::vector<uint8_t> expected(packet_registers.begin() + offset, packet_registers.begin() + offset + length);
	check_packet(mask, offset, expected);
	const unsigned writes = packet_writes;
	video_set_mode(&v_def, 0);
	require(packet_writes == writes, "unchanged mode needlessly rewrote packets");
	for (int repeat = 0; repeat < 3; ++repeat)
	{
		video_reinit();
		check_packet(mask, offset, expected);
		const unsigned restored_writes = packet_writes;
		video_set_mode(&v_def, 0);
		require(packet_writes == restored_writes, "cache failed after successful restore");
	}
	std::printf("PASS: mode %u (%s) survives repeated HDMI re-init\n", config_mode, vrr_modes[vrr_kind].description);
}

static void test_skipped_reinit()
{
	reset(2);
	start();
	const unsigned writes = packet_writes;
	fresh_edid = false;
	video_reinit();
	require(packet_writes == writes, "skipped re-init changed a working packet");
	require(last_vrr_mode == 2, "skipped re-init invalidated the cache");
	require((main_registers[0x40] & 0x40) != 0, "skipped re-init disabled FreeSync");
	std::puts("PASS: skipped re-init leaves the working cache and packet intact");
}

static void test_disable_and_reenable(bool menu, bool direct, bool rtg)
{
	reset(2);
	start();
	if (!menu && !direct && !rtg) cfg.vrr_mode = 0;
	menu_core = menu;
	cfg.direct_video = direct;
	minimig_rtg_hdmi = rtg;
	video_reinit();
	require(use_vrr == 0 && use_freesync_spd == 0, "VRR remained active in disabled output");
	require((main_registers[0x40] & 1) == 0, "disabled output kept the VESA packet enabled");
	require(packet_registers[2] == 25, "disabled output did not restore ordinary SPD");
	menu_core = false;
	cfg.direct_video = false;
	minimig_rtg_hdmi = false;
	cfg.vrr_mode = 2;
	video_set_mode(&v_def, 0);
	spd_config_update();
	require(use_vrr == VRR_FREESYNC && packet_registers[2] == 8,
	        "FreeSync could not be re-enabled after leaving disabled output");
	require((main_registers[0x40] & 0x40) != 0, "re-enabled FreeSync packet was disabled");
	std::printf("PASS: disable/re-enable transition (menu=%d direct=%d rtg=%d)\n", menu, direct, rtg);
}

int main(int argc, char **argv)
{
	if (argc == 2 && std::strcmp(argv[1], "freesync") == 0)
	{
		test_reinit(2, VRR_FREESYNC, 0x40, 0, 12);
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "vesa") == 0)
	{
		test_reinit(3, VRR_VESA, 1, 0xC0, 14);
		return 0;
	}
	test_reinit(2, VRR_FREESYNC, 0x40, 0, 12);
	test_reinit(3, VRR_VESA, 1, 0xC0, 14);
	test_reinit(1, VRR_FREESYNC, 0x40, 0, 12);
	test_reinit(1, VRR_VESA, 1, 0xC0, 14);
	test_skipped_reinit();
	test_disable_and_reenable(false, false, false);
	test_disable_and_reenable(true, false, false);
	test_disable_and_reenable(false, true, false);
	test_disable_and_reenable(false, false, true);
	std::puts("All 9 VRR regression cases passed (simulated I2C/SPI; no display hardware).");
}
