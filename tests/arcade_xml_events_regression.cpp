#include <algorithm>
#include <cctype>
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

static std::string document, opened_target, saved_target, requested_xml;
static std::vector<std::string> events, parser_events, transfers, file_returns;
static std::vector<uint8_t> saved_bytes, temporary_bytes;
static std::map<std::string,std::vector<uint8_t>> files;
static std::map<fileTYPE*,std::string> handles;
static std::vector<unsigned> spi_bytes;
static bool fail_xml_open, parser_completed, fpga_enabled;
static int parser_result = -999;
static unsigned streams, checks, failures, active_index;
static const char *getRootDir() { return "/media/fat"; }
static std::string quote(const std::string &s)
{
    std::string out="\""; const char hex[]="0123456789abcdef";
    for(unsigned char c:s) {
        if(c=='"' || c=='\\') { out+='\\'; out+=c; }
        else if(c=='\n') out+="\\n";
        else if(c=='\r') out+="\\r";
        else if(c=='\t') out+="\\t";
        else if(c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=c;
    }
    return out+'"';
}
static std::string array(const std::vector<std::string> &v,bool raw=false)
{
    std::string out="[";
    for(size_t i=0;i<v.size();i++) { if(i) out+=','; out+=raw?v[i]:quote(v[i]); }
    return out+"]";
}
static std::string bytes(const uint8_t *data,size_t size)
{
    std::string out; const char hex[]="0123456789abcdef";
    for(size_t i=0;i<size;i++) { out+=hex[data[i]>>4]; out+=hex[data[i]&15]; }
    return out;
}
static void check(bool result,const char *name)
{
    checks++; if(!result) { failures++; std::printf("UNEXPECTED: %s\n",name); }
}
static void forbidden(const char *name) { throw std::runtime_error(std::string("Unselected lower branch: ")+name); }
template<size_t N, class... Args> static int observed_sprintf(char (&out)[N], const char *format, Args... args)
{
    int result=std::snprintf(out,N,format,args...);
    if(result<0 || static_cast<size_t>(result)>=N) forbidden("historical sprintf capacity");
    return result;
}
static int formatter(char *out,size_t cap,const char *format,...)
{
    va_list args; va_start(args,format); int result=std::vsnprintf(out,cap,format,args); va_end(args); return result;
}
static int observed_snprintf(char *out,size_t cap,const char *format)
{
    if(std::strchr(format,'%')) forbidden("dynamic percent format");
    return formatter(out,cap,"%s",format);
}
template<class... Args> static int observed_snprintf(char *out,size_t cap,const char *format,Args... args)
{
    const char *allowed[]={"%s","%s.mra","md5 mismatch for rom %d","%s\n%s not found","XML parse: %s: ERROR %d\n"};
    if(std::find_if(std::begin(allowed),std::end(allowed),[&](const char *s){ return !std::strcmp(s,format); })==std::end(allowed)) forbidden("unknown format");
    return formatter(out,cap,format,args...);
}
template<size_t N> static char *observed_strcat(char (&out)[N],const char *s)
{
    if(std::strlen(out)+std::strlen(s)>=N) forbidden("unbounded strcat");
    return std::strcat(out,s);
}
static unsigned long observed_strtoul(const char *s,char **end,int base) { return std::strtoul(s,end,base); }
extern "C" FILE *nvram_fixture_fopen(const char *name,const char *mode)
{
    if(std::strcmp(mode,"rt")) forbidden("parser mode");
    requested_xml=name; events.push_back("xml-open:"+requested_xml);
    if(fail_xml_open) { errno=ENOENT; return nullptr; }
    FILE *stream=std::tmpfile();
    if(!stream || std::fwrite(document.data(),1,document.size(),stream)!=document.size()) forbidden("disposable stream");
    std::rewind(stream); streams++; return stream;
}
static int xml_send_rom(XMLEvent,const XMLNode*,char*,int,SAX_Data*);
static int observe_xml(XMLEvent event,const XMLNode *node,char *text,int n,SAX_Data *sd)
{
    arc_struct *info=static_cast<arc_struct*>(sd->user);
    const std::string before=info->data ? info->data->content : "";
    const int result=xml_send_rom(event,node,text,n,sd);
    parser_events.push_back("{\"event\":"+std::to_string(event)+",\"type\":"+(node?std::to_string(node->tag_type):"null")+
        ",\"tag\":"+(node?quote(node->tag):"null")+",\"text\":"+(text?quote(text):"null")+
        ",\"buffer_before\":"+quote(before)+",\"buffer_after\":"+quote(info->data?info->data->content:"")+
        ",\"inside_rom_after\":"+std::to_string(info->insiderom)+",\"inside_switches_after\":"+std::to_string(info->insidesw)+
        ",\"dip_count_after\":"+std::to_string(switches.dip_num)+",\"callback_return\":"+std::to_string(result)+"}");
    return result;
}
static int observed_parse_file(const char *name,const SAX_Callbacks *callbacks,void *user)
{
    if(callbacks->all_event!=xml_send_rom) forbidden("callback replacement");
    SAX_Callbacks observer=*callbacks; observer.all_event=observe_xml;
    parser_result=XMLDoc_parse_file_SAX(name,&observer,user); parser_completed=true;
    events.push_back("xml-return:"+std::to_string(parser_result)); return parser_result;
}
static void EnableFpga() { if(fpga_enabled) forbidden("nested SPI"); fpga_enabled=true; spi_bytes.clear(); }
static void spi8(unsigned char value) { if(!fpga_enabled) forbidden("SPI inactive"); spi_bytes.push_back(value); }
static void spi_w(uint16_t) { forbidden("nonzero core address"); }
static void DisableFpga()
{
    if(!fpga_enabled) forbidden("inactive SPI disable"); fpga_enabled=false;
    std::string out="spi"; for(unsigned v:spi_bytes) out+=":"+std::to_string(v);
    if(spi_bytes.size()==2 && spi_bytes[0]==FIO_FILE_INDEX) active_index=spi_bytes[1];
    events.push_back(out);
}
static void spi_write(const uint8_t *data,uint32_t length,int wide)
{
    if(!fpga_enabled || spi_bytes!=std::vector<unsigned>{FIO_FILE_TX_DAT} || length>4096) forbidden("invalid core write");
    const std::string row="{\"index\":"+std::to_string(active_index)+",\"length\":"+std::to_string(length)+",\"wide\":"+std::to_string(wide)+",\"bytes\":"+quote(bytes(data,length))+"}";
    transfers.push_back(row); events.push_back("tx:"+std::to_string(active_index)+":"+bytes(data,length));
}
static void spi_read(uint8_t*,uint32_t,int) { forbidden("unselected NVRAM core read"); }
static int shmem_put(uint32_t,uint32_t,void*) { forbidden("shared memory ROM"); return 0; }
static int fixture_open(const char *name,int flags,mode_t mode)
{
    if(flags!=(O_WRONLY|O_CREAT|O_TRUNC|O_SYNC) || mode!=(S_IRWXU|S_IRWXG|S_IRWXO)) forbidden("save flags");
    opened_target=name; events.push_back("save-open:"+opened_target); return 7;
}
static ssize_t fixture_write(int fd,const void *data,size_t size)
{
    if(fd!=7 || size>8) forbidden("save payload");
    temporary_bytes.assign(static_cast<const uint8_t*>(data),static_cast<const uint8_t*>(data)+size);
    events.push_back("save-write:"+bytes(temporary_bytes.data(),temporary_bytes.size())); return static_cast<ssize_t>(size);
}
static int fixture_close(int fd) { if(fd!=7) forbidden("save descriptor"); events.push_back("save-close"); return 0; }
static int fixture_rename(const char *from,const char *to)
{
    if(opened_target!=from) forbidden("save rename identity");
    saved_target=to; saved_bytes=temporary_bytes; files[to]=saved_bytes; events.push_back("save-rename:"+saved_target); return 0;
}
static int fixture_unlink(const char *name) { events.push_back(std::string("save-unlink:")+name); return 0; }
__attribute__((unused)) static int isPathDirectoryPrepared(char *path,int = 1) { events.push_back(std::string("private-directory-query-spy:")+path); return 0; }
__attribute__((unused)) static int fixture_mkdir(const char *path,mode_t mode)
{
    if(path[0]!='/' || mode!=(S_IRWXU|S_IRWXG|S_IRWXO)) forbidden("mkdir identity/mode");
    events.push_back(std::string("mkdir:")+path); return 0;
}
int FileCreatePath(const char *path) {
    if(std::strcmp(path,"config/dips") && std::strcmp(path,"config/dips/")) forbidden("unselected directory");
    events.push_back(std::string("legacy-dip-directory-spy:")+path); return 1;
}
int FileOpen(fileTYPE *file,const char *name,char)
{
    events.push_back(std::string("load-open:")+name);
    if(name[0]!='/' && std::strcmp(name,"config/dips/Example.dip")) forbidden("unselected relative load target");
    auto found=files.find(name); if(found==files.end()) { errno=ENOENT; return 0; }
    handles[file]=name; file->size=found->second.size(); file->filp=reinterpret_cast<FILE*>(uintptr_t(1)); return 1;
}
int FileReadAdv(fileTYPE *file,void *data,int size,int)
{
    if(!handles.count(file) || size<0 || size>8) forbidden("load size/handle");
    const auto &payload=files.at(handles.at(file)); size_t count=std::min(payload.size(),static_cast<size_t>(size));
    if(count) std::memcpy(data,payload.data(),count); events.push_back("load-read:"+std::to_string(count)); return static_cast<int>(count);
}
int FileClose(fileTYPE *file) { handles.erase(file); file->filp=nullptr; file->zip=nullptr; return 0; }
static struct stat64 *getPathStat(const char*) { static struct stat64 st; st.st_size=document.size(); return &st; }
static void set_arcade_root(const char *path) { events.push_back(std::string("arcade-root:")+path); }
static const char *get_arcade_root(int) { forbidden("external ROM root"); return ""; }
static void ProgressMessage(const char* = nullptr,const char* = nullptr,int = 0,int = 0) {}
static void user_io_write_gameid(const char*,int,const char*) { forbidden("game ID"); }
static void set_ovr_buttons(const char*,int) { forbidden("buttons"); }
static void cheats_init_arcade(int,int) { forbidden("cheats"); }
static void cheats_add_arcade(const char*,char*,size_t) { forbidden("cheats"); }
static void cheats_finalize_arcade() { forbidden("cheats"); }
template<size_t N> static void strcpyz(char (&)[N],const char*) { forbidden("cheat name"); }
static int rom_file(const char*,uint32_t,int,int,int,MD5Context*) { forbidden("external ROM file"); return 0; }
static int rom_patch(const uint8_t*,int,uint16_t,int) { forbidden("ROM patch"); return 0; }
static int fixture_hex_allocation_diagnostic(const char*,size_t) { forbidden("hex allocation-failure diagnostic; untested inherited format"); return 0; }
static void observe_file_return(const char *api,const char *name,int result)
{
    const int saved_errno=errno;
    file_returns.push_back("{\"api\":"+quote(api)+",\"name\":"+quote(name)+",\"return_observed\":true,\"return_value\":"+std::to_string(result)+",\"errno\":"+std::to_string(saved_errno)+"}");
    errno=saved_errno;
}
#include "arcade_nvram_production.inc"

static std::string metadata()
{
    std::vector<std::string> dips;
    for(int i=0;i<switches.dip_num && i<64;i++) {
        const dip_struct &d=switches.dip[i];
        dips.push_back("{\"name\":"+quote(d.name)+",\"num\":"+std::to_string(d.num)+",\"start\":"+std::to_string(d.start)+",\"size\":"+std::to_string(d.size)+",\"mask\":"+quote(std::to_string(d.mask))+"}");
    }
    return "{\"name\":"+quote(switches.name)+",\"count\":"+std::to_string(switches.dip_num)+",\"default\":"+quote(std::to_string(switches.dip_def))+
        ",\"current\":"+quote(std::to_string(switches.dip_cur))+",\"saved\":"+quote(std::to_string(switches.dip_saved))+",\"dips\":"+array(dips,true)+"}";
}
static std::string rom(const std::string &part,int index=1)
{
    return "<rom index=\""+std::to_string(index)+"\"><part>"+part+"</part></rom>";
}
int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    try {
        const std::string name=argv[1]; std::string content, observed_rom, control_rom;
        int expected_dips=0; bool divergence=false;
        const std::string sw="<switches default=\"01\"><dip name=\"Mode\" bits=\"0\" ids=\"Off,On\">";
        if(name=="inline-ordinary") content=rom("112233");
        else if(name=="inline-comment") content=rom("11<!--note-->2233");
        else if(name=="inline-pi") content=rom("11<?note audit?>2233");
        else if(name=="inline-cdata") content=rom("11<![CDATA[22]]>33");
        else if(name=="inline-cdata-only") content=rom("<![CDATA[112233]]>");
        else if(name=="inline-comment-part") content=rom("11<!--part-->2233");
        else if(name=="inline-comment-rom") content=rom("11<!--rom-->2233");
        else if(name=="inline-pi-rom") content=rom("11<?rom?>2233");
        else if(name=="inline-cdata-multiple") content=rom("11<![CDATA[22]]><![CDATA[33]]>");
        else if(name=="inline-empty-cdata") content=rom("11<![CDATA[]]>2233");
        else if(name=="inline-two-parts") content="<rom index=\"1\"><part>11</part><!--rom--><part>2233</part></rom>";
        else if(name=="inline-recovery") content=rom("11<!--note-->22")+rom("aabb",2);
        else if(name=="inline-empty-part") content="<rom index=\"1\"><part/></rom>";
        else if(name=="dip-ordinary") { content=sw+"</dip></switches>"; expected_dips=1; }
        else if(name=="dip-self") { content="<switches default=\"01\"><dip name=\"Mode\" bits=\"0\" ids=\"Off,On\"/></switches>"; expected_dips=1; }
        else if(name=="dip-comment-note") { content=sw+"<!--note--></dip></switches>"; expected_dips=1; }
        else if(name=="dip-comment-dip") { content=sw+"<!--dip--></dip></switches>"; expected_dips=1; }
        else if(name=="dip-comment-switches") { content=sw+"</dip></switches><!--switches-->"; expected_dips=1; }
        else if(name=="dip-pi-dip") { content=sw+"<?dip?></dip></switches>"; expected_dips=1; }
        else if(name=="dip-pi-switches") { content=sw+"</dip></switches><?switches?>"; expected_dips=1; }
        else if(name=="dip-cdata-dip") { content=sw+"<![CDATA[dip]]></dip></switches>"; expected_dips=1; }
        else if(name=="dip-cdata-switches") { content=sw+"</dip></switches><![CDATA[switches]]>"; expected_dips=1; }
        else if(name=="fresh-absent") content="<rbf>Example</rbf>";
        else if(name=="fresh-open-error") { content=""; fail_xml_open=true; }
        else return 2;
        if(name.find("inline-")==0 && name!="inline-empty-part") control_rom=name=="inline-recovery"?"1122aabb":"112233";
        document="<misterromdescription>"+content+"</misterromdescription>";
        const int result=arcade_send_rom("/media/fat/_Arcade/Example.mra");
        check(result==0 && parser_completed,"actual selector and file parser completed");
        check(parser_result==(fail_xml_open?0:1),"actual parser result matches bounded valid/open-error control");
        check(streams==(fail_xml_open?0U:1U),"exact disposable file stream count");
        check(switches.dip_num==expected_dips,"only real DIP elements affect count");
        std::string assembled;
        for(const auto &event:events) if(event.find("tx:1:")==0 || event.find("tx:2:")==0) assembled+=event.substr(5);
        check(assembled==control_rom,"actual ROM helper transmits complete XML character data");
        if(expected_dips) {
            check(switches.dip_def==1 && switches.dip_cur==1 && switches.dip_saved==1,"real DIP default/load marker preserved");
            check(!std::strcmp(switches.dip[0].name,"Mode") && switches.dip[0].num==2 && switches.dip[0].start==0 && switches.dip[0].size==1 && switches.dip[0].mask==1,"real Mode DIP metadata preserved");
        }
        unsigned tx_frames=0;
        for(const auto &event:events) {
            if(event.find("tx:1:")==0) { check(event=="tx:1:"+(name=="inline-recovery"?std::string("1122"):control_rom),"index1 transfer framing and payload"); tx_frames++; }
            if(event.find("tx:2:")==0) { check(name=="inline-recovery" && event=="tx:2:aabb","index2 recovery transfer framing and payload"); tx_frames++; }
        }
        check(tx_frames==(control_rom.empty()?0U:name=="inline-recovery"?2U:1U),"exact ROM transfer count");
        check(file_returns.size()==1 && file_returns[0].find("\"api\":\"FileLoadConfig\"")!=std::string::npos && file_returns[0].find("\"name\":\"dips/Example.dip\"")!=std::string::npos && file_returns[0].find("\"return_value\":0")!=std::string::npos,"actual missing-DIP load identity and return observed");
        const std::string before=metadata();
        if(expected_dips) switches.dip_cur^=1;
        events.push_back("dip-save-call"); arcade_sw_save(); events.push_back("dip-save-return");
        if(expected_dips) {
            check(saved_target=="/media/fat/config/dips/Example.dip","intact DIP/config save exact captured target");
            check(saved_bytes==std::vector<uint8_t>(8,0) && switches.dip_cur==0 && switches.dip_saved==0,"actual complete save status updates marker");
            check(file_returns.size()==2 && file_returns.back().find("\"return_value\":8")!=std::string::npos &&
                file_returns.back().find(HAVE_CHECKED_FILE?"\"api\":\"FileSaveConfig\"":"\"api\":\"FileSave\"")!=std::string::npos,
                "actual DIP save nested return observed");
        } else check(saved_target.empty(),"intact DIP save was a finite no-op");
        observed_rom=assembled; divergence=failures!=0;
        std::printf("JSON %s\n",("{\"case\":"+quote(name)+",\"checks\":"+std::to_string(checks)+",\"failures\":"+std::to_string(failures)+
            ",\"case_body_completed\":true,\"trapped\":false,\"excluded\":false,\"selector_return_observed\":true,\"selector_return\":"+std::to_string(result)+",\"parser_return\":"+std::to_string(parser_result)+
            ",\"document\":"+quote(document)+",\"metadata_before_save\":"+before+",\"metadata_after_save\":"+metadata()+",\"dip_save_void_call_completed\":true,\"saved_target\":"+quote(saved_target)+
            ",\"saved_bytes\":"+quote(bytes(saved_bytes.data(),saved_bytes.size()))+",\"observed_rom_bytes\":"+quote(assembled)+",\"ordinary_control_rom_bytes\":"+quote(control_rom)+
            ",\"control_divergence\":"+(divergence?"true":"false")+",\"events\":"+array(events)+",\"parser_events\":"+array(parser_events,true)+
            ",\"transfers\":"+array(transfers,true)+",\"file_returns\":"+array(file_returns,true)+"}").c_str());
        return failures?1:0;
    } catch(const std::exception &error) { std::fprintf(stderr,"Fixture error: %s\n",error.what()); return 2; }
}
