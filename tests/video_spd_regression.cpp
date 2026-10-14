// The runner extracts intact production functions, including SPI video polling.
// OSD presentation, EDID input and unrelated video configuration are simulated.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "cfg.h"
#include "video.h"

using uint = unsigned int;
#define PROFILE_FUNCTION() do {} while (0)
#include "video_spd_state.inc"

cfg_t cfg = {};
VideoInfo current_video_info = {};
static VideoInfo core_video = {};
static uint16_t resolution_counter = 1;
static uint8_t framebuffer_crc = 1;
static uint32_t menustate = MENU_NONE2;
static uint32_t menu_timer = 0;
static bool minimig_core = false;
static std::array<uint8_t, 256> main_registers = {};
static std::array<uint8_t, 256> packet_registers = {};
static unsigned reads = 0;
static unsigned writes = 0;
static std::deque<uint16_t> spi_reply;
static std::vector<uint16_t> spi_words;

static bool is_menu() { return false; }
static bool is_minimig() { return minimig_core; }
static bool hdmi_has_int() { return true; }
static int arcade_get_direction() { return 0; }
static char *user_io_get_core_name2() { static char name[] = "Regression"; return name; }
static bool is_edid_valid() { return true; }
static int read_edid(bool force = false) { if (force) ++edid_version; return 1; }
static void find_edid_vrr_capability() {}
static int i2c_open(uint8_t address, int, int, int *bus = nullptr)
{
	if (bus) *bus = 0;
	return address;
}
static int i2c_smbus_read_byte_data(int fd, uint8_t reg)
{
	++reads;
	return fd == hdmi_main_fd ? main_registers[reg] : packet_registers[reg];
}
static int i2c_smbus_write_byte_data(int fd, uint8_t reg, uint8_t data)
{
	++writes;
	if (fd == hdmi_main_fd) main_registers[reg] = data;
	else packet_registers[reg] = data;
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
static uint16_t spi_uio_cmd(uint16_t) { return 2; }
static void reply32(uint32_t value)
{
	spi_reply.push_back(value & 0xFFFF);
	spi_reply.push_back(value >> 16);
}
static uint8_t spi_uio_cmd_cont(uint16_t command)
{
	spi_reply.clear();
	if (command == UIO_GET_VRES)
	{
		spi_reply.push_back(resolution_counter);
		for (uint32_t value : {core_video.width, core_video.height, core_video.htime,
		                     core_video.vtime, core_video.ptime, core_video.vtimeh, core_video.ctime}) reply32(value);
		spi_reply.push_back(core_video.pixrep);
		spi_reply.push_back(core_video.de_h);
		spi_reply.push_back(core_video.de_v);
		reply32(core_video.frame_clocks);
	}
	else if (command == UIO_GET_FB_PAR)
	{
		for (uint32_t value : {core_video.arx, core_video.ary, core_video.fb_fmt,
		                     core_video.fb_width, core_video.fb_height}) spi_reply.push_back(value);
		return framebuffer_crc;
	}
	else if (command == UIO_SET_VIDEO) spi_words.clear();
	return 0;
}
static uint16_t spi_w(uint16_t value)
{
	if (!spi_reply.empty())
	{
		uint16_t reply = spi_reply.front();
		spi_reply.pop_front();
		return reply;
	}
	spi_words.push_back(value);
	return 0;
}
static void DisableIO() { spi_reply.clear(); }
static void user_io_send_buttons(int) {}
void video_menu_bg(int, int) {}
bool cfg_has_video_sections() { return false; }
void cfg_parse() {}
static void show_video_info(const VideoInfo *, const vmode_custom_t *) {}
static void set_yc_mode() {}
static void set_vfilter(int) {}
static void video_scaling_adjust(const VideoInfo *, const vmode_custom_t *) {}
static void video_resolution_adjust(const VideoInfo *, vmode_custom_t *) {}
bool video_mode_select(uint32_t, vmode_custom_t *mode) { *mode = v_def; return false; }
static bool pll_request_on_integer(double) { return false; }
static int findPLLpar(double, uint32_t *, uint32_t *, double *, bool) { return 0; }
static void OSD_PrintInfo(const char *, int *, int *, int) {}
static void InfoEnable(int, int, int, int) {}
static int get_vga_fb() { return 0; }
static void OsdSetSize(int) {}
static uint32_t GetTimer(int timeout) { return timeout; }
static void OsdUpdate() {}

#include "video_spd_functions.inc"

static void require(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::exit(1);
	}
}

static void clear_counts() { reads = writes = 0; }
static void record(const char *event)
{
	std::printf("REQUESTS %s reads=%u writes=%u\n", event, reads, writes);
}
static void expect_requests(unsigned expected_reads, unsigned expected_writes, const char *message)
{
	require(reads == expected_reads && writes == expected_writes, message);
}
static std::array<uint8_t, 31> payload()
{
	std::array<uint8_t, 31> result = {};
	std::memcpy(result.data(), packet_registers.data(), result.size());
	return result;
}
static void require_hdr()
{
	require((main_registers[0x40] & 2) != 0, "SPD operation disabled HDR packet");
}

static void start(const std::string &name)
{
	cfg = {};
	cfg.hdr = 2;
	cfg.refresh_min = 48;
	cfg.refresh_max = 60;
	cfg.direct_video = name.find("direct_") == 0 || name.find("rtg_") == 0;
	minimig_core = name.find("rtg_") == 0;
	minimig_rtg_hdmi = minimig_core;
	if (name.find("quirk") != std::string::npos) cfg.spd_quirk = name.back() - '0';
	if (name.find("freesync_") == 0) cfg.vrr_mode = 2;
	if (name.find("vesa_") == 0) cfg.vrr_mode = 3;
	core_video = {};
	core_video.width = 1280;
	core_video.height = 720;
	core_video.htime = 2222;
	core_video.vtime = core_video.vtimeh = 1666667;
	core_video.ptime = 1724;
	core_video.ctime = 1650;
	core_video.arx = 16;
	core_video.ary = 9;
	core_video.fb_fmt = minimig_core ? 0xC0 : 0;
	core_video.fb_width = 1280;
	core_video.fb_height = 720;
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
	current_video_info = core_video;
	hdmi_config_init();
	hdmi_config_set_hdr();
	video_set_mode(&v_def, 0);
	video_mode_adjust(true);
	require(current_video_info.width == 1280 && current_video_info.height == 720,
	        "production video polling did not read fake SPI timing values");
	require_hdr();
	clear_counts();
}

static void test_popup(const std::string &name)
{
	const bool metadata_output = cfg.direct_video;
	const bool sends = metadata_output && cfg.spd_quirk < 2;
	const auto before = payload();
	const auto hdr_before = packet_registers;
	const uint8_t enables_before = main_registers[0x40];
	const bool real_menu = name == "normal_menu";
	if (real_menu) menustate = MENU_SYSTEM1;
	else Info("Regression popup", 2000, 0, 0, 0);
	require(menu_present() == 1, "global menu_present semantics changed for Info/menu");
	video_mode_adjust(false);
	record("entry");
	const unsigned entry_reads = reads;
	const unsigned entry_writes = writes;
	const bool entry_payload_correct = sends ? (packet_registers[7] & 4) != 0 : payload() == before;
	require_hdr();
	clear_counts();
	if (!real_menu) Info("Updated popup", 2000, 0, 0, 0);
	video_mode_adjust(false);
	record("held");
	expect_requests(0, 0, "unchanged overlay state made I2C requests");
	clear_counts();
	menustate = MENU_NONE2; // Same state after HandleUI expires an Info popup.
	require(menu_present() == 0, "global menu_present semantics changed for hidden menu");
	video_mode_adjust(false);
	record("exit");
	require(entry_reads == (sends ? 1u : 0u) && entry_writes == (sends ? 34u : 0u),
	        "menu entry made unexpected I2C requests");
	require(entry_payload_correct, "menu entry failed to preserve/modify the correct SPD payload");
	expect_requests(sends ? 1 : 0, sends ? 34 : 0, "menu exit made unexpected I2C requests");
	require(payload() == before, "menu exit failed to preserve/restore the initial payload");
	require(main_registers[0x40] == enables_before, "menu event changed packet enable masks");
	require(std::memcmp(packet_registers.data() + 0xC0, hdr_before.data() + 0xC0, 64) == 0,
	        "menu event changed VESA/HDR spare packet data");
	clear_counts();
	video_mode_adjust(false);
	expect_requests(0, 0, "hidden stable menu made I2C requests");
}

static void test_refresh(const std::string &name)
{
	const bool resolution = name.find("resolution") != std::string::npos;
	if (resolution)
	{
		++resolution_counter;
		core_video.width = 640;
	}
	video_mode_adjust(!resolution);
	record(resolution ? "resolution" : "forced");
	expect_requests(1, 34, "forced/resolution refresh did not program SPD");
	if (resolution) require(current_video_info.width == 640, "resolution event was not read from SPI");
	require((main_registers[0x40] & 0x40) != 0, "refresh disabled SPD packet");
	require_hdr();
}

static void test_reset()
{
	const auto before = packet_registers;
	const uint8_t enables = main_registers[0x40];
	video_reinit();
	record("reset");
	require(writes > 0, "HDMI re-init did not program registers");
	require(main_registers[0x40] == enables, "HDMI re-init failed to restore packet enables");
	require(std::memcmp(packet_registers.data(), before.data(), 31) == 0, "HDMI re-init failed to restore SPD payload");
	require(std::memcmp(packet_registers.data() + 0xC0, before.data() + 0xC0, 64) == 0,
	        "HDMI re-init failed to restore VESA/HDR payloads");
	require_hdr();
}

static void test_rtg_switch()
{
	// Use actual get_video_info and video_mode_adjust to switch output twice.
	minimig_rtg_hdmi = false;
	core_video.fb_fmt = 0;
	++framebuffer_crc;
	video_mode_adjust(false);
	require(video_is_direct(), "Minimig did not switch to Direct Video");
	clear_counts();
	core_video.fb_fmt = 0x40;
	++framebuffer_crc;
	video_mode_adjust(false);
	record("rtg_enter");
	require(minimig_rtg_hdmi && !video_is_direct(), "Minimig RTG output switch failed");
	require(packet_registers[4] == 'D' && packet_registers[5] == 'V', "RTG metadata was not restored");
	require((main_registers[0x40] & 0x40) != 0, "RTG switch did not restore SPD enable");
	require_hdr();
	clear_counts();
	core_video.fb_fmt = 0;
	++framebuffer_crc;
	video_mode_adjust(false);
	record("rtg_exit");
	require(!minimig_rtg_hdmi && video_is_direct(), "Minimig failed to return to Direct Video");
	require((main_registers[0x40] & 0x40) != 0, "Direct Video return did not restore SPD enable");
	require_hdr();
}

int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	const std::string name = argv[1];
	start(name);
	if (name == "rtg_switch") test_rtg_switch();
	else if (name.find("forced") != std::string::npos || name.find("resolution") != std::string::npos) test_refresh(name);
	else if (name.find("reset") != std::string::npos) test_reset();
	else test_popup(name);
	std::printf("PASS: %s\n", name.c_str());
}
