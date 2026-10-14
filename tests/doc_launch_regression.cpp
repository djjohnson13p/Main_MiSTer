#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <strings.h>
#include <sched.h>
#include <vector>
#include "cfg.h"
#include "doc_launch_types.inc"

cfg_t cfg = {};
static int menustate = MENU_DOC_FILE_SELECTED;
static int ttypid = 0, ttystatus = -1;
static char selPath[1024], Selected_tmp[1024], path_buffer[2100];
static std::string fixture_path, script;
static std::vector<std::string> calls, child_args;
static int maximum_write = 0, saved_size = -1, exit_code = -1;
static int fullpath_calls = 0, save_calls = 0, fork_calls = 0;
static bool clobber_path = false, save_success = true, child = false;
static char padded_empty_extension[2] = {};
const char *getFullPath(const char *)
{
	fullpath_calls++;
	std::memcpy(path_buffer, fixture_path.c_str(), fixture_path.size() + 1);
	return path_buffer;
}
void video_chvt(int v)
{
	if (v != 2) std::abort();
	calls.push_back("video");
	if (clobber_path) std::strcpy(path_buffer, "/media/fat/Clobbered.txt");
}
void video_fb_enable(int v) { if (v != 1) std::abort(); calls.push_back("fb"); }
void vga_nag() { calls.push_back("nag"); }
int fixture_unlink(const char *name) { if (std::strcmp(name, "/tmp/script")) std::abort(); calls.push_back("unlink"); return 0; }
int FileSave(const char *name, void *buffer, int size)
{
	if (std::strcmp(name, "/tmp/script") || size < 0) std::abort();
	save_calls++; saved_size = size; calls.push_back("save");
	script.assign((char *)buffer, (size_t)size);
	return save_success;
}
int fixture_fork() { fork_calls++; calls.push_back("fork"); return child ? 0 : 123; }
int fixture_sched_setaffinity(int process, size_t size, cpu_set_t *set)
{
	if (process || size != sizeof(cpu_set_t) || !CPU_ISSET(0, set) || CPU_COUNT(set) != 1) std::abort();
	calls.push_back("affinity"); return 0;
}
int fixture_setsid() { calls.push_back("setsid"); return 0; }
int fixture_execl(const char *name, const char *arg, ...)
{
	calls.push_back("execl"); child_args.push_back(name);
	va_list arguments; va_start(arguments, arg);
	for (const char *value = arg; value; value = va_arg(arguments, const char *)) child_args.push_back(value);
	va_end(arguments); return -1;
}
[[noreturn]] void fixture_exit(int code) { calls.push_back("exit"); throw code; }

// Measure historical writes without executing its demonstrated overflow.
// The selected legacy command and binary arrays both have 2048 bytes.
int measured_sprintf(char *buffer, const char *format, ...)
{
	va_list arguments, measure; va_start(arguments, format); va_copy(measure, arguments);
	int size = std::vsnprintf(nullptr, 0, format, measure); va_end(measure);
	if (size > maximum_write) maximum_write = size;
	std::vsnprintf(buffer, 2048, format, arguments); va_end(arguments);
	return size;
}
#define sprintf measured_sprintf
#define unlink fixture_unlink
#define fork fixture_fork
#define exit fixture_exit
#define sched_setaffinity fixture_sched_setaffinity
#define setsid fixture_setsid
#define execl fixture_execl
#include "doc_launch_production.inc"
#undef execl
#undef setsid
#undef sched_setaffinity
#undef exit
#undef fork
#undef unlink
#undef sprintf

int main(int argc, char **argv)
{
	if (argc != 8) return 2;
	std::ifstream input(argv[1], std::ios::binary);
	fixture_path.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	if (!input || fixture_path.size() >= sizeof(path_buffer)) return 3;
	std::snprintf(selPath, sizeof(selPath), "%s", fixture_path.c_str());
	cfg.fb_terminal = std::atoi(argv[3]); clobber_path = std::atoi(argv[4]);
	save_success = std::atoi(argv[5]); child = std::atoi(argv[6]);
	try { selected_case(); } catch (int code) { exit_code = code; }
	std::ofstream output(argv[2], std::ios::binary); output.write(script.data(), script.size());
	std::ofstream record(argv[7], std::ios::binary);
	for (const auto &call : calls) record << call << '\0';
	record << "ARGS" << '\0'; for (const auto &arg : child_args) record << arg << '\0';
	int expected_state = cfg.fb_terminal ? menustate == MENU_DOC_FILE_SELECTED_2 : menustate == MENU_DOC_NO_FBTERM;
	std::printf("REPORT %d %d %d %d %d %d %d %d %d %d\n", maximum_write, saved_size, fullpath_calls,
		save_calls, fork_calls, expected_state, std::memcmp(selPath, Selected_tmp, sizeof(selPath)) == 0,
		exit_code, ttystatus, ttypid);
}
