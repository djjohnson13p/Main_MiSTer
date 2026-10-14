// External finite observations over unchanged extracted production bodies.
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <vector>
#include "startup_globals.inc"
#include "startup_validator.inc"

struct FixtureStop { std::string kind; int code; };
struct FixtureUnsafeCopy { std::string destination; size_t bytes; size_t capacity; };
struct FixtureFormatter { int returned, libc_return; size_t capacity; std::string published, format, root, payload, complete_target, before, after_libc; bool negative_fault; };
struct FixtureSpec {
    std::string name, path, xml, descriptor, root, expected_default, complete_target;
    bool xml_present = false, exists = false, ready = true, loop_ready = true;
    int argc = 2;
    std::string expected_guard;
    int negative_formatter_call = 0;
    bool last_default_rejected = false;
    int expected_disable_osd = 0;
};
FixtureSpec fixture_spec;
std::vector<std::string> fixture_calls, fixture_queries, fixture_preparses, fixture_prints, fixture_errors, fixture_core_names;
std::vector<std::string> fixture_main_stderr;
std::vector<FixtureFormatter> fixture_formats;
size_t fixture_spi_position = 0, fixture_spi_reads = 0;
int fixture_ready_calls = 0, fixture_poll_calls = 0, fixture_copy_core = 0, fixture_copy_rbf = 0;
int fixture_root_calls = 0;
int fixture_checks = 0, fixture_failures = 0;
int fixture_observed_unsafe_guards = 0;
bool fixture_in_init = false;
std::string fixture_guard;
size_t fixture_guard_bytes = 0, fixture_guard_capacity = 0;
std::string fixture_frontier;
int fixture_main_return = -999;
int fixture_exit_code = -999;
const char *fixture_expected_path = nullptr, *fixture_expected_xml = nullptr;
const char *fixture_validated_path = nullptr, *fixture_validated_xml = nullptr;
int fixture_validator_calls = 0;
bool fixture_validator_before_runtime = false, fixture_validator_result = false;

void fixture_check(bool value, const char *message)
{
    fixture_checks++;
    if (!value) { fixture_failures++; fixture_errors.push_back(std::string("ASSERT: ") + message); }
}
void fixture_call(const char *name) { fixture_calls.emplace_back(name); }
void fixture_outside(const char *name) { throw std::runtime_error(std::string("Excluded lower option branch: ") + name); }
std::string fixture_format_string(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int size = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (size < 0 || size > 131072) throw std::runtime_error("Fixture log formatter limit");
    std::vector<char> text(static_cast<size_t>(size) + 1);
    std::vsnprintf(text.data(), text.size(), format, args);
    return std::string(text.data(), static_cast<size_t>(size));
}
int fixture_printf(const char *format, ...)
{
    va_list args; va_start(args, format);
    std::string text = fixture_format_string(format, args); va_end(args);
    fixture_prints.push_back(text);
    return static_cast<int>(text.size());
}
int fixture_fprintf(FILE *stream, const char *format, ...)
{
    if (stream != stderr) fixture_outside("non-stderr fprintf");
    va_list args; va_start(args, format);
    std::string text = fixture_format_string(format, args); va_end(args);
    fixture_main_stderr.push_back(text);
    return static_cast<int>(text.size());
}
template <size_t N> char *fixture_strcpy(char (&destination)[N], const char *source)
{
    if (!source) throw std::runtime_error("Unexpected NULL strcpy source");
    char *pointer = destination;
    std::string kind = pointer == core_path ? "core_path" : pointer == rbf_path ? "rbf_path" : "parser-local";
    size_t needed = std::strlen(source) + 1;
    if (needed > N) throw FixtureUnsafeCopy{kind, needed, N};
    if (pointer == core_path) { fixture_call("copy-core_path"); fixture_copy_core++; }
    if (pointer == rbf_path) { fixture_call("copy-rbf_path"); fixture_copy_rbf++; }
    std::memcpy(destination, source, needed);
    return destination;
}
int fixture_snprintf(char *destination, size_t capacity, const char *format, ...)
{
    if (destination == defmra && std::strcmp(format, "%s%s") && std::strcmp(format, "%s/_Arcades/%s"))
        throw std::runtime_error("Unexpected DEFMRA format: varargs not read");
    va_list args; va_start(args, format);
    std::string root, payload, complete;
    std::string before;
    if (destination == defmra) {
        before.assign(defmra, sizeof(defmra));
        va_list capture; va_copy(capture, args);
        root = va_arg(capture, const char *);
        payload = va_arg(capture, char *);
        va_end(capture);
        complete = root + (std::string(format) == "%s%s" ? "" : "/_Arcades/") + payload;
    }
    int result = std::vsnprintf(destination, capacity, format, args); va_end(args);
    if (destination == defmra) {
        fixture_check(result >= 0 && static_cast<size_t>(result) == complete.size() && capacity == sizeof(defmra), "actual formatter complete return and destination extent");
        fixture_check(std::string(destination) == complete.substr(0, capacity-1), "actual bounded formatter published prefix");
        bool negative = fixture_spec.negative_formatter_call == static_cast<int>(fixture_formats.size()) + 1;
        int effective_return = negative ? -1 : result;
        fixture_formats.push_back({effective_return, result, capacity, destination, format, root, payload, complete, before, std::string(defmra, sizeof(defmra)), negative});
        return effective_return;
    }
    return result;
}
[[noreturn]] void fixture_exit(int code) { throw FixtureStop{"exit", code}; }
int fixture_unlink(const char *path) { fixture_check(std::string(path) == "/tmp/GAMEID", "actual init cleanup path"); fixture_call("unlink"); return 0; }
void a2065_stop() { fixture_call("a2065-stop"); }
void next_enet_stop() { fixture_call("next-stop"); }
void sun_enet_stop() { fixture_call("sun-stop"); }
void offload_start() { fixture_call("offload"); }
void fpga_io_init() { fixture_call("fpga-init"); }
int is_fpga_ready(int mode) { fixture_check(mode == 1, "main readiness mode"); fixture_call("ready"); return (++fixture_ready_calls == 1) ? fixture_spec.ready : fixture_spec.loop_ready; }
void FindStorage() { fixture_call("storage"); }
void fpga_wait_to_reset() { fixture_call("wait-reset"); }
void frame_timer() { fixture_call("frame-timer"); }
void input_poll(int mode) { fixture_check(mode == 0, "main input poll mode"); fixture_call("input-poll"); }
void HandleUI() { fixture_call("HandleUI"); }
void OsdUpdate() { fixture_call("OsdUpdate"); }
void user_io_poll() { fixture_call("user-io-poll"); if (++fixture_poll_calls == 2) throw FixtureStop{"poll-frontier", 0}; }
void scheduler_init() { fixture_call("scheduler-init"); }
void scheduler_run() { fixture_call("scheduler-run"); throw FixtureStop{"scheduler-frontier", 0}; }
template <typename T> int fixture_sched_setaffinity(int pid, size_t size, const T *mask)
{
    fixture_check(pid == 0 && size == sizeof(T) && mask, "affinity spy actual parameters"); fixture_call("affinity"); return 0;
}
// These unique adapters apply only while compiling the actual main on MSYS.
// Linux keeps its native CPU mask declarations and macros; no native types are redeclared.
struct FixtureCpuSet { unsigned long bits[16]; };
void fixture_cpu_zero(FixtureCpuSet *mask) { std::memset(mask, 0, sizeof(*mask)); }
void fixture_cpu_set(int bit, FixtureCpuSet *mask) { if (bit != 1) fixture_outside("CPU_SET"); mask->bits[0] |= 1UL << bit; }
void spi_uio_cmd_cont(int command) { fixture_check(command == UIO_GET_STRING, "actual descriptor SPI command"); fixture_call("descriptor-SPI-start"); }
char spi_in()
{
    fixture_spi_reads++;
    if (fixture_spi_position >= fixture_spec.descriptor.size()) return 0;
    return fixture_spec.descriptor[fixture_spi_position++];
}
void DisableIO() { fixture_call("descriptor-SPI-stop"); }
const char *getRootDir() { fixture_root_calls++; fixture_call("descriptor-root"); return fixture_spec.root.c_str(); }
int FileExists(const char *path, int use_zip)
{
    fixture_check(use_zip == 1, "FileExists argument from actual extracted header default");
    fixture_queries.emplace_back(path);
    // Explicit finite regular-file response. No actual file is opened/queried.
    return fixture_spec.exists && fixture_queries.back() == fixture_spec.expected_default && !fixture_queries.back().empty() && fixture_queries.back().back() != '/';
}
void arcade_pre_parse(const char *path) { fixture_preparses.emplace_back(path); }
void user_io_read_core_name() { fixture_call("read-core-name"); }
void OsdCoreNameSet(const char *name) { fixture_core_names.emplace_back(name); }
const char *OsdCoreNameGet() { return "CORE"; }
const char *user_io_get_core_name() { return "CORE"; }
int substrcpy(char *, const char *, int) { fixture_outside("substrcpy"); return 0; }
int user_io_status_bits(const char *, int *, int *, int = 0, int = 0) { fixture_outside("status_bits"); return 0; }
uint32_t user_io_status_get(const char *, int = 0) { fixture_outside("status_get"); return 0; }
void set_emu_mode(int) { fixture_outside("set_emu_mode"); }
void x86_set_fdd_boot(int) { fixture_outside("x86_set_fdd_boot"); }
int FileLoadConfig(const char *, void *, int) { fixture_outside("FileLoadConfig"); return 0; }
void StoreIdx_F(int, const char *) { fixture_outside("StoreIdx_F"); }
void StoreIdx_S(int, const char *) { fixture_outside("StoreIdx_S"); }
unsigned char user_io_ext_idx(char *, char *) { fixture_outside("ext_idx"); return 0; }
#ifdef FIXTURE_REAL_TX_ENUM
UserIoFileTxResult user_io_file_tx_checked(const char *, unsigned char, char, char, char, uint32_t) { fixture_outside("file_tx_checked"); return UserIoFileTxResult::RejectedBeforeDownload; }
#endif
int user_io_file_tx(const char *, unsigned char, char, char, char = 0, uint32_t = 0) { fixture_outside("file_tx"); return 0; }
void user_io_file_mount(const char *, int) { fixture_outside("file_mount"); }
void user_io_set_index(int) { fixture_outside("set_index"); }
void x86_set_image(int, const char *) { fixture_outside("x86_image"); }
void mcd_set_image(int, const char *) { fixture_outside("mcd_image"); }
void pcecd_set_image(int, const char *) { fixture_outside("pcecd_image"); }
void marty_set_image(int, const char *) { fixture_outside("marty_image"); }
void game_docs_init(const char *, uint32_t) { fixture_outside("game_docs"); }
void cheats_init(const char *, uint32_t) { fixture_outside("cheats"); }
int is_x86() { return 0; } int is_pcxt() { return 0; } int is_cpc() { return 0; }
int is_uneon() { return 0; } int is_megacd() { return 0; } int is_pce() { return 0; }
int is_marty() { return 0; } int is_c128() { return 0; } int is_gba() { return 0; }
void user_io_init(const char *, const char *);
#ifdef FIXTURE_REAL_INIT_VALIDATOR
bool fixture_observed_validator(const char *path, const char *xml)
{
    fixture_validator_calls++;
    fixture_validated_path = path; fixture_validated_xml = xml;
    fixture_validator_before_runtime = fixture_calls.empty();
    fixture_check(fixture_expected_path ? path == fixture_expected_path : path && !path[0], "main validator binds original path argument");
    fixture_check(xml == fixture_expected_xml, "main validator binds original optional XML pointer");
    fixture_validator_result = user_io_init_paths_valid(path, xml);
    return fixture_validator_result;
}
#define user_io_init_paths_valid fixture_observed_validator
#endif

#define printf fixture_printf
#define fprintf fixture_fprintf
#define strcpy fixture_strcpy
#define snprintf fixture_snprintf
#define exit fixture_exit
#define unlink fixture_unlink
#define sched_setaffinity fixture_sched_setaffinity
#ifndef __linux__
#define cpu_set_t FixtureCpuSet
#ifdef CPU_ZERO
#undef CPU_ZERO
#endif
#ifdef CPU_SET
#undef CPU_SET
#endif
#define CPU_ZERO fixture_cpu_zero
#define CPU_SET fixture_cpu_set
#endif
#define main fixture_actual_main
#include "startup_production.inc"
#undef main
#ifdef FIXTURE_REAL_INIT_VALIDATOR
#undef user_io_init_paths_valid
#endif
#undef printf
#undef fprintf
#undef strcpy
#undef snprintf
#undef exit
#undef unlink
#undef sched_setaffinity
#ifndef __linux__
#undef cpu_set_t
#undef CPU_ZERO
#undef CPU_SET
#endif

void user_io_init(const char *path, const char *xml)
{
    fixture_check(fixture_expected_path ? path == fixture_expected_path : path && !path[0], "init receives unchanged original path pointer");
    fixture_check(xml == fixture_expected_xml, "init receives unchanged optional XML pointer");
#ifdef FIXTURE_REAL_INIT_VALIDATOR
    fixture_check(path == fixture_validated_path && xml == fixture_validated_xml, "init receives exactly the pair validated by actual main");
#endif
    fixture_call("selected-init-intervals"); fixture_in_init = true;
    fixture_selected_init_intervals(path, xml);
    fixture_in_init = false;
}
std::string fixture_quote(const std::string &value)
{
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32) { char b[7]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
std::string fixture_array(const std::vector<std::string> &items)
{
    std::string out = "[";
    for (size_t i = 0; i < items.size(); i++) { if (i) out += ','; out += fixture_quote(items[i]); }
    return out + ']';
}
std::string fixture_legal_path(size_t bytes)
{
    std::string out;
    while (bytes - out.size() > 240) { out += std::string(240, 'a'); out += '/'; }
    out += std::string(bytes - out.size(), 'b');
    if (bytes >= 4) out.replace(bytes - 4, 4, ".mra");
    return out;
}
FixtureSpec fixture_case(std::string name, std::string path = "CORE.rbf", bool present = false, std::string xml = "", std::string descriptor = "CORE;;X;", std::string root = "/media/fat", bool exists = false, std::string published = "", std::string complete = "")
{
    FixtureSpec item; item.name = name; item.path = path; item.xml_present = present; item.xml = xml;
    item.descriptor = descriptor; item.root = root; item.exists = exists; item.expected_default = published; item.complete_target = complete;
    if (descriptor == "CORE;;X;" || descriptor == "DEFMRA,A.mra;;X;" || descriptor == "CORE;DEFMRA,A.mra;X;" || descriptor == "CORE;;XDEFMRA,A.mra;" || descriptor == "CORE;;DEFMRA,A.mra;DEFMRA,B.mra;X;") item.expected_disable_osd = 1;
    if ((present ? xml.size() : path.size()) > 1023) item.expected_guard = "core_path";
    else if (path.size() > 1023) item.expected_guard = "rbf_path";
    return item;
}
std::vector<FixtureSpec> fixture_catalog()
{
    std::vector<FixtureSpec> v;
    auto add = [&](FixtureSpec value) { v.push_back(value); };
    auto short_default = [&](const char *name, const char *root, const char *payload) {
        std::string target = std::string(root) + (payload[0] == '/' ? "" : "/_Arcades/") + payload;
        add(fixture_case(name, "CORE.rbf", false, "", std::string("CORE;;DEFMRA,") + payload + ';', root, true, target, target));
    };
    FixtureSpec noargs = fixture_case("main-no-arguments"); noargs.argc = 1; noargs.path = ""; add(noargs);
    add(fixture_case("main-empty-path", "")); add(fixture_case("main-short-path"));
    add(fixture_case("main-explicit-mra", "CORE.rbf", true, "games/A.mra"));
    add(fixture_case("main-explicit-mgl", "CORE.rbf", true, "games/A.mgl"));
    add(fixture_case("main-explicit-empty-xml", "CORE.rbf", true, ""));
    const std::string fit = fixture_legal_path(1023), big = fixture_legal_path(1024);
    add(fixture_case("main-path1023-nullxml", fit)); add(fixture_case("main-path1023-shortxml", fit, true, "A.mra"));
    add(fixture_case("main-shortpath-xml1023", "CORE.rbf", true, fit)); add(fixture_case("main-both1023", fit, true, fit));
    add(fixture_case("main-path1024-nullxml-first-guard", big)); add(fixture_case("main-path1024-shortxml-second-guard", big, true, "A.mra"));
    add(fixture_case("main-shortpath-xml1024-first-guard", "CORE.rbf", true, big)); add(fixture_case("main-both1024-first-guard", big, true, big));
    FixtureSpec notready = noargs; notready.name = "main-noargs-fpga-notready"; notready.ready = false; add(notready);
    notready = fixture_case("main-path1024-fpga-notready", big); notready.ready = false; add(notready);
    FixtureSpec wait = fixture_case("main-ready-then-loop-notready"); wait.loop_ready = false; add(wait);
    FixtureSpec extra = fixture_case("main-extra-argument-ignored", "CORE.rbf", true, "A.mra"); extra.argc = 4; add(extra);
    add(fixture_case("main-percent-utf8-spelling", "games/100%_caf\xc3\xa9.rbf", true, "games/100%_caf\xc3\xa9.mra"));
    short_default("default-fat-relative-short", "/media/fat", "games/A.mra"); short_default("default-fat-leading-short", "/media/fat", "/games/A.mra");
    short_default("default-usb-relative-short", "/media/usb0", "games/A.mra"); short_default("default-usb-leading-short", "/media/usb0", "/games/A.mra");
    add(fixture_case("default-none")); add(fixture_case("default-absent-file", "CORE.rbf", false, "", "CORE;;DEFMRA,A.mra;", "/media/fat", false, "/media/fat/_Arcades/A.mra", "/media/fat/_Arcades/A.mra"));
    add(fixture_case("default-explicit-xml-suppresses", "CORE.rbf", true, "Explicit.mra", "CORE;;DEFMRA,A.mra;", "/media/fat", true, "/media/fat/_Arcades/A.mra"));
    add(fixture_case("default-explicit-emptyxml-suppresses", "CORE.rbf", true, "", "CORE;;DEFMRA,A.mra;", "/media/fat", true, "/media/fat/_Arcades/A.mra"));
    std::string over = fixture_legal_path(1004), clipped = ("/media/fat/_Arcades/" + over).substr(0, 1023);
    add(fixture_case("default-explicitxml-clipped-default-suppresses", "CORE.rbf", true, "Explicit.mra", "CORE;;DEFMRA," + over + ';', "/media/fat", true, clipped, "/media/fat/_Arcades/" + over));
    const char *names[] = {"default-index0-ignored", "default-index1-ignored", "default-lowercase-ignored", "default-mixedcase-ignored", "default-prefix-ignored", "default-missingcomma-ignored", "default-maskedprefix-ignored", "default-DIP-prefix-ignored", "default-X-prefix-ignored"};
    const char *descriptors[] = {"DEFMRA,A.mra;;X;", "CORE;DEFMRA,A.mra;X;", "CORE;;defmra,A.mra;", "CORE;;Defmra,A.mra;", "CORE;;DEFMRAx,A.mra;", "CORE;;DEFMRA;", "CORE;;H0DEFMRA,A.mra;", "CORE;;DIPDEFMRA,A.mra;", "CORE;;XDEFMRA,A.mra;"};
    for (size_t i = 0; i < 9; i++) add(fixture_case(names[i], "CORE.rbf", false, "", descriptors[i]));
    add(fixture_case("default-valid-then-valid", "CORE.rbf", false, "", "CORE;;DEFMRA,A.mra;DEFMRA,B.mra;", "/media/fat", true, "/media/fat/_Arcades/B.mra"));
    add(fixture_case("default-valid-then-oversize", "CORE.rbf", false, "", "CORE;;DEFMRA,A.mra;DEFMRA," + over + ';', "/media/fat", true, clipped, "/media/fat/_Arcades/" + over));
    add(fixture_case("default-oversize-then-valid", "CORE.rbf", false, "", "CORE;;DEFMRA," + over + ";DEFMRA,B.mra;", "/media/fat", true, "/media/fat/_Arcades/B.mra"));
    add(fixture_case("default-valid-then-empty", "CORE.rbf", false, "", "CORE;;DEFMRA,A.mra;DEFMRA,;", "/media/fat", false, "/media/fat/_Arcades/"));
    add(fixture_case("default-empty-payload", "CORE.rbf", false, "", "CORE;;DEFMRA,;", "/media/fat", false, "/media/fat/_Arcades/"));
    add(fixture_case("default-empty-descriptor", "CORE.rbf", false, "", ""));
    add(fixture_case("default-empty-index2-stops-later", "CORE.rbf", false, "", "CORE;;;DEFMRA,A.mra;"));
    add(fixture_case("default-semicolon-field-order", "CORE.rbf", false, "", "CORE;;DEFMRA,A.mra;DEFMRA,B.mra;X;", "/media/fat", true, "/media/fat/_Arcades/B.mra"));
    short_default("default-percent-utf8-spelling", "/media/fat", "games/100%_caf\xc3\xa9.mra");
    short_default("default-leading-double-slash", "/media/fat", "//games/A.mra");
    for (std::string root_name : {"fat", "usb0"}) for (std::string kind : {"relative", "leading"}) for (size_t size : {1023U, 1024U}) for (bool exists : {true, false}) {
        std::string root = "/media/" + root_name;
        std::string prefix = root + (kind == "relative" ? "/_Arcades/" : "");
        size_t payload_size = size - prefix.size();
        std::string payload = kind == "leading" ? "/" + fixture_legal_path(payload_size - 1) : fixture_legal_path(payload_size);
        std::string target = prefix + payload;
        std::string name = "default-" + root_name + '-' + kind + '-' + std::to_string(size) + '-' + (exists ? "present" : "absent");
        add(fixture_case(name, "CORE.rbf", false, "", "CORE;;DEFMRA," + payload + ';', root, exists, target.substr(0, 1023), target));
    }
    std::string item2048 = fixture_legal_path(2041), item2049 = fixture_legal_path(2042);
    std::string published2048 = ("/media/fat/_Arcades/" + item2048).substr(0, 1023);
    add(fixture_case("descriptor-item2048", "CORE.rbf", false, "", "CORE;;DEFMRA," + item2048 + ';', "/media/fat", true, published2048, "/media/fat/_Arcades/" + item2048));
    add(fixture_case("descriptor-item2049", "CORE.rbf", false, "", "CORE;;DEFMRA," + item2049 + ';', "/media/fat", true, ("/media/fat/_Arcades/" + item2049.substr(0, 2041)).substr(0, 1023), "/media/fat/_Arcades/" + item2049.substr(0, 2041)));
    add(fixture_case("descriptor-item2049-next-valid", "CORE.rbf", false, "", "CORE;;DEFMRA," + item2049 + ";DEFMRA,B.mra;", "/media/fat", true, "/media/fat/_Arcades/B.mra"));
    for (size_t size : {10239U, 10240U}) {
        std::string payload = fixture_legal_path(size - 13);
        add(fixture_case("descriptor-spi" + std::to_string(size), "CORE.rbf", false, "", "CORE;;DEFMRA," + payload, "/media/fat", true, ("/media/fat/_Arcades/" + payload.substr(0, 2041)).substr(0, 1023), "/media/fat/_Arcades/" + payload.substr(0, 2041)));
    }
    add(fixture_case("descriptor-default-after-spi-cutoff", "CORE.rbf", false, "", "CORE;;" + std::string(10233, 'q') + ";DEFMRA,B.mra;"));
    FixtureSpec negative = fixture_case("formatter-negative-first", "CORE.rbf", false, "", "CORE;;DEFMRA,A.mra;", "/media/fat", true, "/media/fat/_Arcades/A.mra");
    negative.negative_formatter_call = 1; negative.last_default_rejected = true; add(negative);
    negative = fixture_case("formatter-negative-later-retires-valid", "CORE.rbf", false, "", "CORE;;DEFMRA,A.mra;DEFMRA,B.mra;", "/media/fat", true, "/media/fat/_Arcades/B.mra");
    negative.negative_formatter_call = 2; negative.last_default_rejected = true; add(negative);
    negative = fixture_case("formatter-negative-then-fitting-recovery", "CORE.rbf", false, "", "CORE;;DEFMRA,A.mra;DEFMRA,B.mra;", "/media/fat", true, "/media/fat/_Arcades/B.mra");
    negative.negative_formatter_call = 1; add(negative);
    add(fixture_case("recovery-valid-oversize-then-fitting", "CORE.rbf", false, "", "CORE;;DEFMRA,A.mra;DEFMRA," + over + ";DEFMRA,B.mra;", "/media/fat", true, "/media/fat/_Arcades/B.mra"));
    std::string exact_fit = fixture_legal_path(1003);
    add(fixture_case("recovery-oversize-then-exact1023", "CORE.rbf", false, "", "CORE;;DEFMRA," + over + ";DEFMRA," + exact_fit + ';', "/media/fat", true, "/media/fat/_Arcades/" + exact_fit));
    return v;
}
void fixture_reset(FixtureSpec spec)
{
    fixture_spec = spec; fixture_calls.clear(); fixture_queries.clear(); fixture_preparses.clear(); fixture_prints.clear(); fixture_errors.clear(); fixture_main_stderr.clear(); fixture_core_names.clear(); fixture_formats.clear();
    fixture_spi_position = fixture_spi_reads = 0; fixture_ready_calls = fixture_poll_calls = fixture_copy_core = fixture_copy_rbf = fixture_root_calls = 0;
    fixture_in_init = false; fixture_guard.clear(); fixture_frontier.clear(); fixture_main_return = fixture_exit_code = -999; fixture_guard_bytes = fixture_guard_capacity = 0;
    std::memset(core_path, 'K', sizeof(core_path)); core_path[sizeof(core_path)-1] = 0;
    std::memset(rbf_path, 'R', sizeof(rbf_path)); rbf_path[sizeof(rbf_path)-1] = 0;
    std::memset(defmra, 0, sizeof(defmra)); std::memset(cfgstr, 0, sizeof(cfgstr));
    is_arcade_type = 0; ss_base = ss_size = 0; disable_osd = 0; use_cheats = 0;
    fixture_expected_path = fixture_expected_xml = fixture_validated_path = fixture_validated_xml = nullptr;
    fixture_validator_calls = 0; fixture_validator_before_runtime = fixture_validator_result = false;
}
std::string fixture_hex(const std::string &bytes)
{
    const char digits[] = "0123456789abcdef";
    std::string out; out.reserve(bytes.size()*2);
    for (unsigned char byte : bytes) { out += digits[byte >> 4]; out += digits[byte & 15]; }
    return out;
}
void fixture_execute(FixtureSpec spec)
{
    fixture_reset(spec);
    int before_checks = fixture_checks, before_failed = fixture_failures;
    std::string core_before(core_path, sizeof(core_path)), rbf_before(rbf_path, sizeof(rbf_path));
    std::string default_before(defmra, sizeof(defmra)), descriptor_before(cfgstr, sizeof(cfgstr));
    std::vector<std::string> argv_strings = {"fixture-main", spec.path, spec.xml, "ignored-extra"};
    std::vector<char *> argv; for (std::string &value : argv_strings) argv.push_back(&value[0]); argv.push_back(nullptr);
    int argc = spec.xml_present ? std::max(3, spec.argc) : spec.argc;
    fixture_expected_path = argc > 1 ? argv[1] : nullptr;
    fixture_expected_xml = argc > 2 ? argv[2] : nullptr;
    try { fixture_main_return = fixture_actual_main(argc, argv.data()); fixture_frontier = "observed-main-return"; }
    catch (const FixtureUnsafeCopy &copy) { fixture_observed_unsafe_guards++; fixture_guard = copy.destination; fixture_guard_bytes = copy.bytes; fixture_guard_capacity = copy.capacity; fixture_frontier = "legacy-pre-copy-guard"; }
    catch (const FixtureStop &stop) { fixture_frontier = stop.kind; if (stop.kind == "exit") fixture_exit_code = stop.code; }
    catch (const std::exception &error) { fixture_errors.push_back(error.what()); fixture_failures++; fixture_frontier = "unexpected-lower-guard"; }
    bool invalid = spec.path.size() >= sizeof(rbf_path) || (spec.xml_present && spec.xml.size() >= sizeof(core_path));
#ifdef FIXTURE_REAL_INIT_VALIDATOR
    fixture_check(fixture_validator_calls == 1 && fixture_validator_before_runtime && fixture_validator_result == !invalid, "actual main invokes actual validator before runtime work");
#else
    fixture_check(fixture_validator_calls == 0, "historical absent helper has no synthetic substitute");
#endif
    if (invalid) {
        fixture_check(fixture_frontier == "observed-main-return" && fixture_main_return == EXIT_FAILURE && fixture_exit_code == -999, "invalid actual main returns EXIT_FAILURE, no exit/guard/frontier return inferred");
        fixture_check(fixture_calls.empty() && fixture_poll_calls == 0, "invalid main before every runtime endpoint");
        fixture_check(fixture_prints.empty(), "invalid main has no startup stdout");
        fixture_check(fixture_main_stderr == std::vector<std::string>{"Core or XML startup path is too long or invalid.\n"}, "invalid main exact literal stderr once");
        fixture_check(std::string(core_path, sizeof(core_path)) == core_before && std::string(rbf_path, sizeof(rbf_path)) == rbf_before && std::string(defmra, sizeof(defmra)) == default_before && std::string(cfgstr, sizeof(cfgstr)) == descriptor_before, "invalid main leaves all four source arrays byte-exact");
        fixture_check(fixture_spi_reads == 0 && fixture_formats.empty() && fixture_queries.empty() && fixture_preparses.empty(), "invalid main never reaches descriptor, formatter, query or preparse");
#ifndef FIXTURE_REAL_INIT_VALIDATOR
        if (spec.ready) {
            fixture_check(fixture_frontier == "legacy-pre-copy-guard" && fixture_guard == spec.expected_guard && fixture_guard_bytes == 1025 && fixture_guard_capacity == 1024, "historical unsafe copy ends before write at exact first/second boundary");
            fixture_check(fixture_copy_core == (spec.expected_guard == "rbf_path" ? 1 : 0) && fixture_copy_rbf == 0, "historical preceding first-copy publication is observed");
        } else {
            fixture_check(fixture_frontier == "exit" && fixture_exit_code == 0 && fixture_guard.empty(), "historical unready gate invokes exit0 before copy, no main return inferred");
        }
#endif
    } else {
        fixture_check(fixture_guard.empty(), "fitting delivered main arguments never hit unsafe write guard");
        fixture_check(!fixture_calls.empty() && fixture_calls[0] == "affinity", "fitting actual main retains first runtime action");
        fixture_check(std::count(fixture_calls.begin(), fixture_calls.end(), "offload") == 1 && std::count(fixture_calls.begin(), fixture_calls.end(), "fpga-init") == 1, "fitting original early endpoint counts");
        std::vector<std::string> startup = {"\nMinimig by Dennis van Weeren", "\nARM Controller by Jakub Bednarski", "\nMiSTer code by Sorgelig\n\n", std::string("Version ") + (version+5) + "\n\n"};
        if (argc > 1) startup.push_back("Core path: " + spec.path + "\n");
        if (argc > 2) startup.push_back("XML path: " + spec.xml + "\n");
        fixture_check(fixture_prints.size() >= startup.size() && std::equal(startup.begin(), startup.end(), fixture_prints.begin()), "fitting startup stdout exact original prologue/spelling");
        fixture_check(fixture_main_stderr.empty(), "fitting main has no rejection stderr");
        if (!spec.ready) {
            fixture_check(fixture_frontier == "exit" && fixture_exit_code == 0 && fixture_main_return == -999, "unready fitting main invokes exit0 without inferred return");
            fixture_check(fixture_copy_core == 0 && fixture_copy_rbf == 0 && fixture_spi_reads == 0 && fixture_queries.empty(), "unready route never reaches selected init");
        } else {
            std::string desired = spec.expected_default;
            if (spec.complete_target.size() >= sizeof(defmra) || spec.last_default_rejected) desired.clear();
            fixture_check(fixture_copy_rbf == 1 && std::string(rbf_path) == spec.path, "fitting original RBF spelling published");
            fixture_check(std::string(defmra) == desired, "complete default identity admitted or rejected without prior fallback");
            fixture_check(static_cast<size_t>(fixture_root_calls) == fixture_formats.size(), "one root capture per actual formatter call");
            size_t expected_reads = spec.descriptor.size() >= 10239 ? 10239 : spec.descriptor.size() + 1;
            fixture_check(fixture_spi_reads == expected_reads && std::strlen(cfgstr) == std::min<size_t>(10239, spec.descriptor.size()), "whole reader descriptor admission");
            char *item = user_io_get_confstr(2); std::string item_value = item ? item : "";
            fixture_check(item_value.size() <= 2048, "whole getter item capacity");
            bool query = !spec.xml_present && !desired.empty();
            fixture_check(fixture_queries == (query ? std::vector<std::string>{desired} : std::vector<std::string>{}), "only complete admitted default is queried; explicit XML suppresses adoption");
            bool adopted = query && spec.exists && desired.back() != '/';
            std::vector<std::string> preparses; if (spec.xml_present) preparses.push_back(spec.xml); if (adopted) preparses.push_back(desired);
            fixture_check(fixture_preparses == preparses, "exact explicit/default preparse under finite regular-file response");
            std::string expected_core = adopted ? desired : spec.xml_present ? spec.xml : spec.path;
            fixture_check(std::string(core_path) == expected_core && fixture_copy_core == (adopted ? 2 : 1), "correct complete core publication and count");
            fixture_check(is_arcade_type == (adopted || (spec.xml_present && isXmlName(spec.xml.c_str()) == 1) ? 1 : 0), "original explicit/default arcade rules");
            int rejected_formats = 0;
            for (size_t i = 0; i < fixture_formats.size(); i++) {
                const auto &f = fixture_formats[i]; bool rejected = f.returned < 0 || static_cast<size_t>(f.returned) >= f.capacity;
                if (rejected) {
                    rejected_formats++; std::string retired = f.after_libc; retired[0] = 0;
                    const std::string after = i+1 < fixture_formats.size() ? fixture_formats[i+1].before : std::string(defmra, sizeof(defmra));
                    fixture_check(after == retired, "rejection retires only byte0 and preserves exact post-libc array tail");
                }
                fixture_check(f.negative_fault == (static_cast<int>(i)+1 == spec.negative_formatter_call), "negative formatter injection confined to explicitly declared call");
            }
            fixture_check(std::count(fixture_prints.begin(), fixture_prints.end(), "Default MRA path is too long or invalid; ignoring it.\n") == rejected_formats, "exact literal default rejection diagnostic per invalid effective declaration");
            fixture_check(std::count(fixture_prints.begin(), fixture_prints.end(), std::string("Using default MRA: ") + desired + "\n") == (adopted ? 1 : 0), "exact optional default adoption stdout");
#ifdef USE_SCHEDULER
            fixture_check(fixture_frontier == "scheduler-frontier" && fixture_main_return == -999 && fixture_poll_calls == 0, "scheduler endpoint stops at explicit lower frontier, no main return inferred");
            fixture_check(std::count(fixture_calls.begin(), fixture_calls.end(), "scheduler-init") == 1 && std::count(fixture_calls.begin(), fixture_calls.end(), "scheduler-run") == 1, "scheduler endpoint counts");
#else
            fixture_check(fixture_frontier == "poll-frontier" && fixture_main_return == -999 && fixture_poll_calls == 2, "loop ends at explicit second poll frontier, no main return inferred");
            for (const char *name : {"frame-timer", "input-poll", "HandleUI", "OsdUpdate"}) fixture_check(std::count(fixture_calls.begin(), fixture_calls.end(), name) == 1, "one complete original loop iteration");
            fixture_check(std::count(fixture_calls.begin(), fixture_calls.end(), "wait-reset") == (spec.loop_ready ? 0 : 2), "original loop readiness branch");
#endif
        }
    }
    std::string formats = "[";
    for (size_t i = 0; i < fixture_formats.size(); i++) {
        if (i) formats += ',';
        const auto &f = fixture_formats[i];
        formats += "{\"return\":" + std::to_string(f.returned) + ",\"libc_return\":" + std::to_string(f.libc_return) + ",\"capacity\":" + std::to_string(f.capacity) + ",\"published\":" + fixture_quote(f.published) + ",\"format\":" + fixture_quote(f.format) + ",\"captured_root\":" + fixture_quote(f.root) + ",\"delivered_payload\":" + fixture_quote(f.payload) + ",\"complete_target\":" + fixture_quote(f.complete_target) + ",\"before_hex\":" + fixture_quote(fixture_hex(f.before)) + ",\"after_libc_hex\":" + fixture_quote(fixture_hex(f.after_libc)) + ",\"synthetic_negative\":" + (f.negative_fault ? "true" : "false") + '}';
    }
    formats += ']';
    fixture_check(disable_osd == (spec.ready && !invalid ? spec.expected_disable_osd : 0), "finite original X-option disable_osd state");
    char *delivered = fixture_spi_reads ? user_io_get_confstr(2) : nullptr; std::string delivered_item = delivered ? delivered : "";
    std::string out = "{\"case\":" + fixture_quote(spec.name) + ",\"kind\":\"main-default\",\"assertions\":" + std::to_string(fixture_checks-before_checks) + ",\"failed_assertions\":" + std::to_string(fixture_failures-before_failed);
    out += ",\"frontier\":" + fixture_quote(fixture_frontier) + ",\"observed_main_return\":" + std::to_string(fixture_main_return) + ",\"observed_exit_code\":" + std::to_string(fixture_exit_code);
    out += ",\"guard_destination\":" + fixture_quote(fixture_guard) + ",\"guard_required_bytes\":" + std::to_string(fixture_guard_bytes) + ",\"guard_capacity\":" + std::to_string(fixture_guard_capacity);
    out += ",\"calls\":" + fixture_array(fixture_calls) + ",\"path\":" + fixture_quote(spec.path) + ",\"xml_present\":" + (spec.xml_present ? "true" : "false") + ",\"xml\":" + fixture_quote(spec.xml) + ",\"root\":" + fixture_quote(spec.root);
    out += ",\"descriptor_input_bytes\":" + std::to_string(spec.descriptor.size()) + ",\"descriptor_delivered_bytes\":" + std::to_string(std::strlen(cfgstr)) + ",\"SPI_reads\":" + std::to_string(fixture_spi_reads);
    out += ",\"delivered_item2\":" + fixture_quote(delivered_item) + ",\"delivered_item2_bytes\":" + std::to_string(delivered_item.size()) + ",\"complete_delivered_target\":" + fixture_quote(spec.complete_target);
    out += ",\"defmra\":" + fixture_quote(defmra) + ",\"formats\":" + formats + ",\"queries\":" + fixture_array(fixture_queries) + ",\"preparses\":" + fixture_array(fixture_preparses);
    out += ",\"core_path\":" + fixture_quote(core_path) + ",\"rbf_path\":" + fixture_quote(rbf_path) + ",\"arcade_flag\":" + std::to_string(static_cast<int>(is_arcade_type)) + ",\"copy_core_count\":" + std::to_string(fixture_copy_core) + ",\"copy_rbf_count\":" + std::to_string(fixture_copy_rbf);
    out += ",\"core_array_hex\":" + fixture_quote(fixture_hex(std::string(core_path, sizeof(core_path)))) + ",\"rbf_array_hex\":" + fixture_quote(fixture_hex(std::string(rbf_path, sizeof(rbf_path)))) + ",\"defmra_array_hex\":" + fixture_quote(fixture_hex(std::string(defmra, sizeof(defmra))));
    out += ",\"production_prints\":" + fixture_array(fixture_prints) + ",\"main_stderr\":" + fixture_array(fixture_main_stderr) + ",\"version_line\":" + fixture_quote(std::string("Version ") + (version+5) + "\n\n");
    out += ",\"validator_calls\":" + std::to_string(fixture_validator_calls) + ",\"validator_before_runtime\":" + (fixture_validator_before_runtime ? "true" : "false") + ",\"errors\":" + fixture_array(fixture_errors) + '}';
    out.pop_back(); out += ",\"disable_osd\":" + std::to_string(disable_osd) + '}';
    std::puts(out.c_str());
}
#ifdef FIXTURE_REAL_INIT_VALIDATOR
void fixture_helper_case(const char *name, const char *path, const char *xml, bool expected, size_t path_bytes = 0, size_t xml_bytes = 0)
{
    fixture_reset(fixture_case(name)); int before_checks = fixture_checks, before_failed = fixture_failures;
    std::string core_before(core_path, sizeof(core_path)), rbf_before(rbf_path, sizeof(rbf_path)), default_before(defmra, sizeof(defmra)), descriptor_before(cfgstr, sizeof(cfgstr));
    std::string path_before = path && path_bytes ? std::string(path, path_bytes) : "";
    std::string xml_before = xml && xml_bytes ? std::string(xml, xml_bytes) : "";
    bool observed = user_io_init_paths_valid(path, xml);
    fixture_check(observed == expected, "direct actual production validator boolean result");
    fixture_check(fixture_calls.empty() && fixture_prints.empty() && fixture_main_stderr.empty() && fixture_queries.empty() && fixture_preparses.empty(), "pure actual query has no runtime endpoints or output");
    fixture_check(std::string(core_path, sizeof(core_path)) == core_before && std::string(rbf_path, sizeof(rbf_path)) == rbf_before && std::string(defmra, sizeof(defmra)) == default_before && std::string(cfgstr, sizeof(cfgstr)) == descriptor_before, "pure query preserves all four source arrays");
    fixture_check((!path_bytes || std::string(path, path_bytes) == path_before) && (!xml_bytes || std::string(xml, xml_bytes) == xml_before), "pure query preserves all explicitly supplied input bytes");
    std::printf("{\"case\":%s,\"kind\":\"direct-helper\",\"assertions\":%d,\"failed_assertions\":%d,\"path_null\":%s,\"xml_null\":%s,\"explicit_path_bytes\":%zu,\"explicit_xml_bytes\":%zu,\"observed\":%s,\"expected\":%s,\"source_arrays_unchanged\":%s,\"errors\":%s}\n", fixture_quote(name).c_str(), fixture_checks-before_checks, fixture_failures-before_failed, path ? "false" : "true", xml ? "false" : "true", path_bytes, xml_bytes, observed ? "true" : "false", expected ? "true" : "false", (std::string(core_path,sizeof(core_path))==core_before && std::string(rbf_path,sizeof(rbf_path))==rbf_before && std::string(defmra,sizeof(defmra))==default_before && std::string(cfgstr,sizeof(cfgstr))==descriptor_before) ? "true" : "false", fixture_array(fixture_errors).c_str());
}
void fixture_helper_controls()
{
    std::string fit = fixture_legal_path(1023), big = fixture_legal_path(1024);
    char unended_path[1024], unended_xml[1024], final_path[1024], final_xml[1024];
    std::memset(unended_path, 'p', sizeof(unended_path)); std::memset(unended_xml, 'x', sizeof(unended_xml));
    std::memset(final_path, 'p', sizeof(final_path)); std::memset(final_xml, 'x', sizeof(final_xml)); final_path[1023] = final_xml[1023] = 0;
    fixture_helper_case("api-nullpath-nullxml", nullptr, nullptr, false);
    fixture_helper_case("api-nullpath-emptyxml", nullptr, "", false);
    fixture_helper_case("api-nullpath-fittingxml", nullptr, "A.mra", false);
    fixture_helper_case("api-empty-nullxml", "", nullptr, true);
    fixture_helper_case("api-empty-emptyxml", "", "", true);
    fixture_helper_case("api-empty-fittingxml", "", "A.mra", true);
    fixture_helper_case("api-path1023-nullxml", fit.c_str(), nullptr, true, 1024);
    fixture_helper_case("api-path1024-nullxml", big.c_str(), nullptr, false, 1025);
    fixture_helper_case("api-path1023-emptyxml", fit.c_str(), "", true, 1024);
    fixture_helper_case("api-path1024-emptyxml", big.c_str(), "", false, 1025);
    fixture_helper_case("api-fittingpath-xml1023", "CORE.rbf", fit.c_str(), true, 0, 1024);
    fixture_helper_case("api-fittingpath-xml1024", "CORE.rbf", big.c_str(), false, 0, 1025);
    fixture_helper_case("api-both1023", fit.c_str(), fit.c_str(), true, 1024, 1024);
    fixture_helper_case("api-path-unterminated1024", unended_path, nullptr, false, sizeof(unended_path));
    fixture_helper_case("api-xml-unterminated1024", "CORE.rbf", unended_xml, false, 0, sizeof(unended_xml));
    fixture_helper_case("api-path-finalnul1023", final_path, nullptr, true, sizeof(final_path));
    fixture_helper_case("api-xml-finalnul1023", "CORE.rbf", final_xml, true, 0, sizeof(final_xml));
    fixture_helper_case("api-percent-utf8", "games/100%_caf\xc3\xa9.rbf", "games/100%_caf\xc3\xa9.mra", true);
}
#endif
int main()
{
    std::vector<FixtureSpec> cases = fixture_catalog();
    fixture_check(cases.size() == 74, "original69 plus5 fixed formatter/recovery input count");
    for (const FixtureSpec &spec : cases) fixture_execute(spec);
    size_t helpers = 0;
#ifdef FIXTURE_REAL_INIT_VALIDATOR
    fixture_helper_controls(); helpers = 18;
#endif
    std::printf("{\"summary\":{\"cases\":%zu,\"main_default_cases\":%zu,\"direct_helper_cases\":%zu,\"assertions\":%d,\"failed_assertions\":%d,\"observed_legacy_pre_copy_guards\":%d}}\n", cases.size()+helpers, cases.size(), helpers, fixture_checks, fixture_failures, fixture_observed_unsafe_guards);
    return fixture_failures ? 1 : 0;
}
