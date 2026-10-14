#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "sxmlc.h"
#include "support/arcade/buffer.h"
#define USE_PROTOTYPES 1
#include "lib/md5/md5.h"
#ifndef __linux__
typedef off_t __off64_t;
#define stat64 stat
#endif
#include "arcade_nvram_types.inc"

struct Boundary { std::string kind; };
static unsigned checks, failures, allocation_rejections;
static std::string document;
static bool fail_xml_open, parser_seen, parser_completed;
static int parser_result;
static unsigned parser_streams, starts, ends, errors, nvm_reads;
static bool allocation_seen, budget_rejected;
static bool fail_scalar_allocation, null_root, change_root_on_new, change_binding_on_spi, change_binding_on_new;
static unsigned root_calls;
static bool fail_nvm_allocation, fail_save_open, fail_save_write, fail_save_close, fail_save_rename, save_started, save_completed;
static size_t allocation_bytes;
static std::string requested_xml, file_opened, saved_target;
static std::vector<std::string> events, snapshots, parsed_numbers, transfers, file_returns;
static std::vector<uint8_t> temporary_payload;
static std::map<std::string, std::vector<uint8_t>> files;
static std::map<fileTYPE *, std::string> opened_files;
static std::vector<unsigned> spi_bytes;
static bool fpga_enabled;
static std::string storage_root = "/media/fat";
static const char *getRootDir() { root_calls++; return null_root ? nullptr : storage_root.c_str(); }
static std::string quote(const std::string &value)
{
    std::string out = "\"";
    const char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += c;
    }
    return out + "\"";
}
static std::string array(const std::vector<std::string> &values, bool raw = false)
{
    std::string out = "[";
    for (size_t i = 0; i < values.size(); i++) { if (i) out += ','; out += raw ? values[i] : quote(values[i]); }
    return out + "]";
}
static std::string bytes(const uint8_t *data, size_t length)
{
    const char hex[] = "0123456789abcdef"; std::string out;
    for (size_t i = 0; i < length; i++) { out += hex[data[i] >> 4]; out += hex[data[i] & 15]; }
    return out;
}
static void check(bool ok, const char *label)
{
    checks++;
    if (!ok) { failures++; std::printf("FAIL: %s\n", label); }
}
static unsigned count(const std::string &prefix)
{
    return static_cast<unsigned>(std::count_if(events.begin(), events.end(), [&](const std::string &item) { return item.find(prefix) == 0; }));
}
static void forbidden(const char *name) { throw std::runtime_error(std::string("Unrelated production branch executed: ") + name); }
static void overwrite_binding();
// Replacement allocation boundary stays observable under optimizer elision.
__attribute__((noinline)) void *operator new[](size_t length)
{
    allocation_seen = true; allocation_bytes = length;
    events.push_back("nvm-new:" + std::to_string(length));
    if (change_root_on_new) storage_root = "/media/usb0";
    if (change_binding_on_new) { change_binding_on_new=false; overwrite_binding(); }
    if (length > 4096) throw Boundary{"bounded-array-allocation"};
    if (fail_nvm_allocation) { allocation_rejections++; events.push_back("nvm-allocation-rejected:"+std::to_string(length)); throw std::bad_alloc(); }
    if (void *result = std::malloc(length ? length : 1)) return result;
    throw std::bad_alloc();
}
__attribute__((noinline)) void *operator new(size_t length)
{
    if (fail_scalar_allocation) throw std::bad_alloc();
    if (void *p = std::malloc(length ? length : 1)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void *data) noexcept { std::free(data); }
__attribute__((noinline)) void operator delete(void *data, size_t) noexcept { std::free(data); }
__attribute__((noinline)) void *operator new[](size_t length, const std::nothrow_t &) noexcept
{
    allocation_seen = true; allocation_bytes = length;
    events.push_back("nvm-new:" + std::to_string(length));
    if (change_root_on_new) storage_root = "/media/usb0";
    if (change_binding_on_new) { change_binding_on_new=false; overwrite_binding(); }
    if (length > 4096) { budget_rejected = true; allocation_rejections++; events.push_back("nvm-allocation-rejected:"+std::to_string(length)); return nullptr; }
    if (fail_nvm_allocation) { allocation_rejections++; events.push_back("nvm-allocation-rejected:"+std::to_string(length)); return nullptr; }
    return std::malloc(length ? length : 1);
}
__attribute__((noinline)) void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void *data) noexcept { std::free(data); }
__attribute__((noinline)) void operator delete[](void *data, size_t) noexcept { std::free(data); }
static int libc_formatter(char *out, size_t capacity, const char *format, ...)
{
    va_list args; va_start(args, format);
    const int result = std::vsnprintf(out, capacity, format, args);
    va_end(args); return result;
}
static int observed_snprintf(char *out, size_t capacity, const char *format)
{
    for (const char *p = format; *p; p++) if (*p == '%') {
        if (p[1] != '%') throw Boundary{"untrusted-dynamic-format"};
        p++;
    }
    return libc_formatter(out, capacity, format);
}
template<class... Args> static int observed_snprintf(char *out, size_t capacity, const char *format, Args... args)
{
    const char *allowed[] = {"%s", "%s.mra", "md5 mismatch for rom %d", "%s\n%s not found", "XML parse: %s: ERROR %d\n"};
    if (std::find_if(std::begin(allowed), std::end(allowed), [&](const char *v) { return !std::strcmp(v, format); }) == std::end(allowed))
        throw std::runtime_error("Unknown formatter rejected before varargs");
    return libc_formatter(out, capacity, format, args...);
}
template<size_t N> static char *observed_strcat(char (&out)[N], const char *text)
{
    const char *end = static_cast<const char *>(std::memchr(out, 0, N));
    if (!end || std::strlen(text) >= N - static_cast<size_t>(end - out)) throw Boundary{"bounded-producer-append"};
    return std::strcat(out, text);
}
static unsigned long observed_strtoul(const char *text, char **end, int base)
{
    const unsigned long value = std::strtoul(text, end, base);
    const int saved_errno = errno;
    parsed_numbers.push_back("{\"text\":" + quote(text) + ",\"unsigned_long_value\":" + quote(std::to_string(value)) + ",\"errno\":" + std::to_string(saved_errno) + "}");
    errno = saved_errno;
    return value;
}
extern "C" FILE *nvram_fixture_fopen(const char *filename, const char *mode)
{
    if (std::strcmp(mode, "rt")) throw std::runtime_error("Unexpected XML stream mode");
    requested_xml = filename; events.push_back("xml-open:" + requested_xml);
    if (fail_xml_open) { errno = ENOENT; return nullptr; }
    FILE *stream = std::tmpfile();
    if (!stream) throw std::runtime_error("Disposable parser stream unavailable");
    if (std::fwrite(document.data(), 1, document.size(), stream) != document.size()) throw std::runtime_error("Disposable stream write failed");
    std::rewind(stream); parser_streams++;
    return stream;
}
static int xml_send_rom(XMLEvent, const XMLNode *, char *, int, SAX_Data *);
static int observed_xml_callback(XMLEvent event, const XMLNode *node, char *text, int n, SAX_Data *data)
{
    if (event == XML_EVENT_START_DOC) starts++;
    if (event == XML_EVENT_END_DOC) ends++;
    if (event == XML_EVENT_ERROR) errors++;
    events.push_back("xml-event:" + std::to_string(event) + (node ? std::string(":") + node->tag + "#type=" + std::to_string(node->tag_type) : ""));
    return xml_send_rom(event, node, text, n, data);
}
static int observed_parse_file(const char *filename, const SAX_Callbacks *callbacks, void *user)
{
    if (callbacks->all_event != xml_send_rom) throw std::runtime_error("Unexpected production SAX callback");
    SAX_Callbacks observed = *callbacks; observed.all_event = observed_xml_callback;
    parser_seen = true;
    parser_result = XMLDoc_parse_file_SAX(filename, &observed, user);
    parser_completed = true;
    events.push_back("xml-return:" + std::to_string(parser_result));
    return parser_result;
}
static void EnableFpga()
{
    if (fpga_enabled) throw std::runtime_error("Nested FPGA enable");
    fpga_enabled = true; spi_bytes.clear();
}
static void overwrite_binding();
static void spi8(unsigned char value)
{
    if (!fpga_enabled) throw std::runtime_error("SPI byte outside enable");
    spi_bytes.push_back(value);
    if (change_binding_on_spi && spi_bytes.size() == 2 && spi_bytes[0] == FIO_FILE_INDEX) { change_binding_on_spi = false; overwrite_binding(); }
}
static void spi_w(uint16_t) { forbidden("nonzero core address"); }
static void DisableFpga()
{
    if (!fpga_enabled) throw std::runtime_error("FPGA disable without enable");
    fpga_enabled = false;
    std::string tx = "spi";
    for (unsigned v : spi_bytes) tx += ":" + std::to_string(v);
    events.push_back(tx);
}
static void spi_write(const uint8_t *data, uint32_t length, int wide)
{
    if (!fpga_enabled || spi_bytes != std::vector<unsigned>{FIO_FILE_TX_DAT} || length > 4096) throw std::runtime_error("Unsafe/invalid core write");
    transfers.push_back("tx:" + std::to_string(length) + ":" + std::to_string(wide) + ":" + bytes(data, length));
    events.push_back(transfers.back());
}
static void spi_read(uint8_t *data, uint32_t length, int wide)
{
    if (!fpga_enabled || spi_bytes != std::vector<unsigned>{FIO_FILE_TX_DAT} || length > 4096) throw std::runtime_error("Unsafe/invalid core read");
    for (uint32_t i = 0; i < length; i++) data[i] = static_cast<uint8_t>(0x40 + i);
    transfers.push_back("rx:" + std::to_string(length) + ":" + std::to_string(wide) + ":" + bytes(data, length));
    events.push_back(transfers.back());
}
static int fixture_open(const char *name, int flags, mode_t permissions)
{
    if (flags != (O_WRONLY | O_CREAT | O_TRUNC | O_SYNC) || permissions != (S_IRWXU | S_IRWXG | S_IRWXO)) throw std::runtime_error("Save flags differ");
    file_opened = name; events.push_back("save-open:" + file_opened);
    if (fail_save_open) { errno = EACCES; return -1; }
    return 7;
}
static ssize_t fixture_write(int fd, const void *data, size_t size)
{
    if (fd != 7 || size > 4096) throw std::runtime_error("Save size/descriptor invalid");
    if (fail_save_write) { events.push_back("save-write-failed"); errno = EIO; return -1; }
    temporary_payload.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    events.push_back("save-write:" + std::to_string(size)); return static_cast<ssize_t>(size);
}
static int fixture_close(int fd) { if (fd != 7) throw std::runtime_error("Close descriptor"); events.push_back("save-close"); if (fail_save_close) { errno=EIO; return -1; } return 0; }
static int fixture_rename(const char *from, const char *to)
{
    if (file_opened != from) throw std::runtime_error("Rename temp mismatch");
    if (fail_save_rename) { events.push_back("save-rename-failed"); errno=EIO; return -1; }
    saved_target = to; files[to] = temporary_payload; events.push_back("save-rename:" + saved_target); return 0;
}
static int fixture_unlink(const char *name) { events.push_back("save-unlink:" + std::string(name)); return 0; }
int FileCreatePath(const char *name) {
    if (std::strlen(name)>2098) throw std::runtime_error("Directory spy would exceed legacy capacity");
    events.push_back("mkdir-query:" + std::string(name)); return 1;
}
__attribute__((unused)) static int observed_sprintf(char *out, const char *format, ...)
{
    if (out != full_path || (std::strcmp(format,"%s/%s") && std::strcmp(format,"%s")))
        throw std::runtime_error("Unrecognized legacy format/buffer");
    va_list args; va_start(args,format);
    const char *a=va_arg(args,const char *);
    const char *b=std::strcmp(format,"%s") ? va_arg(args,const char *) : nullptr;
    va_end(args);
    if (!a || (std::strcmp(format,"%s")==0 ? false : !b)) throw Boundary{"bounded-null-root"};
    const size_t n=std::strlen(a)+(b ? 1+std::strlen(b) : 0);
    if (n>=sizeof(full_path)) throw Boundary{"bounded-legacy-path"};
    return b ? libc_formatter(out,sizeof(full_path),format,a,b) : libc_formatter(out,sizeof(full_path),format,a);
}
#if !HAVE_CHECKED_FILE
static char *make_fullpath(const char *name,int mode);
#endif
int FileOpen(fileTYPE *file, const char *name, char)
{
    const std::string requested = name;
#if !HAVE_CHECKED_FILE
    // Legacy FileOpenEx reaches this actual unchecked producer before opening.
    // Exercise it above the endpoint spy; its formatter guard ends unsafe cases.
    const std::string path=make_fullpath(name,0);
#else
    if (requested.empty() || requested[0]!='/') {
        if (null_root) throw Boundary{"bounded-null-root"};
    }
    const std::string path = !requested.empty() && requested[0]=='/' ? requested : storage_root+"/"+requested;
#endif
    events.push_back("load-open:" + path);
    if (path.find("/config/nvram/") != std::string::npos) nvm_reads++;
    auto found = files.find(path);
    if (found == files.end()) { errno = ENOENT; return 0; }
    opened_files[file] = path; file->size = found->second.size(); file->filp = reinterpret_cast<FILE *>(uintptr_t(1)); return 1;
}
int FileReadAdv(fileTYPE *file, void *data, int size, int)
{
    auto found = opened_files.find(file);
    if (found == opened_files.end() || size < 0 || size > 4096) throw std::runtime_error("Unsafe/invalid load");
    const auto &payload = files.at(found->second);
    const size_t length = std::min(static_cast<size_t>(size), payload.size());
    if (length) std::memcpy(data, payload.data(), length);
    events.push_back("load-read:" + std::to_string(length)); return static_cast<int>(length);
}
int FileClose(fileTYPE *file) { opened_files.erase(file); file->filp = nullptr; file->zip = nullptr; return 0; }
static struct stat64 *getPathStat(const char *) { static struct stat64 st; st.st_size = document.size(); return &st; }
static void set_arcade_root(const char *path) { events.push_back("arcade-root:" + std::string(path)); }
static const char *get_arcade_root(int) { forbidden("ROM root query"); return ""; }
static void ProgressMessage(const char * = nullptr, const char * = nullptr, int = 0, int = 0) {}
static void user_io_write_gameid(const char *, int, const char *) { forbidden("game ID branch"); }
static void set_ovr_buttons(const char *, int) { forbidden("buttons branch"); }
static void cheats_init_arcade(int a,int b) { if(a || b) forbidden("nonempty cheats branch"); events.push_back("cheats-start"); }
static void cheats_add_arcade(const char *, char *, size_t) { forbidden("cheats branch"); }
static void cheats_finalize_arcade() { events.push_back("cheats-end"); }
template<size_t N> static void strcpyz(char (&)[N], const char *) { forbidden("cheat-name branch"); }
extern "C" void MD5Init(MD5Context *c) { std::memset(c,0,sizeof(*c)); }
extern "C" void MD5Final(unsigned char *out, MD5Context *) { std::memset(out,0,16); }
static void rom_start(unsigned char index) { events.push_back("rom-start:" + std::to_string(index)); }
static void rom_finish(int ok, uint32_t, int index) { events.push_back("rom-finish:"+std::to_string(index)+":"+std::to_string(ok)); }
static int rom_file(const char *, uint32_t, int, int, int, MD5Context *) { forbidden("ROM file"); return 0; }
static int rom_data(const uint8_t *, int, int, MD5Context *) { forbidden("ROM data"); return 0; }
static int rom_patch(const uint8_t *, int, uint16_t, int) { forbidden("ROM patch"); return 0; }
static unsigned char *hexstr_to_char(const char *, size_t *) { forbidden("ROM/DIP hexadecimal data"); return nullptr; }
static void observe_file_return(const char *api,const char *name,int result)
{
    file_returns.push_back("{\"api\":"+quote(api)+",\"name\":"+quote(name)+",\"return_observed\":true,\"return_value\":"+std::to_string(result)+"}");
}
#include "arcade_nvram_production.inc"

static std::string nvm_document(const std::string &index, const std::string &size)
{
    return "<mister><nvram index=\"" + index + "\" size=\"" + size + "\"/></mister>";
}
static std::string leaf_name(std::string name)
{
    if (name.size()>=4 && !strcasecmp(name.c_str()+name.size()-4,".mra"))
        name.replace(name.size()-4,4,".nvm");
    return name;
}
static void select(const std::string &basename, const std::string &xml)
{
    document=xml; parser_seen=parser_completed=false; parser_result=-999;
    events.push_back("selection:"+basename);
    const int result=arcade_send_rom(("/media/fat/_Arcade/"+basename).c_str());
    snapshots.push_back("{\"basename\":"+quote(basename)+",\"name\":"+quote(nvram_name)+
        ",\"index\":"+std::to_string(nvram_idx)+",\"size\":"+std::to_string(nvram_size)+
        ",\"selector_return\":"+std::to_string(result)+",\"parser_completed\":"+
        (parser_completed ? "true":"false")+",\"parser_return\":"+std::to_string(parser_result)+"}");
    check(result==0 && parser_seen && parser_completed,"intact selector completed and actual parser return recorded");
}
static void seed(const std::string &basename, unsigned length)
{
    std::vector<uint8_t> data(length);
    for(unsigned i=0;i<length;i++) data[i]=static_cast<uint8_t>(i+1);
    files["/media/fat/config/nvram/"+leaf_name(basename)]=data;
}
static void save()
{
    save_started=true; save_completed=false;
    events.push_back("menu-save-call"); arcade_nvm_save(); events.push_back("menu-save-return");
    save_completed=true;
}
static size_t event_position(const std::string &prefix)
{
    for(size_t i=0;i<events.size();i++) if(events[i].find(prefix)==0) return i;
    return events.size();
}
static std::vector<std::string> nvm_io()
{
    std::vector<std::string> result;
    for(const auto &s:events) if(s.find("spi:")==0 || s.find("rx:")==0 || s.find("tx:")==0)
        result.push_back(s);
    return result;
}
static void overwrite_binding()
{
    nvram_idx=9; nvram_size=3;
    std::memcpy(nvram_name,"Other.nvm",10);
#if HAVE_NVRAM_BINDING
    std::memcpy(nvram_path,"/media/usb0/config/nvram/Other.nvm",33);
#endif
    events.push_back("same-thread-binding-overwrite");
}
static bool no_nvm_effects()
{
    return !allocation_seen && !nvm_reads && !count("tx:") && !count("rx:") &&
           !count("mkdir-query:") && !count("save-open:");
}
static void reject_selection(const std::string &basename, const std::string &xml)
{
    select(basename,xml); save();
    check(no_nvm_effects(),"rejected selection performs no NVRAM allocation/directory/file/core operation");
}
template<size_t N> struct Guarded {
    std::array<unsigned char,16> before;
    std::array<char,N> data;
    std::array<unsigned char,16> after;
    Guarded() { before.fill(0xa6); data.fill('Q'); after.fill(0x5b); std::memcpy(data.data(),"KEEP",5); }
    bool guards() const
    {
        return std::all_of(before.begin(),before.end(),[](unsigned char x){return x==0xa6;}) &&
               std::all_of(after.begin(),after.end(),[](unsigned char x){return x==0x5b;});
    }
    bool unchanged(const std::array<char,N> &prior) const { return guards() && !std::memcmp(data.data(),prior.data(),N); }
};
static bool excluded;
static void scenario(const std::string &name)
{
    if(name.find("ordinary-")==0) {
        if(name=="ordinary-full") seed("Game.mra",8);
        if(name=="ordinary-short") seed("Game.mra",3);
        if(name=="ordinary-empty") seed("Game.mra",0);
        select("Game.mra",nvm_document("4","8"));
        const bool positive=name=="ordinary-full" || name=="ordinary-short";
        check(nvram_idx==4 && nvram_size==8 && nvm_reads==1,"complete declaration loads once");
        check(count("tx:")==static_cast<unsigned>(positive),"only positive read causes download");
        if(positive) check(nvm_io()==std::vector<std::string>{"spi:85:4","spi:83:255",
            name=="ordinary-short" ? "tx:8:0:0102030000000000" : "tx:8:0:0102030405060708","spi:84","spi:83:0"},
            "actual helper exact download command/data order and zero tail");
        save();
        check(saved_target=="/media/fat/config/nvram/Game.nvm" && temporary_payload.size()==8 &&
              count("rx:")==1,"valid binding permits first save without prior positive file");
        check(event_position("nvm-new:")<event_position("mkdir-query:") &&
              event_position("rx:")<event_position("save-open:"),"allocation precedes mkdir and core upload precedes save");
    } else if(name.find("stale-")==0 || name=="malformed-before" || name=="unmatched-end") {
        select("A.mra",nvm_document("4","8"));
        const unsigned reads=nvm_reads, allocations=count("nvm-new:");
        std::string next="<mister/>";
        if(name=="stale-index-only") next="<mister><nvram index=\"9\"/></mister>";
        if(name=="stale-size-only") next="<mister><nvram size=\"3\"/></mister>";
        if(name=="stale-empty") next="<mister><nvram></nvram></mister>";
        if(name=="stale-open-error") fail_xml_open=true;
        if(name=="malformed-before") next="<mister><nvram index=\"9\"";
        if(name=="unmatched-end") next="<mister></nvram></mister>";
        select("B.mra",next); save();
        check(nvm_reads==reads && count("nvm-new:")==allocations && !count("rx:") && saved_target.empty(),
              "new absent/incomplete/open/error/unmatched selection cannot reuse prior binding");
        if(name=="stale-open-error") check(parser_result==0 && starts==1,"failed actual open precedes STARTDOC");
        if(name=="malformed-before") check(parser_result==0 && errors==1 && ends==starts,"actual parse failure and ENDDOC do not publish readiness");
    } else if(name=="malformed-after" || name=="unfinished-node" || name=="nested-nvram") {
        seed("Game.mra",8);
        std::string xml="<mister><nvram index=\"4\" size=\"8\"/><broken";
        if(name=="unfinished-node") xml="<mister><nvram index=\"4\" size=\"8\">";
        if(name=="nested-nvram") xml="<mister><nvram index=\"4\" size=\"8\"><nvram index=\"9\" size=\"3\"/></nvram></mister>";
        select("Game.mra",xml); save();
        check(!count("rx:") && saved_target.empty(),"error/unfinished/nested candidate retires save binding");
        check(nvm_reads==(name=="malformed-after" ? 1u:0u) &&
              count("tx:")==static_cast<unsigned>(name=="malformed-after"),"earlier completed load remains observed; unfinished/nested candidates do not load");
    } else if(name=="nested-switches" || name=="nested-rom" || name=="nested-cheats") {
        seed("Game.mra",8);
        const std::string tag=name=="nested-switches" ? "switches" : name=="nested-rom" ? "rom" : "cheats";
        select("Game.mra","<mister><nvram index=\"4\" size=\"8\"/><"+tag+"><nvram index=\"9\" size=\"3\"/></"+tag+"></mister>");
        check(nvm_reads==1 && count("tx:")==1 && nvram_idx==4 && nvram_size==8,
              "ignored contextual NVRAM neither reloads nor replaces existing same-document binding");
        save(); check(saved_target=="/media/fat/config/nvram/Game.nvm" && temporary_payload.size()==8,"ignored context preserves complete earlier binding");
    } else if(name=="repeated-complete" || name=="repeated-partial") {
        seed("Game.mra",8);
        const std::string next=name=="repeated-complete" ? "<nvram index=\"9\" size=\"3\"/>" : "<nvram index=\"9\"/>";
        select("Game.mra","<mister><nvram index=\"4\" size=\"8\"/>"+next+"</mister>");
        check(nvm_reads==(name=="repeated-complete" ? 2u:1u),"each complete declaration loads at close; partial cannot merge earlier size");
        save();
        if(name=="repeated-complete") {
            check(nvram_idx==9 && nvram_size==3 && count("rx:")==1 && temporary_payload.size()==3,"last complete declaration remains saveable");
            for(const std::string &special : {std::string("<!--nvram-->"),std::string("<![CDATA[nvram]]>"),std::string("<?nvram?>")})
            for(bool inside : {false,true}) {
                const unsigned reads=nvm_reads,rx=count("rx:"); saved_target.clear();
                const std::string xml=inside ? "<mister><nvram index=\"4\" size=\"8\">"+special+"</nvram></mister>" :
                    "<mister><nvram index=\"4\" size=\"8\"/>"+special+"</mister>";
                select("Game.mra",xml); save();
                check(nvm_reads==reads+1 && count("rx:")==rx+1 && saved_target=="/media/fat/config/nvram/Game.nvm" && temporary_payload.size()==8,
                    inside ? "non-element nvram content inside candidate does not invalidate real matching close" :
                             "non-element nvram content after valid declaration does not retire readiness");
            }
        } else check(!count("rx:") && saved_target.empty(),"later partial declaration disables prior readiness");
    } else if(name=="declaration-order") {
        seed("Game.mra",8);
        select("Game.mra","<mister><rom index=\"3\"/><nvram index=\"4\" size=\"8\"/><rom index=\"1\"/></mister>");
        check(count("tx:")==1 && event_position("rom-finish:3")<event_position("tx:") &&
              event_position("tx:")<event_position("rom-start:1"),"actual SAX close-time NVRAM transfer remains between earlier/later ROM callbacks");
    } else if(name.find("duplicate-")==0 || name=="attributes-reordered") {
        std::string attrs="index=\"4\" index=\"9\" size=\"8\"";
        if(name=="duplicate-case") attrs="index=\"4\" INDEX=\"9\" size=\"8\"";
        if(name=="duplicate-invalid-valid") attrs="index=\"junk\" index=\"9\" size=\"8\"";
        if(name=="duplicate-valid-invalid") attrs="index=\"9\" index=\"junk\" size=\"8\"";
        if(name=="attributes-reordered") attrs="size=\"8\" index=\"4\"";
        select("Game.mra","<mister><nvram "+attrs+"/></mister>"); save();
        if(name=="duplicate-valid-invalid") check(no_nvm_effects(),"last matching invalid attribute rejects complete candidate");
        else check(nvram_idx==(name=="attributes-reordered" ? 4:9) && nvram_size==8 && nvm_reads==1 && count("rx:")==1,
                   "actual stored attribute order applies last value validity and supports reordered fields");
    } else if(name.find("index-")==0 || name.find("size-")==0) {
        std::string index="4",size="8"; bool reject=false, disabled=false; int expected_index=4,expected_size=8;
        if(name=="index-zero") index="0",disabled=true;
        if(name=="index-255") index="255",expected_index=255;
        if(name=="index-256") index="256",reject=true;
        if(name=="index-negative") index="-1",reject=true;
        if(name=="index-prefix") index="9junk",reject=true;
        if(name=="index-hex") index="0x09",expected_index=9;
        if(name=="index-octal") index="010",expected_index=8;
        if(name=="index-spaces-plus") index="  +9  ",expected_index=9;
        if(name=="size-zero") size="0",disabled=true;
        if(name=="size-prefix") size="3junk",reject=true;
        if(name=="size-negative") size="-1",reject=true;
        if(name=="size-overflow") size="18446744073709551616",reject=true;
        if(name=="size-octal") size="010",expected_size=8;
        select("Game.mra",nvm_document(index,size)); save();
        if(reject || disabled) check(no_nvm_effects(),"invalid/range/zero numeric metadata cannot cause NVRAM operations");
        else check(nvram_idx==expected_index && nvram_size==expected_size && nvm_reads==1 && count("rx:")==1 &&
                   std::find(events.begin(),events.end(),"spi:85:"+std::to_string(expected_index))!=events.end(),
                   "strict accepted grammar/index bound preserves actual byte index");
    } else if(name=="recovery") {
        select("A.mra",nvm_document("4","8"));
        select("B.mra",nvm_document("256","8"));
        seed("C.mra",3); select("C.mra",nvm_document("9","3")); save();
        check(nvm_reads==2 && count("tx:")==1 && saved_target=="/media/fat/config/nvram/C.nvm" && temporary_payload.size()==3,
              "later independent valid MRA recovers after rejected selection");
    } else if(name=="captured-root") {
        seed("Game.mra",8); change_root_on_new=true;
        select("Game.mra",nvm_document("4","8")); const unsigned roots=root_calls; save();
        check(count("tx:")==1 && saved_target=="/media/fat/config/nvram/Game.nvm" && root_calls==roots,
              "root mutation after accepted target cannot redirect load/save or trigger save re-resolution");
    } else if(name=="snapshot-binding") {
        select("Game.mra",nvm_document("4","8")); change_binding_on_spi=true; save();
        check(saved_target=="/media/fat/config/nvram/Game.nvm" && temporary_payload.size()==8 &&
              transfers==std::vector<std::string>{"rx:8:0:4041424344454647"},
              "same-thread binding overwrite during SPI cannot alter captured target/index/length");
        select("Game.mra",nvm_document("4","8")); change_binding_on_new=true; save();
        check(saved_target=="/media/fat/config/nvram/Game.nvm" && temporary_payload.size()==8 &&
              count("rx:")==2 && transfers.back()=="rx:8:0:4041424344454647",
              "allocator callback overwrite cannot change snapshotted core size/path");
        const auto io=nvm_io();
        check(io.size()>=5 && std::vector<std::string>(io.end()-5,io.end())==std::vector<std::string>{
              "spi:85:4","spi:83:170","rx:8:0:4041424344454647","spi:84","spi:83:0"},
              "allocator callback overwrite preserves captured index and complete save command sequence");
    } else if(name=="allocation-load" || name=="allocation-save") {
        if(name=="allocation-load") fail_nvm_allocation=true;
        select("Game.mra",nvm_document("4","8"));
        if(name=="allocation-load") check(!nvm_reads && !count("tx:"),"null allocation precedes load/file/core endpoints");
        else fail_nvm_allocation=true;
        const unsigned dirs=count("mkdir-query:"),rx=count("rx:"),opens=count("save-open:");
        save();
        check(count("mkdir-query:")==dirs && count("rx:")==rx && count("save-open:")==opens && save_completed,
              "null payload allocation returns locally before directory/core/save endpoints");
        fail_nvm_allocation=false; save();
        check(saved_target=="/media/fat/config/nvram/Game.nvm" && temporary_payload.size()==8,"allocation rejection leaves fitting binding usable for later first save");
    } else if(name.find("save-")==0) {
        select("Game.mra",nvm_document("4","8"));
        fail_save_open=name=="save-open-failure"; fail_save_write=name=="save-write-failure";
        fail_save_close=name=="save-close-failure"; fail_save_rename=name=="save-rename-failure";
        save();
        check(save_completed && count("rx:")==1 && saved_target.empty(),"void save completes after core RX without claiming failed persistence");
        check(name=="save-open-failure" ? !count("save-unlink:") : count("save-unlink:")==1,
              "actual FileSave cleanup branch remains observed");
        fail_save_open=fail_save_write=fail_save_close=fail_save_rename=false; save();
        check(saved_target=="/media/fat/config/nvram/Game.nvm","valid binding permits retry after filesystem save failure");
    } else if(name=="large-legal-budget") {
        select("Game.mra",nvm_document("4","5000")); save();
        check(budget_rejected && !nvm_reads && !count("tx:") && !count("rx:") && !count("mkdir-query:") && save_completed,
              "fixture budget injects nonthrowing rejection before endpoints; no legal large payload success claimed");
    } else if(name=="number-builder" || name=="name-builder" || name=="target-builder") {
#if HAVE_NVRAM_BINDING
        if(name=="number-builder") {
            struct IntGuard { unsigned before; int value; unsigned after; } out{0x12345678,71,0xabcdef01};
            const char *bad[]={""," ","-1"," -0 ","+","0x","08","9junk","256","18446744073709551616"};
            for(const char *s:bad) { out.value=71; check(!arcade_nvm_number(s,255,&out.value) && out.value==71 &&
                out.before==0x12345678 && out.after==0xabcdef01,"numeric rejection preserves entire guarded output"); }
            check(!arcade_nvm_number(nullptr,255,&out.value) && !arcade_nvm_number("4",255,nullptr),"numeric null argument rejection");
            check(arcade_nvm_number("  +0xff \t",255,&out.value) && out.value==255,"whitespace/sign/base0 exact index limit");
            check(arcade_nvm_number("2147483647",INT_MAX,&out.value) && out.value==INT_MAX,"legal INTMAX accepted by actual pure numeric helper without payload allocation");
            out.value=71; check(!arcade_nvm_number("2147483648",INT_MAX,&out.value) && out.value==71,"one beyond signed-int range rejected before narrowing");
            check(arcade_nvm_number("5000",INT_MAX,&out.value) && out.value==5000 && !allocation_seen,"legal5000 accepted without arbitrary product cap");
        } else if(name=="name-builder") {
            Guarded<400> out; auto before=out.data;
            check(!arcade_nvm_name(nullptr,out.data.data(),400) && out.unchanged(before),"null name leaves full guarded output unchanged");
            check(!arcade_nvm_name("",out.data.data(),400) && out.unchanged(before),"empty name leaves full guarded output unchanged");
            check(!arcade_nvm_name("bad/name.mra",out.data.data(),400) && out.unchanged(before),"slash in leaf rejects without publication");
            check(!arcade_nvm_name("Game.mra",nullptr,400) && !arcade_nvm_name("Game.mra",out.data.data(),0) && out.unchanged(before),"null output/zero capacity rejection");
            const std::string exact=std::string(256,'N')+".mra";
            check(arcade_nvm_name(exact.c_str(),out.data.data(),261) && std::string(out.data.data())==leaf_name(exact) && out.guards(),"exact260-byte component with261 room");
            out.data.fill('Q'); std::memcpy(out.data.data(),"KEEP",5); before=out.data;
            check(!arcade_nvm_name(exact.c_str(),out.data.data(),260) && out.unchanged(before),"one-short builder capacity leaves all bytes unchanged");
            check(!arcade_nvm_name((exact+"N").c_str(),out.data.data(),400) && out.unchanged(before),"one beyond component bound leaves all bytes unchanged");
            std::memcpy(out.data.data(),"A.mra.extra.MRA",16);
            check(arcade_nvm_name(out.data.data(),out.data.data(),400) && std::string(out.data.data())=="A.mra.extra.nvm" && out.guards(),"complete output/input alias preserves final suffix identity");
            std::memcpy(out.data.data()+12,"Game%.mra",10);
            check(arcade_nvm_name(out.data.data()+12,out.data.data(),400) && std::string(out.data.data())=="Game%.nvm" && out.guards(),"overlapping substring alias preserves literal percent");
        } else {
            Guarded<2200> out; auto before=out.data;
            check(!arcade_nvm_target(nullptr,"Game.mra",out.data.data(),2200) && out.unchanged(before),"null root leaves complete guarded target unchanged");
            check(!arcade_nvm_target("relative","Game.mra",out.data.data(),2200) && out.unchanged(before),"relative root rejected before target publication");
            check(!arcade_nvm_target("","Game.mra",out.data.data(),2200) && out.unchanged(before),"empty root rejected before target publication");
            const std::string expected="/media/fat/config/nvram/Game.nvm";
            check(arcade_nvm_target("/media/fat","Game.mra",out.data.data(),expected.size()+1) && std::string(out.data.data())==expected && out.guards(),"exact-fit absolute target capacity");
            out.data=before;
            check(!arcade_nvm_target("/media/fat","Game.mra",out.data.data(),expected.size()) && out.unchanged(before),"one-short absolute target capacity");
            std::memcpy(out.data.data(),"/media/fat",11);
            check(arcade_nvm_target(out.data.data(),"Game.mra",out.data.data(),2200) && std::string(out.data.data())==expected && out.guards(),"target aliases captured root");
            std::memcpy(out.data.data()+12,"Game.mra",9);
            check(arcade_nvm_target("/media/fat",out.data.data()+12,out.data.data(),2200) && std::string(out.data.data())==expected && out.guards(),"target overlaps input leaf");
            const std::string root="/"+std::string(2076,'R');
            check(arcade_nvm_target(root.c_str(),"Game.mra",out.data.data(),2100) && std::strlen(out.data.data())==2099 && out.guards(),"exact full2099 target bound");
            out.data=before;
            check(!arcade_nvm_target((root+"R").c_str(),"Game.mra",out.data.data(),2100) && out.unchanged(before),"one beyond full path bound leaves all bytes unchanged");
            fail_scalar_allocation=true;
            const int rc=arcade_nvm_target("/media/fat","Game.mra",out.data.data(),2200);
            const int result_errno=errno; fail_scalar_allocation=false;
            check(!rc && result_errno==ENOMEM && out.unchanged(before),"target allocation failure leaves complete output unchanged");
        }
#else
        excluded=true; check(true,"new private helper unavailable in selected historical source");
#endif
    } else if(name=="root-null" || name=="root-relative" || name=="root-path-reject") {
        if(name=="root-null") null_root=true;
        if(name=="root-relative") storage_root="relative";
        if(name=="root-path-reject") storage_root="/"+std::string(2099,'R');
        reject_selection("Game.mra",nvm_document("4","8"));
    } else if(name=="name-empty" || name=="name-over-component") {
        reject_selection(name=="name-empty" ? "" : std::string(257,'G')+".mra",nvm_document("4","8"));
    } else if(name=="name-distinct") {
        const std::string a=std::string(250,'G')+"A.mra",b=std::string(250,'G')+"B.mra";
        select(a,nvm_document("4","8")); save(); const std::string first=saved_target;
        select(b,nvm_document("4","8")); save();
        check(first=="/media/fat/config/nvram/"+leaf_name(a) && saved_target=="/media/fat/config/nvram/"+leaf_name(b) &&
              first!=saved_target && files.count(first)==1 && files.count(saved_target)==1,"distinct complete255-byte names retain two payload identities");
    } else {
        std::string basename;
        if(name=="literal-percent") basename="Game%.mra";
        else if(name=="escaped-percent") basename="Game%%.mra";
        else if(name=="format-n") basename="Game%n.mra";
        else if(name=="name-internal-suffix") basename="A.mra.extra.mra";
        else if(name=="name-case-suffix") basename="Game.MrA";
        else if(name=="name-extensionless") basename="Game";
        else if(name.find("name-")==0) {
            const unsigned length=static_cast<unsigned>(std::stoul(name.substr(5)));
            basename=std::string(length-4,'G')+".mra";
        } else throw std::runtime_error("Unknown scenario");
        select(basename,nvm_document("4","8")); save();
        check(std::string(nvram_name)==leaf_name(basename) && saved_target=="/media/fat/config/nvram/"+leaf_name(basename) &&
              temporary_payload.size()==8,"full literal basename and terminal suffix preserve complete saved identity");
    }
}
int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    const std::string name=argv[1]; bool trapped=false,case_body_completed=false; std::string trap;
    try { scenario(name); case_body_completed=true; }
    catch(const Boundary &b) { trapped=true; trap=b.kind; check(false,"historical unsafe boundary ended case before libc/allocation"); }
    catch(const std::bad_array_new_length &) { trapped=true; trap="array-length-before-allocator"; check(false,"historical array length rejection ends scenario"); }
    catch(const std::bad_alloc &) { fail_scalar_allocation=false; if(!fail_nvm_allocation) return 2;
        trapped=true; trap="throwing-array-allocation"; check(false,"historical throwing payload allocation ends scenario"); }
    catch(const std::exception &e) { fail_scalar_allocation=false; std::fprintf(stderr,"Fixture error: %s\n",e.what()); return 2; }
    if(trapped) check(!save_completed && saved_target.empty(),"ended trap has no invented later persistent write or save return");
    check(!fpga_enabled,"all observed core helper scopes are complete");
    std::printf("JSON {\"case\":%s,\"checks\":%u,\"failures\":%u,\"excluded\":%s,\"case_body_completed\":%s,\"trapped\":%s,\"trap\":%s,\"int_bits\":%zu,\"long_bits\":%zu,\"size_t_bits\":%zu,\"final_index\":%d,\"final_size\":%d,\"final_name\":%s,\"parser_completed\":%s,\"parser_return\":%s,\"save_started\":%s,\"save_completed\":%s,\"allocation_rejection_injected\":%s,\"allocation_rejections\":%u,\"allocation_seen\":%s,\"allocation_bytes\":%s,\"root_calls\":%u,\"nvm_load_opens\":%u,\"saved_target\":%s,\"snapshots\":%s,\"numbers\":%s,\"file_returns\":%s,\"transfers\":%s,\"events\":%s}\n",
        quote(name).c_str(),checks,failures,excluded?"true":"false",case_body_completed?"true":"false",trapped?"true":"false",quote(trap).c_str(),
        sizeof(int)*CHAR_BIT,sizeof(unsigned long)*CHAR_BIT,sizeof(size_t)*CHAR_BIT,nvram_idx,nvram_size,quote(nvram_name).c_str(),
        parser_completed?"true":"false",parser_completed?std::to_string(parser_result).c_str():"null",
        save_started?"true":"false",save_completed?"true":"false",allocation_rejections?"true":"false",allocation_rejections,
        allocation_seen?"true":"false",allocation_seen?quote(std::to_string(allocation_bytes)).c_str():"null",root_calls,nvm_reads,
        quote(saved_target).c_str(),array(snapshots,true).c_str(),array(parsed_numbers,true).c_str(),array(file_returns,true).c_str(),array(transfers).c_str(),array(events).c_str());
    return failures?1:0;
}
