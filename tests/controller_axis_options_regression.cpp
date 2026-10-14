#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include <math.h>
#include "axis_fixture_cfg.h"
#include "axis_fixture_spi.h"
#include "axis_fixture_str_util.h"

#ifdef __linux__
#include <linux/input.h>
using FixtureEvent = input_event;
using FixtureAbsInfo = input_absinfo;
#else
// Only the fields consumed by the selected ordinary event interval are modeled.
// These unique names never replace or redeclare native Linux ABI declarations.
struct FixtureEvent { uint16_t type, code; int32_t value; };
struct FixtureAbsInfo { int32_t value, minimum, maximum, fuzz, flat, resolution; };
struct FixtureOpaqueFFEffect { uint8_t unexecuted_storage[64]; };
#define EV_ABS 3
#define EV_REL 2
#endif

#include "axis_types.inc"
#include "axis_cases.inc"

static_assert(std::is_unsigned<char>::value, "Explicit byte-consumer char model required");
cfg_t cfg = {};
static bool fixture_psx = false, fixture_n64 = false, fixture_osd = false;
static uint32_t fixture_tv = 0;
static int assertions = 0;
static std::vector<std::string> failures, trace;
struct Api { std::string side; int player, x, y; };
struct Packet { int command, player, mode; std::vector<uint16_t> words; };
struct Math { std::string stage; int device, stick, x, y, out_x, out_y, cardinal; float range; };
static std::vector<Api> api;
static std::vector<Packet> packets;
static std::vector<Math> math_steps;
static bool packet_active = false;
static Packet pending_packet;
static std::string identity;
static int normal_events = 0;

static void check(bool condition, const std::string &description)
{
    ++assertions;
    if (!condition) failures.push_back(description);
}

static int signed_byte(unsigned char value)
{
    return value >= 128 ? int(value) - 256 : value;
}

static std::string quote(const std::string &value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else out << char(c);
    }
    out << '"';
    return out.str();
}

static std::string byte_signature(const void *data, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

char is_psx() { trace.push_back("is_psx"); return fixture_psx; }
char is_n64() { trace.push_back("is_n64"); return fixture_n64; }
char user_io_osd_is_visible() { trace.push_back("osd_visible"); return fixture_osd; }
uint32_t user_io_status_get(const char *option, int extra)
{
    check(std::strcmp(option, "TV") == 0 && extra == 1, "actual N64 TV query");
    trace.push_back("status:TV:1");
    return fixture_tv;
}

uint8_t spi_uio_cmd8_cont(uint8_t command, uint8_t player)
{
    check(!packet_active, "packet starts after previous DisableIO");
    packet_active = true;
    pending_packet = Packet{command, player, io_ver, {}};
    trace.push_back("spi_command:" + std::to_string(command) + ":" + std::to_string(player));
    return 0;
}

uint16_t fpga_spi(uint16_t word)
{
    check(packet_active, "actual SPI inline writes only inside packet");
    pending_packet.words.push_back(word);
    trace.push_back("spi_word:" + std::to_string(word));
    return 0;
}

void DisableIO()
{
    check(packet_active, "actual packet closes");
    packets.push_back(pending_packet);
    packet_active = false;
    trace.push_back("DisableIO");
}

static void record_api(const char *side, unsigned char player, char x, char y)
{
    api.push_back(Api{side, player, signed_byte(static_cast<unsigned char>(x)), signed_byte(static_cast<unsigned char>(y))});
    trace.push_back(std::string("analog:") + side + ":" + std::to_string(player));
}

static void record_deadzone(int *x, int *y, const devInput *device, int stick)
{
    const int dev = int(device - input);
    check(dev >= 0 && dev < NUMDEV && stick >= 0 && stick < 2, "ordinary deadzone observer boundary");
    math_steps.push_back(Math{"deadzone", dev, stick, *x, *y, 0, 0, device->max_cardinal[stick], device->max_range[stick]});
    trace.push_back("deadzone");
}
static void finish_deadzone(int x, int y)
{
    math_steps.back().out_x = x;
    math_steps.back().out_y = y;
}
static void record_n64(int x, int y, int cardinal, float range)
{
    math_steps.push_back(Math{"n64", -1, -1, x, y, 0, 0, cardinal, range});
    trace.push_back("n64");
}
static void finish_n64(int x, int y)
{
    math_steps.back().out_x = x;
    math_steps.back().out_y = y;
}

#include "axis_output.inc"
#include "axis_math.inc"
#include "axis_dispatch.inc"

static void event(int code, int value, int device = 0, int minimum = -127, int maximum = 127, int type = EV_ABS)
{
    FixtureEvent ev = {};
    FixtureAbsInfo abs = {};
    ev.type = static_cast<uint16_t>(type); ev.code = static_cast<uint16_t>(code); ev.value = value;
    abs.minimum = minimum; abs.maximum = maximum;
    const FixtureEvent original_ev = ev;
    const FixtureAbsInfo original_abs = abs;
    selected_normal_event(&ev, &abs, device);
    ++normal_events;
    check(std::memcmp(&ev, &original_ev, sizeof(ev)) == 0, "selected event input bytes unchanged");
    check(std::memcmp(&abs, &original_abs, sizeof(abs)) == 0, "selected absinfo input bytes unchanged");
}

static void setup(int index)
{
    std::memset(input, 0, sizeof(input));
    cfg = cfg_t{};
    core_type = CORE_TYPE_8BIT; grabbed = 1;
    for (int dev = 0; dev < NUMDEV; ++dev) {
        input[dev].bind = -1;
        input[dev].num = dev < 3 ? static_cast<uint8_t>(dev + 1) : 0;
        input[dev].stick_l[0] = SYS_AXIS1_X; input[dev].stick_l[1] = SYS_AXIS1_Y;
        input[dev].stick_r[0] = SYS_AXIS2_X; input[dev].stick_r[1] = SYS_AXIS2_Y;
        input[dev].mmap[SYS_AXIS1_X] = 0; input[dev].mmap[SYS_AXIS1_Y] = 1;
        input[dev].mmap[SYS_AXIS2_X] = 3; input[dev].mmap[SYS_AXIS2_Y] = 4;
        input[dev].mmap[SYS_AXIS_MX] = 2; input[dev].mmap[SYS_AXIS_MY] = 5;
        input[dev].vid = 0x1234; input[dev].pid = static_cast<uint16_t>(0x5678 + dev);
        input[dev].unique_hash = 0x13579bdf;
        std::snprintf(input[dev].idstr, sizeof(input[dev].idstr), "%04x_%04x", input[dev].vid, input[dev].pid);
        std::snprintf(input[dev].id, sizeof(input[dev].id), "usb-normal-%d/input0", dev);
        std::snprintf(input[dev].sysfs, sizeof(input[dev].sysfs), "/devices/normal/usb%d", dev);
    }
    if (index < 5) {
        const unsigned zones[] = {0, 1, 2, 10, 64};
        input[0].deadzone = zones[index];
        input[0].max_cardinal[0] = 127;
        input[0].max_range[0] = std::hypot(127.0f, 127.0f);
    }
    fixture_psx = index == 9 || index == 10 || index == 50 || index == 51;
    fixture_n64 = (index >= 30 && index <= 39) || index == 44 || index == 45;
    io_ver = index == 6 || index == 8 || index == 9 || index == 22 || (index >= 32 && index <= 39 && (index & 1));
    if (index >= 32 && index <= 39) fixture_tv = static_cast<uint32_t>(index - 32);
    if (index == 13) input[0].bind = 1;
    if (index == 15) grabbed = 0;
    if (index == 16) fixture_osd = true;
    if (index == 17) input[0].num = 0;
    if (index == 18) core_type = CORE_TYPE_UNKNOWN;
    if (index == 19 || index == 20) mouse_emu = 1;
    if (index == 24 || index == 25) input[0].lightgun = 1;
    if (index >= 26 && index <= 29) {
        input[0].quirk = QUIRK_WHEEL;
        input[0].wh_steer = 0; input[0].wh_accel = 3;
        input[0].wh_brake = 4; input[0].wh_clutch = 5; input[0].wh_combo = 6;
        if (index == 28) input[0].wh_pedal_invert = 1;
    }
    if (index == 41) { cfg.controller_unique_mapping[0] = 1; cfg.controller_unique_mapping[1] = 1; }
    if (index == 42) { cfg.controller_unique_mapping[0] = 1; cfg.controller_unique_mapping[1] = 0x99998888; }
    if (index == 43) std::snprintf(input[0].idstr, sizeof(input[0].idstr), "%s", u8"1234_5678_café");
    if (index == 44 || index == 45) { input[0].num = 4; fixture_tv = index == 44 ? 2 : 1; }
    if (index == 46 || index == 47) joyswap = 1;
    if (index == 48 || index == 50) input[0].deadzone = 10;
    if (index == 49 || index == 51) input[0].deadzone = 64;
}

static void ordinary_sequence(bool right)
{
    const int x = right ? 3 : 0, y = right ? 4 : 1;
    event(x, 80); event(y, -40); event(x, 80); event(x, 1); event(x, -127); event(y, 127);
}

static void execute_case(int index)
{
    if (index < 5) {
        const int vectors[][2] = {{0,0},{1,0},{2,0},{10,0},{11,0},{64,0},{65,0},{80,-40},{127,0},{-127,0},{127,127},{-90,90}};
        for (const auto &v : vectors) {
            int x = v[0], y = v[1];
            joy_apply_deadzone(&x, &y, &input[0], 0);
            check(x >= -127 && x <= 127 && y >= -127 && y <= 127, "normal deadzone range");
            check((x == 0 || (x < 0) == (v[0] < 0)) && (y == 0 || (y < 0) == (v[1] < 0)), "deadzone sign");
            check(std::hypot(double(x), double(y)) <= std::hypot(double(v[0]), double(v[1])) + 1.0, "deadzone does not increase radius beyond integer rounding");
            if (!input[0].deadzone) check(x == v[0] && y == v[1], "zero deadzone exact identity");
            if (input[0].deadzone <= 2 && input[0].deadzone && std::max(std::abs(v[0]),std::abs(v[1])) <= int(input[0].deadzone)) check(x == 0 && y == 0, "small box deadzone");
            if (input[0].deadzone > 2 && std::hypot(double(v[0]),double(v[1])) <= input[0].deadzone) check(x == 0 && y == 0, "inner radial deadzone");
        }
        return;
    }
    switch (index) {
    case 5: case 6: case 46: ordinary_sequence(false); break;
    case 7: case 8: case 47: ordinary_sequence(true); break;
    case 9: event(0, -32768,0,-32768,32767); event(1,32767,0,-32768,32767); break;
    case 10: event(3,-32768,0,-32768,32767); event(4,32767,0,-32768,32767); break;
    case 11: event(0,1); event(1,-1); event(0,2); event(1,-2); break;
    case 12: event(0,80); event(0,80); event(1,-40); event(1,-40); break;
    case 13: event(0,80); event(1,-40); break;
    case 14: event(0,80,0); event(0,-64,1); event(1,-40,0); event(1,32,1); break;
    case 15: case 16: case 17: case 18: event(0,80); event(1,-40); break;
    case 19: event(2,80); break;
    case 20: event(5,-96); break;
    case 21: case 22: event(8,128,0,0,255); break;
    case 23: event(7,-12,0,-127,127,EV_REL); event(7,7,0,-127,127,EV_REL); break;
    case 24: event(0,80); event(1,-40); break;
    case 25: event(3,80); event(4,-40); break;
    case 26: event(0,80); event(0,-80); break;
    case 27: case 28: event(3,0,0,0,255); event(3,255,0,0,255); break;
    case 29: event(6,-80); event(6,80); event(6,0); break;
    case 30: event(0,127); break;
    case 31: event(0,127); event(1,127); break;
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39: event(3,127); event(4,127); break;
    case 40: identity = get_unique_mapping(0); break;
    case 41: identity = get_unique_mapping(0); break;
    case 42: identity = get_unique_mapping(0); break;
    case 43: identity = get_unique_mapping(0,1); break;
    case 44: case 45: event(0,127); event(1,127); break;
    case 48: case 49: case 50: case 51: event(0,127); event(1,127); event(0,65); event(1,-65); event(0,0); event(1,0); break;
    default: check(false,"unknown finite case"); break;
    }
    if (index == 5 || index == 6 || index == 7 || index == 8 || index == 46 || index == 47) {
        const int expected[][2] = {{80,0},{80,-40},{0,-40},{-127,-40},{-127,127}};
        check(api.size() == 5,"ordinary exact output count and duplicate suppression");
        for (size_t i = 0; i < api.size() && i < 5; ++i) check(api[i].x == expected[i][0] && api[i].y == expected[i][1],"ordinary exact paired bytes");
    }
    if (index == 9 || index == 10) check(api.size()==2 && api[0].x==-128 && api[1].x==-128 && api[1].y==127,"PSX asymmetric endpoint bytes");
    if (index == 12) check(api.size()==2,"raw repeated events suppress output");
    if (index == 13) check(api.size()==2 && api.back().player==1 && input[0].axis_pos[0]==80 && input[1].max_cardinal[0]==80,"bound raw cache and resolved player");
    if (index == 14) check(api.size()==4 && api[2].player==0 && api[2].x==80 && api[3].player==1 && api[3].x==-64,"separate player paired positions");
    if (index == 15 || index == 16 || index == 17) check(api.empty(),"disabled route has no analog endpoint");
    if (index == 18) check(api.size()==2 && packets.empty(),"non8bit endpoint retains no SPI packet");
    if (index == 19 || index == 20) check(api.empty() && (index==19 ? mouse_emu_x==6 : mouse_emu_y==-8),"ordinary mouse route preserved");
    if (index == 21 || index == 22) check(api.size()==1 && api[0].player==15 && api[0].x==-128,"unsigned paddle byte retained");
    if (index == 23) check(api.size()==2 && api[0].player==143 && api[0].x==-12 && api[1].x==7,"spinner special packet preserved");
    if (index == 30) check(api.size()==1 && api[0].x==85 && api[0].y==0,"N64 cardinal85");
    if (index == 31 || (index>=32 && index<=39) || index==44 || index==45) check(api.size()==2 && api.back().x==69 && api.back().y==69,"N64 learned diagonal69");
    if (index>=32 && index<=39) {
        const int player[] = {0,1,2,2,0,0,0,0}, stick[] = {1,0,0,0,0,0,0,0};
        check(!api.empty() && api.back().player==player[index-32] && api.back().side==(stick[index-32]?"r":"l"),"N64 TV routing table");
    }
    if (index==44) check(!api.empty() && api.back().player==1 && api.back().side=="r","N64 p3 reverse route");
    if (index==45) check(!api.empty() && api.back().player==2 && api.back().side=="r","N64 p2 reverse route");
    if (index==40 || index==42) check(identity=="1234_5678","actual default/nonmatching identity");
    if (index==41) check(identity=="1234_5678_13579bdf","actual configured unique identity");
    if (index==43) check(identity==u8"1234_5678_café","force unique retains actual empty-list identity");
    if (index==46 || index==47) check(!packets.empty() && packets.back().player==1,"actual joyswap byte player");
}

static bool permitted_byte(size_t byte)
{
    const size_t device = byte / sizeof(devInput), offset = byte % sizeof(devInput);
    if (device >= 3) return false;
    const size_t starts[] = {offsetof(devInput,axis_pos),offsetof(devInput,ss_range),offsetof(devInput,max_cardinal),offsetof(devInput,max_range)};
    const size_t lengths[] = {sizeof(input[0].axis_pos),sizeof(input[0].ss_range),sizeof(input[0].max_cardinal),sizeof(input[0].max_range)};
    for (size_t i=0;i<4;++i) if(offset>=starts[i] && offset<starts[i]+lengths[i]) return true;
    return false;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const int index = std::atoi(argv[1]);
    if (index<0 || index>=int(sizeof(case_names)/sizeof(case_names[0]))) return 2;
    setup(index);
    unsigned char before[sizeof(input)]; std::memcpy(before,input,sizeof(input));
    const cfg_t cfg_before = cfg;
    execute_case(index);
    bool untouched = true;
    const auto *after = reinterpret_cast<const unsigned char *>(input);
    for(size_t byte=0;byte<sizeof(input);++byte) if(!permitted_byte(byte) && before[byte]!=after[byte]) untouched=false;
    check(untouched,"all input bytes outside declared raw cache/learning fields unchanged");
    check(std::memcmp(&cfg_before,&cfg,sizeof(cfg))==0,"all config bytes unchanged");
    check(!packet_active,"no incomplete packet");
    for(const auto &packet:packets) check(packet.words.size()==size_t(packet.mode?1:2),"actual byte/word packet width");
    std::ostringstream out;
    out << "{\"name\":" << quote(case_names[index]) << ",\"assertions\":" << assertions << ",\"failed_assertions\":" << failures.size();
    out << ",\"normal_events\":" << normal_events << ",\"identity\":" << quote(identity) << ",\"mouse\":[" << mouse_emu_x << ',' << mouse_emu_y << ']';
    out << ",\"unpermitted_input_bytes_unchanged\":" << (untouched?"true":"false") << ",\"cfg_bytes_unchanged\":" << (std::memcmp(&cfg_before,&cfg,sizeof(cfg))==0?"true":"false");
    out << ",\"same_build_input_signature_before\":" << quote(byte_signature(before,sizeof(before))) << ",\"same_build_input_signature_after\":" << quote(byte_signature(input,sizeof(input)));
    out << ",\"same_build_cfg_signature\":" << quote(byte_signature(&cfg,sizeof(cfg))) << ",\"api\":[";
    for(size_t i=0;i<api.size();++i){if(i)out<<',';out<<"["<<quote(api[i].side)<<','<<api[i].player<<','<<api[i].x<<','<<api[i].y<<']';}
    out << "],\"packets\":[";
    for(size_t i=0;i<packets.size();++i){if(i)out<<',';const auto &p=packets[i];out<<'['<<p.command<<','<<p.player<<','<<p.mode<<",[";for(size_t j=0;j<p.words.size();++j){if(j)out<<',';out<<p.words[j];}out<<"]]";}
    out << "],\"math\":[";
    for(size_t i=0;i<math_steps.size();++i){if(i)out<<',';const auto &m=math_steps[i];uint32_t bits=0;std::memcpy(&bits,&m.range,sizeof(bits));out<<'['<<quote(m.stage)<<','<<m.device<<','<<m.stick<<','<<m.x<<','<<m.y<<','<<m.out_x<<','<<m.out_y<<','<<m.cardinal<<','<<bits<<']';}
    out << "],\"raw_devices\":[";
    for(int dev=0;dev<3;++dev){if(dev)out<<',';out<<"{\"device\":"<<dev<<",\"num\":"<<int(input[dev].num)<<",\"bind\":"<<input[dev].bind<<",\"deadzone\":"<<input[dev].deadzone<<",\"axis_pos\":[";for(int a=0;a<256;++a){if(a)out<<',';out<<int(input[dev].axis_pos[a]);}out<<"],\"ss_range\":["<<input[dev].ss_range[0]<<','<<input[dev].ss_range[1]<<"],\"max_cardinal\":["<<input[dev].max_cardinal[0]<<','<<input[dev].max_cardinal[1]<<"],\"max_range_bits\":[";for(int s=0;s<2;++s){if(s)out<<',';uint32_t bits=0;std::memcpy(&bits,&input[dev].max_range[s],sizeof(bits));out<<bits;}out<<"]}";}
    out << "],\"trace\":[";for(size_t i=0;i<trace.size();++i){if(i)out<<',';out<<quote(trace[i]);}
    out << "],\"failures\":[";for(size_t i=0;i<failures.size();++i){if(i)out<<',';out<<quote(failures[i]);}out<<"]}";
    std::cout << "CASE_JSON=" << out.str() << '\n';
    return failures.empty()?0:1;
}
