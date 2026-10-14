#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include "sxmlc.h"
#include "mra_path_types.inc"

struct Boundary : std::runtime_error { using std::runtime_error::runtime_error; };
static unsigned checks, failures, hidden, games, parser_calls, streams, directory_calls, core_calls, directory_closes;
static bool wrapper_completed, parser_observed, target_differs, copy_ended;
static bool excluded, fail_once;
static bool path_observed,path_nonnull,rbf_observed,rbf_nonnull,root_return_observed;
static int root_return=-999;
static std::vector<std::string> failure_labels, messages;
static int wrapper_return=-999, parser_return=-999;
static std::string wanted_xml, prefix_xml, requested_xml, rbf_fragment="TestCore", selected_leaf="TestCore_20260101.rbf";
static std::string actual_directory, wanted_directory, actual_core, wanted_core, handed_xml, boundary, path_input, path_output;
static std::vector<std::string> events;
static size_t entry_index;
static bool overwrite_shared;
static bool games_found,games_unterminated;
static std::string games_reply;
struct Entry { std::string name;unsigned char type=DT_REG; };
static std::vector<Entry> declared_entries;
static bool entries_declared, directory_missing, xml_missing;
static std::string xml_body;
static unsigned root_calls, allocation_faults;
static bool fail_after_root, fail_after_parser, fail_after_entries, mutate_input_at_root, change_storage_at_open;
static bool replace_root, null_root;
static std::string root_reply;
static const char *last_handed_xml, *last_handed_core;
static std::vector<std::string> path_failures;
static std::string guard_before_hex,guard_after_hex,root_before_hex;
static size_t fail_size;
static unsigned core_open_calls,osd_disable_calls;
static unsigned parser_errors;
static int core_return=-999;
static bool core_return_observed;
static std::vector<std::string> core_messages;
struct mra_path_stat64 { long long st_size; };
typedef int64_t __off64_t;
static_assert(sizeof(mra_path_stat64::st_size)==8&&sizeof(__off64_t)==8,"Core fixture signed size types remain64-bit");
int fpga_load_rbf(const char *,const char *,const char *);
__attribute__((noinline)) void *operator new(std::size_t size)
{
    if(fail_once&&(!fail_size||size==fail_size)) { fail_once=false;++allocation_faults;throw std::bad_alloc(); }
    if(void *out=std::malloc(size?size:1))return out;
    throw std::bad_alloc();
}
__attribute__((noinline)) void *operator new[](std::size_t size) { return ::operator new(size); }
__attribute__((noinline)) void operator delete(void *out) noexcept { std::free(out); }
__attribute__((noinline)) void operator delete[](void *out) noexcept { std::free(out); }
__attribute__((noinline)) void operator delete(void *out,std::size_t) noexcept { std::free(out); }
__attribute__((noinline)) void operator delete[](void *out,std::size_t) noexcept { std::free(out); }
#if !HAVE_MRA_PATHS
static int (*volatile forwarded_path_format)(char *,size_t,const char *,...)=std::snprintf;
#endif
using Callback = decltype(SAX_Callbacks::all_event);
static Callback forwarded;

static std::string quote(const std::string &s)
{
    const char hex[]="0123456789abcdef"; std::string out="\"";
    for(unsigned char c:s) {
        if(c=='"'||c=='\\') { out+='\\';out+=c; }
        else if(c=='\n')out+="\\n";
        else if(c<32||c>=127) { out+="\\u00";out+=hex[c>>4];out+=hex[c&15]; }
        else out+=c;
    }
    return out+'"';
}
static std::string array(const std::vector<std::string> &items)
{
    std::string out="["; for(size_t i=0;i<items.size();++i) { if(i)out+=',';out+=quote(items[i]); } return out+"]";
}
static std::string hex_bytes(const void *data,size_t length)
{
    const char hex[]="0123456789abcdef";const unsigned char *bytes=static_cast<const unsigned char*>(data);std::string out;
    for(size_t i=0;i<length;++i) { out+=hex[bytes[i]>>4];out+=hex[bytes[i]&15]; }
    return out;
}
static const char *boolean(bool value) { return value?"true":"false"; }
static void check(bool value,const char *label)
{
    ++checks; if(!value) { ++failures;failure_labels.emplace_back(label);std::printf("FAIL: %s\n",label); }
}
static void require(bool value,const char *label) { if(!value)throw std::runtime_error(label); }
static void observe_root(const char *root) { events.emplace_back(std::string("root:")+root); }
static const char *root_boundary(const char *root)
{
    ++root_calls;observe_root(root);
    if(mutate_input_at_root)std::strcpy(full_path,"Changed/Other.xml");
    if(fail_after_root) { fail_after_root=false;fail_once=true; }
    return null_root?nullptr:replace_root?root_reply.c_str():root;
}
#if HAVE_MRA_PATHS
static void observe_path_failure(int code,int status)
{
    path_failures.emplace_back("errno:"+std::to_string(code)+"|path-status:"+std::to_string(status));
}
#endif
static void observe_rbf_path(const char *name,const char *out) { path_observed=true;path_nonnull=out!=nullptr;path_input=name;path_output=out?out:"";events.emplace_back("rbf-path:"+path_output); }
static void observe_get_rbf(const char *out) { rbf_observed=true;rbf_nonnull=out!=nullptr;events.emplace_back(out?std::string("rbf-return:")+out:"rbf-return:NULL"); }
#if !HAVE_MRA_PATHS
static char *bounded_xml_copy(char *out,const char *in)
{
    if(std::strlen(in)>=1024)throw Boundary("xml_load legacy absolute copy ends before write");
    return std::strcpy(out,in);
}
static char *bounded_root_copy(char *out,const char *in)
{
    if(std::strlen(in)>=1024)throw Boundary("set_arcade_root legacy copy ends before write");
    return std::strcpy(out,in);
}
#endif
static void MenuHide() { ++hidden;events.emplace_back("menu-hide"); }
static void Info(const char *message) { messages.emplace_back(message);events.emplace_back(std::string("info:")+message); }
static void Info(const char *message,int duration) { core_messages.emplace_back(std::string(message)+"|"+std::to_string(duration));events.emplace_back(std::string("core-info:")+message); }
static void OsdDisable() { ++osd_disable_calls;events.emplace_back("osd-disable"); }
static int path_core_open(const char *name,int flags)
{
    require(flags==O_RDONLY,"Actual core loader opens read-only");++core_open_calls;
    check(std::string(name)==actual_core,"Actual core open receives complete handed target");events.emplace_back(std::string("core-open:")+name);errno=ENOENT;return -1;
}
static void fpga_core_reset(int) { throw std::runtime_error("Unexpected FPGA reset endpoint"); }
static void make_env(const char *,const char *) { throw std::runtime_error("Unexpected core environment endpoint"); }
static void do_bridge(int) { throw std::runtime_error("Unexpected FPGA bridge endpoint"); }
static void reboot(int) { throw std::runtime_error("Unexpected reboot endpoint"); }
static int path_core_fstat64(int,struct mra_path_stat64 *) { throw std::runtime_error("Unexpected successful core descriptor"); }
static int path_core_read(int,void *,size_t) { throw std::runtime_error("Unexpected core read endpoint"); }
static int path_core_close(int) { throw std::runtime_error("Unexpected core close endpoint"); }
static int socfpga_load(void *,__off64_t) { throw std::runtime_error("Unexpected physical FPGA load endpoint"); }
static void app_restart(const char *,const char *) { throw std::runtime_error("Unexpected restart endpoint"); }
static int findGamesDir(char *name,size_t capacity)
{
    require(capacity==1024&&!std::strcmp(name,"mame"),"Selected findGamesDir endpoint");
    ++games;events.emplace_back("find-games:mame");
    if(overwrite_shared)std::strcpy(full_path,"/unrelated/borrowed-buffer.mra");
    if(games_found) {
        if(games_unterminated)std::memset(name,'g',capacity);
        else { require(games_reply.size()<capacity,"Declared found parent fits");std::memcpy(name,games_reply.c_str(),games_reply.size()+1); }
    }
    return games_found?1:0;
}
static int observed_core_handoff(const char *name,const char *config,const char *xml)
{
    require(!config&&xml,"Selected core handoff endpoint");
    ++core_calls;actual_core=name;handed_xml=xml;events.emplace_back("core-handoff:"+actual_core+"|"+handed_xml);
    last_handed_xml=xml;last_handed_core=name;
    const int result=fpga_load_rbf(name,config,xml);core_return=result;core_return_observed=true;return result;
}
static DIR *observed_opendir(const char *directory)
{
    ++directory_calls;actual_directory=directory;entry_index=0;events.emplace_back("directory:"+actual_directory);
    if(directory_missing) { errno=ENAMETOOLONG;return nullptr; }
    return reinterpret_cast<DIR*>(uintptr_t(1));
}
static struct dirent *observed_readdir(DIR *handle)
{
    require(handle==reinterpret_cast<DIR*>(uintptr_t(1)),"Finite DIR handle");
    static struct dirent entry;
    if(entry_index==(entries_declared?declared_entries.size():1)) {
        if(fail_after_entries) { fail_after_entries=false;fail_once=true; }
        return nullptr;
    }
    std::memset(&entry,0,sizeof(entry));entry.d_type=DT_REG;
    const std::string name=entries_declared?declared_entries[entry_index].name:requested_xml==prefix_xml?"Arcade-PrefixCore_20260101.rbf":selected_leaf;
    if(entries_declared)entry.d_type=declared_entries[entry_index].type;
    ++entry_index;
    require(name.size()<sizeof(entry.d_name),"Finite component fits dirent");
    std::strcpy(entry.d_name,name.c_str());events.emplace_back("entry:"+name+"|type:"+std::to_string(entry.d_type));return &entry;
}
static int observed_closedir(DIR *handle)
{
    require(handle==reinterpret_cast<DIR*>(uintptr_t(1)),"Finite close handle");++directory_closes;events.emplace_back("directory-close");errno=EIO;return 0;
}
extern "C" FILE *mra_path_fopen(const char *name,const char *mode)
{
    require(!std::strcmp(mode,"rt"),"Actual SAX read mode");
    requested_xml=name;events.emplace_back("sax-open:"+requested_xml);
    require(requested_xml==wanted_xml||requested_xml==prefix_xml,"Only declared disposable XML endpoints");
    if(change_storage_at_open) { device=1;usbnum=5; }
    if(xml_missing) { errno=ENOMEM;return nullptr; }
    const std::string body=xml_body.empty()?"<misterromdescription><rbf>"+(requested_xml==prefix_xml?std::string("PrefixCore"):rbf_fragment)+"</rbf></misterromdescription>":xml_body;
    FILE *stream=std::tmpfile();
    require(stream&&std::fwrite(body.data(),1,body.size(),stream)==body.size(),"Disposable XML stream ready");
    std::rewind(stream);++streams;return stream;
}
static int callback(XMLEvent event,const XMLNode *node,SXML_CHAR *text,int n,SAX_Data *sd)
{
    if(event==XML_EVENT_ERROR)++parser_errors;
    const int result=forwarded(event,node,text,n,sd);
    events.emplace_back("callback:"+std::to_string(event)+":"+(node?std::to_string(node->tag_type):"null")+":"+(node?std::string(node->tag):"")+":"+(text?std::string(text):"")+":"+std::to_string(result));
    return result;
}
static int observed_parse(const char *name,const SAX_Callbacks *callbacks,void *user)
{
    ++parser_calls;forwarded=callbacks->all_event;SAX_Callbacks seen=*callbacks;seen.all_event=callback;
    const int result=XMLDoc_parse_file_SAX(name,&seen,user);parser_observed=true;parser_return=result;
    if(fail_after_parser) { fail_after_parser=false;fail_once=true; }
    return result;
}
#include "mra_path_production.inc"

static std::string directory(size_t length,const std::string &prefix="")
{
    std::string out=prefix;
    while(length-out.size()>200)out+=std::string(199,'d')+'/';
    out+=std::string(length-out.size(),'e');
    check(out.size()==length,"Complete generated directory length");
    size_t start=0;
    for(size_t i=0;i<=out.size();++i)if(i==out.size()||out[i]=='/') { check(i-start<=255,"All generated directory components fit");start=i+1; }
    return out;
}
static std::string relative_xml(size_t length)
{
    const std::string dir=directory(900,"_Arcade/");
    const std::string leaf=std::string(length-905,'n')+".mra";
    check(leaf.size()<=255,"XML leaf fits ordinary filesystem component");
    const std::string out=dir+'/'+leaf;
    check(out.size()==length&&out.size()<1024,"Menu-relative and command-relative bytes fit producer capacities");return out;
}
static void load(const std::string &input)
{
    wrapper_return=xml_load(input.c_str());wrapper_completed=true;
    check(wrapper_return==0&&hidden==1,"Actual public wrapper return and MenuHide");
}
static void admitted()
{
    check(parser_calls==1&&parser_observed&&parser_return==1&&streams==1,"Actual parser call and return complete");
    check(directory_calls==1&&directory_closes==1&&core_calls==1,"Actual matcher, closedir and core handoff calls observed");
    check(rbf_observed&&rbf_nonnull&&messages.empty(),"Complete RBF pointer return and no admission message");
    check(core_return_observed&&core_return==-1&&core_open_calls==1&&osd_disable_calls==1&&core_messages==std::vector<std::string>{actual_core+"\nNot Found|5000"},"Actual core open failure retains its existing timed message and return without restart");
    std::vector<std::string> order;
    bool selection=false;
    for(const std::string &event:events) {
        if(event=="menu-hide")selection=true;
        if(!selection)continue;
        if(event=="menu-hide"||event=="directory-close"||event=="osd-disable")order.push_back(event);
        else for(const char *prefix:{"root:","find-games:","sax-open:","directory:","core-handoff:","core-open:","core-info:"})
            if(event.compare(0,std::strlen(prefix),prefix)==0) { order.emplace_back(prefix);break; }
    }
    std::vector<std::string> expected={"menu-hide","root:"};if(games)expected.emplace_back("find-games:");
    expected.insert(expected.end(),{"sax-open:","directory:","directory-close","core-handoff:","osd-disable","core-open:","core-info:"});
    check(order==expected,"Admitted selection captures root before parser/query and closes DIR before actual core open");
}
static void rejected(const char *message,bool parsed)
{
    check(parser_calls==(parsed?1U:0U)&&parser_observed==parsed&&streams==(parsed?1U:0U),"Rejection preserves actual parser boundary");
    if(parsed)check(parser_return==1,"Actual accepted XML parser return before later path rejection");
    check(!core_calls,"Rejected path never reaches core handoff");
    check(messages==std::vector<std::string>{message},"Explicit path-admission message observed");
    if(parsed)check(rbf_observed&&!rbf_nonnull,"Actual rejected RBF pointer result is NULL");
    else check(!games&&!directory_calls&&!directory_closes&&!rbf_observed,"Known entry failure precedes root search, RBF parser and directory helpers");
}
static void private_cases(const std::string &name)
{
#if HAVE_MRA_PATHS
    struct Guard { unsigned char before[8];char value[1024];unsigned char after[8]; } guard;
    std::memset(&guard,0xa5,sizeof(guard));std::strcpy(guard.value,"PriorValue");
    guard_before_hex=hex_bytes(&guard,sizeof(guard));
    const Guard original=guard;arcade_path_error error=ARCADE_PATH_OK;
    if(name=="builder-copy") {
        check(arcade_path_copy(guard.value,guard.value,sizeof(guard.value),&error)&&!std::memcmp(&guard,&original,sizeof(guard)),"Exact-copy alias is defined and preserves all bytes");
        check(arcade_path_copy(guard.value+1,guard.value,sizeof(guard.value),&error)&&std::string(guard.value)=="riorValue","Substring copy alias preserves literal value");
        guard=original;
        check(!arcade_path_copy(std::string(1024,'n').c_str(),guard.value,sizeof(guard.value),&error)&&error==ARCADE_PATH_TOO_LONG&&!std::memcmp(&guard,&original,sizeof(guard)),"Overcapacity copy preserves all destination/canary bytes");
        check(!arcade_path_copy(nullptr,guard.value,sizeof(guard.value),&error)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Null copy input rejects unchanged");
        check(!arcade_path_copy("x",nullptr,sizeof(guard.value),&error)&&error==ARCADE_PATH_INVALID,"Null copy output rejects explicitly");
        check(!arcade_path_copy("x",guard.value,0,&error)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Zero copy capacity rejects unchanged");
        check(!arcade_path_copy_root("relative",guard.value,sizeof(guard.value),&error)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Relative captured root rejects unchanged");
        check(!arcade_path_copy_root("",guard.value,sizeof(guard.value),&error)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Empty captured root rejects unchanged");
        check(!arcade_path_copy_root(nullptr,guard.value,sizeof(guard.value),&error)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Null captured root rejects unchanged");
        check(arcade_path_copy(std::string(1023,'f').c_str(),guard.value,sizeof(guard.value),&error)&&std::strlen(guard.value)==1023&&guard.value[1023]==0,"Exact1023 copy is complete and terminated");
    } else if(name=="builder-join"||name=="builder-parent") {
        const bool parent=name=="builder-parent";
        std::strcpy(guard.value,"/Root/");std::strcpy(guard.value+64,"sub/../Leaf");
        const std::string expected=parent?"/Root//sub/..":"/Root//sub/../Leaf";
        check(arcade_path_join(guard.value,guard.value+64,guard.value,sizeof(guard.value),&error,parent)&&std::string(guard.value)==expected,"Root/name/output aliases preserve literal slash and dot spelling");
        guard=original;
        check(!arcade_path_join("/Root",std::string(1020,'n').c_str(),guard.value,sizeof(guard.value),&error,parent)&&error==ARCADE_PATH_TOO_LONG&&!std::memcmp(&guard,&original,sizeof(guard)),"Whole join capacity is checked before parent cut/publication");
        check(!arcade_path_join(nullptr,"x",guard.value,sizeof(guard.value),&error,parent)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Invalid literal join preserves all output");
        check(!arcade_path_join("/Root",nullptr,guard.value,sizeof(guard.value),&error,parent)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Null literal name preserves all output");
        check(!arcade_path_join("/Root","x",nullptr,sizeof(guard.value),&error,parent)&&error==ARCADE_PATH_INVALID,"Null literal output rejects explicitly");
        check(!arcade_path_join("/Root","x",guard.value,0,&error,parent)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Zero literal capacity preserves all output");
        check(!arcade_path_join("",std::string(1023,'x').c_str(),guard.value,sizeof(guard.value),&error,parent)&&error==ARCADE_PATH_TOO_LONG&&!std::memcmp(&guard,&original,sizeof(guard)),"Exact1024 literal join rejects before optional parent cut");
        std::strcpy(guard.value,"x/Root/");std::strcpy(guard.value+64,"sub/../Leaf");
        check(arcade_path_join(guard.value+1,guard.value+64,guard.value,sizeof(guard.value),&error,parent)&&std::string(guard.value)==expected,"Substring root and name aliases are owned before overlapping output");
        const std::string leaf=std::string(1022,'x');
        check(arcade_path_join("",leaf.c_str(),guard.value,sizeof(guard.value),&error,parent)&&std::string(guard.value)==(parent?"":"/"+leaf),"Complete1023 literal join accepted before optional parent extraction");
    } else if(name=="builder-resolve") {
        std::strcpy(guard.value,"/Root/");std::strcpy(guard.value+64,"sub/../Leaf");
        check(arcade_path_resolve(guard.value,guard.value+64,guard.value,sizeof(guard.value),&error)&&std::string(guard.value)=="/Root//sub/../Leaf","Actual resolver owns aliased relative inputs before publication");
        guard=original;
        check(!arcade_path_resolve("/Root","",guard.value,sizeof(guard.value),&error)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Empty selected XML rejects unchanged");
        check(!arcade_path_resolve("relative","name",guard.value,sizeof(guard.value),&error)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Invalid selected root rejects unchanged");
        check(!arcade_path_resolve("/Root",nullptr,guard.value,sizeof(guard.value),&error)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Null selected input rejects unchanged");
        check(!arcade_path_resolve("/Root","name",nullptr,sizeof(guard.value),&error)&&error==ARCADE_PATH_INVALID,"Null selected output rejects explicitly");
        check(!arcade_path_resolve("/Root","name",guard.value,0,&error)&&error==ARCADE_PATH_INVALID&&!std::memcmp(&guard,&original,sizeof(guard)),"Zero selected capacity rejects unchanged");
        check(!arcade_path_resolve("/Root",std::string(1018,'n').c_str(),guard.value,sizeof(guard.value),&error)&&error==ARCADE_PATH_TOO_LONG&&!std::memcmp(&guard,&original,sizeof(guard)),"Exact1024 relative selected join rejects unchanged");
        const std::string absolute="/"+std::string(1022,'a');
        check(arcade_path_resolve("/Root",absolute.c_str(),guard.value,sizeof(guard.value),&error)&&std::string(guard.value)==absolute,"Exact1023 absolute spelling is unchanged");
    } else if(name=="builder-oom") {
        const std::string long_root="/"+std::string(400,'r'),long_name=std::string(400,'n');
        fail_once=true;const int joined=arcade_path_join(long_root.c_str(),"Leaf",guard.value,sizeof(guard.value),&error);
        check(!fail_once&&!joined&&error==ARCADE_PATH_UNAVAILABLE&&errno==ENOMEM&&!std::memcmp(&guard,&original,sizeof(guard)),"Real long literal ownership allocation failure leaves entire output unchanged");
        fail_once=true;const int resolved=arcade_path_resolve("/Root",long_name.c_str(),guard.value,sizeof(guard.value),&error);
        check(!fail_once&&!resolved&&error==ARCADE_PATH_UNAVAILABLE&&errno==ENOMEM&&!std::memcmp(&guard,&original,sizeof(guard)),"Actual selected resolver allocation failure leaves entire output unchanged");
    } else if(name=="builder-ui") {
        arcade_path_info(ARCADE_PATH_INVALID,false);arcade_path_info(ARCADE_PATH_TOO_LONG,false);arcade_path_info(ARCADE_PATH_UNAVAILABLE,false);
        arcade_path_info(ARCADE_PATH_INVALID,true);arcade_path_info(ARCADE_PATH_TOO_LONG,true);arcade_path_info(ARCADE_PATH_UNAVAILABLE,true);
        check(messages==std::vector<std::string>{"Invalid XML path!","XML path too long!","XML path unavailable!","Invalid RBF path!","RBF path too long!","RBF path unavailable!"},"Actual private UI helper maps all explicit XML/RBF error categories");
    } else if(name=="builder-root-alias") {
        std::strcpy(arcade_root,"/media/fat/_First/Game.mra");std::strcpy(mame_root,"/media/fat");
        check(set_arcade_root(arcade_root,mame_root,true,&error)&&std::string(arcade_root)=="/media/fat/_First"&&std::string(mame_root)=="/media/fat/_First","Root bundle captures full input and fallback aliases before publication");
        check(set_arcade_root(arcade_root+11,mame_root+6,true,&error)&&std::string(arcade_root)=="/fat/_First"&&std::string(mame_root)=="/fat/_First","Root bundle captures substring aliases before either shared root changes");
    } else if(name=="root-found"||name=="root-empty") {
        games_found=true;games_reply=name=="root-found"?"../network/games/mame":"mame";
        check(set_arcade_root("/media/fat/_Arcade/Game.mra","/media/fat",true,&error)&&error==ARCADE_PATH_OK,"Valid root bundle is prepared");
        check(std::string(arcade_root)=="/media/fat/_Arcade"&&std::string(mame_root)==(name=="root-found"?"../network/games":""),"Found relative and empty MAME parent semantics preserved");
        char roots_before[2048];std::memcpy(roots_before,arcade_root,1024);std::memcpy(roots_before+1024,mame_root,1024);
        games_unterminated=true;
        check(!set_arcade_root("/Other/_Arcade/New.mra","/media/fat",true,&error)&&error==ARCADE_PATH_TOO_LONG&&!std::memcmp(roots_before,arcade_root,1024)&&!std::memcmp(roots_before+1024,mame_root,1024),"Invalid returned parent cannot partially publish either root");
    } else if(name=="root-cores-bounds") {
        const std::string base=directory(1014,"/")+"/_X";
        check(base.size()==1017&&set_arcade_root((base+"/a.mra").c_str(),"/media/fat",true,&error),"Root plus cores exact1023 is admitted");
        const char *result=get_arcade_root(1,&error);
        check(result&&std::string(result)==base+"/cores"&&error==ARCADE_PATH_OK,"Exact complete cores getter output");
        const std::string before_arcade=arcade_root,before_mame=mame_root;const unsigned before_games=games;
        const std::string over=directory(1015,"/")+"/_X";
        check(over.size()==1018&&!set_arcade_root((over+"/.mra").c_str(),"/media/fat",true,&error)&&error==ARCADE_PATH_TOO_LONG&&games==before_games&&std::string(arcade_root)==before_arcade&&std::string(mame_root)==before_mame,"Known cores join overflow rejects before query and root publication");
    } else if(name=="getter-preservation") {
        std::strcpy(mame_root,"BeforeMame");const char *borrowed=get_arcade_root(0,&error);char previous[1024];std::memcpy(previous,borrowed,1024);
        const std::string over=directory(1015,"/")+"/_X";std::memcpy(arcade_root,over.c_str(),over.size()+1);
        check(!get_arcade_root(1,&error)&&error==ARCADE_PATH_TOO_LONG&&!std::memcmp(previous,borrowed,1024),"Failed root getter preserves every previous buffer byte");
        const char *old=get_rbf_path("Cores/TestCore","/media/fat",&error);const std::string input=old;
        const char *aliased=get_rbf_path(old,"/media/fat",&error);
        check(aliased&&std::string(aliased)==("/media/fat/"+input).substr(0,("/media/fat/"+input).find_last_of('/')),"RBF parent helper captures its own borrowed output alias");
    } else require(false,"Unknown private path control");
    check(std::all_of(guard.before,guard.before+8,[](unsigned char b){return b==0xa5;})&&std::all_of(guard.after,guard.after+8,[](unsigned char b){return b==0xa5;}),"Private builder outer canaries intact");
    guard_after_hex=hex_bytes(&guard,sizeof(guard));
    check(!parser_calls&&!directory_calls&&!core_calls,"Private path controls make no parser/DIR/core calls");
#else
    (void)name;excluded=true;
#endif
}
static void clear_observations()
{
    hidden=games=parser_calls=streams=directory_calls=core_calls=directory_closes=root_calls=0;
    wrapper_completed=parser_observed=path_observed=path_nonnull=rbf_observed=rbf_nonnull=false;
    wrapper_return=parser_return=-999;messages.clear();events.clear();
    requested_xml.clear();actual_directory.clear();actual_core.clear();handed_xml.clear();path_input.clear();path_output.clear();
    core_open_calls=osd_disable_calls=0;core_return=-999;core_return_observed=false;core_messages.clear();
    parser_errors=0;
}
static bool extended_case(const std::string &name)
{
    if(name.compare(0,4,"new-")==0||name=="entry-resolve-oom"||name=="rbf-parent-oom"||name=="rbf-final-oom"||name=="rbf-cores-oom") {
#if HAVE_MRA_PATHS
        if(name=="new-recovery") {
            wanted_xml="/media/fat/_Arcade/First.mra";wanted_directory="/media/fat/_Arcade/cores";
            selected_leaf="Arcade-TestCore_20260101.rbf";wanted_core=wanted_directory+'/'+selected_leaf;
            load(wanted_xml);admitted();
            char before_xml[1024],before_core[1024],before_arcade[1024],before_mame[1024];
            std::memcpy(before_xml,last_handed_xml,1024);std::memcpy(before_core,last_handed_core,1024);
            std::memcpy(before_arcade,arcade_root,1024);std::memcpy(before_mame,mame_root,1024);
            clear_observations();const std::string too_long="/"+std::string(1023,'x');
            load(too_long);rejected("XML path too long!",false);
            check(!std::memcmp(before_xml,last_handed_xml,1024)&&!std::memcmp(before_core,last_handed_core,1024)&&!std::memcmp(before_arcade,arcade_root,1024)&&!std::memcmp(before_mame,mame_root,1024),"Entry failure preserves every prior XML/core/root output byte");
            clear_observations();wanted_xml="/media/fat/_Arcade/Recovered.mra";load("_Arcade/Recovered.mra");admitted();
            check(handed_xml==wanted_xml&&actual_core==wanted_core&&root_calls==1,"Valid selection recovers with one captured root after rejected entry");
            return true;
        }
        const char *input="_Arcade/Example.mra";std::string owned;
        const char *message="Invalid XML path!";bool parsed=false;
        if(name=="new-null")input=nullptr;
        else if(name=="new-empty")input="";
        else if(name=="new-invalid-root") { replace_root=true;root_reply="relative/root"; }
        else if(name=="new-null-root")null_root=true;
        else if(name=="new-overlong-root") { replace_root=true;root_reply="/"+std::string(1023,'r');message="XML path too long!"; }
        else if(name=="new-cores-oom") {
            owned=directory(100,"/")+"/_Arcade/Example.mra";input=owned.c_str();wanted_xml=owned;
            fail_size=owned.substr(0,owned.find_last_of('/')).size()+1;fail_once=true;message="XML path unavailable!";
        }
        else if(name=="entry-resolve-oom") { owned=directory(400)+"/Example.mra";input=owned.c_str();fail_after_root=true;message="XML path unavailable!"; }
        else if(name=="rbf-parent-oom"||name=="rbf-final-oom") {
            wanted_xml="/media/fat/launch.xml";input=wanted_xml.c_str();rbf_fragment=directory(100)+"/TestCore";
            wanted_directory="/media/fat/"+rbf_fragment.substr(0,rbf_fragment.find_last_of('/'));
            wanted_core=wanted_directory+'/'+selected_leaf;parsed=true;message="RBF path unavailable!";
            fail_after_parser=name=="rbf-parent-oom";fail_after_entries=name=="rbf-final-oom";
        } else if(name=="rbf-cores-oom") {
            wanted_xml=directory(100,"/")+"/_Arcade/Example.mra";input=wanted_xml.c_str();
            wanted_directory=wanted_xml.substr(0,wanted_xml.find_last_of('/'))+"/cores";
            selected_leaf="Arcade-TestCore_20260101.rbf";wanted_core=wanted_directory+'/'+selected_leaf;
            parsed=true;message="RBF path unavailable!";fail_after_parser=true;
        } else require(false,"Known new entry/allocation control");
        wrapper_return=xml_load(input);wrapper_completed=true;
        check(wrapper_return==0&&hidden==1,"New public rejection returns zero after MenuHide");rejected(message,parsed);
        if(parsed)check(directory_calls==(name=="rbf-final-oom"?1U:0U)&&directory_closes==directory_calls,"Allocation path failure closes every opened directory");
        else check(std::string(arcade_root)=="PriorArcade"&&std::string(mame_root)=="PriorMame","Invalid/unavailable entry leaves prior roots unchanged");
        if(name=="new-null"||name=="new-empty")check(!root_calls,"Invalid input rejects before a root provider is called");
        if(name=="rbf-cores-oom")check(games==1&&std::string(arcade_root)==wanted_directory.substr(0,wanted_directory.size()-6)&&std::string(mame_root)==arcade_root,"Valid root bundle remains published before later cores getter allocation failure");
        if(name.find("oom")!=std::string::npos)check(allocation_faults==1&&!fail_once&&errno==ENOMEM,"One real ownership allocation failed with ENOMEM retained through cleanup");
        return true;
#else
        excluded=true;return true;
#endif
    }
    if(name=="nonarcade-join1023"||name=="nonarcade-join1024") {
        wanted_xml="/media/fat/launch.xml";const bool over=name=="nonarcade-join1024";
        const std::string parent=directory(over?932:931),leaf(80,'C');
        rbf_fragment=parent+'/'+leaf;selected_leaf=leaf+".rbf";
        wanted_directory="/media/fat/"+parent;wanted_core=wanted_directory+'/'+selected_leaf;
        check(("/media/fat/"+rbf_fragment).size()==(over?1024U:1023U),"Complete nonarcade root/field seam declared exactly");
        load(wanted_xml);rejected("RBF path too long!",true);
        check(path_observed&&path_input==rbf_fragment&&path_nonnull==!over,"Full field join admission happens before parent extraction");
        check(directory_calls==(over?0U:1U)&&directory_closes==directory_calls,"Exact1023 field reaches directory;1024 field makes no query");
        if(!over)check(path_output==wanted_directory&&actual_directory==wanted_directory,"Exact fitting parent remains complete before later filename rejection");
        return true;
    }
    if(name=="selected-cores1023"||name=="selected-cores1024") {
        const bool over=name=="selected-cores1024";
        const std::string base=directory(over?1015:1014,"/")+"/_X";
        wanted_xml=base+(over?"/.mra":"/a.mra");wanted_directory=base+"/cores";
        selected_leaf="Arcade-TestCore_20260101.rbf";wanted_core=wanted_directory+'/'+selected_leaf;
        check(wanted_xml.size()==1023&&wanted_directory.size()==(over?1024U:1023U),"Selected complete XML fits while known cores join straddles capacity");
        load(wanted_xml);rejected(over?"XML path too long!":"RBF path too long!",!over);
        if(over)check(std::string(arcade_root)=="PriorArcade"&&std::string(mame_root)=="PriorMame","Known cores failure preserves prior root bundle");
        else check(std::string(arcade_root)==base&&std::string(mame_root)==base&&games==1&&actual_directory==wanted_directory&&directory_calls==1&&directory_closes==1,"Valid roots remain published when final selected core later rejects");
        return true;
    }
    if(name=="final1023"||name=="final1024"||name=="final1024-reversed") {
        const bool fit=name=="final1023";wanted_xml="/media/fat/launch.xml";rbf_fragment="Core";selected_leaf="Core_20260101.rbf";
        wanted_directory="/media/fat/"+directory((fit?1023:1024)-12-selected_leaf.size());
        rbf_fragment=wanted_directory.substr(11)+"/Core";wanted_core=wanted_directory+'/'+selected_leaf;
        entries_declared=true;declared_entries={{"Core.rbf"},{selected_leaf}};
        if(name=="final1024-reversed")std::reverse(declared_entries.begin(),declared_entries.end());
        check(wanted_core.size()==(fit?1023U:1024U)&&("/media/fat/"+rbf_fragment).size()<1024,"Exact final seam has a fitting complete field and directory");
        check((wanted_directory+"/Core.rbf").size()<1024,"Older shorter eligible core would fit");
        load(wanted_xml);
        if(fit) { admitted();check(actual_core==wanted_core,"Exact1023 selected core reaches handoff complete"); }
        else { rejected("RBF path too long!",true);check(errno==ENAMETOOLONG,"Final path error survives close errno change"); }
        check(path_nonnull&&path_output==wanted_directory&&actual_directory==wanted_directory&&directory_calls==1&&directory_closes==1,"Exact directory queried once and closed after scanning both eligible entries");
        check(entry_index==2,"Both eligible entries observed without shorter fallback");return true;
    }
    if(name=="matcher-plain"||name=="matcher-plain-reversed"||name=="matcher-arcade"||name=="matcher-arcade-reversed") {
        const bool arcade=name.find("arcade")!=std::string::npos;
        wanted_xml=arcade?"/media/fat/_Arcade/Example.mra":"/media/fat/launch.xml";
        wanted_directory=arcade?"/media/fat/_Arcade/cores":"/media/fat";rbf_fragment="TestCore";entries_declared=true;
        selected_leaf=arcade?"aRcAdE-tEsTcOrE_20260101.RbF":"testcore_20250101.RBF";
        declared_entries={{"TestCore.rbf"},{"TestCore_20290101.rbf",DT_DIR},{"TestCorePlus_999.rbf"},{"TestCore_999.txt"},{"TestCore_20240101.rbf"},{selected_leaf,static_cast<unsigned char>(arcade?DT_LNK:DT_UNKNOWN)},{"Arcade-TestCore_20990101.rbf"},{"Arcade-TestCorePlus_999.rbf"}};
        if(name.find("reversed")!=std::string::npos)std::reverse(declared_entries.begin(),declared_entries.end());
        wanted_core=wanted_directory+'/'+selected_leaf;load(wanted_xml);admitted();
        check(actual_directory==wanted_directory&&actual_core==wanted_core&&entry_index==declared_entries.size(),"Extension, directory, case, prefix boundary and lexicographic selection retain declared behavior in either enumeration order");
        return true;
    }
    if(name=="no-match"||name=="no-rbf"||name=="invalid-scalar"||name=="sax-malformed"||name=="sax-error"||name=="sax-open-failure"||name=="directory-failure") {
        wanted_xml="/media/fat/launch.xml";wanted_directory="/media/fat";
        if(name=="no-match") { entries_declared=true;declared_entries={{"TestCorePlus_999.rbf"},{"Arcade-TestCore_999.rbf"},{"TestCore_999.rbf",DT_DIR},{"TestCore.txt"}}; }
        else if(name=="no-rbf")xml_body="<misterromdescription/>";
        else if(name=="invalid-scalar")xml_body="<misterromdescription><rbf>TestCore<x/></rbf></misterromdescription>";
        else if(name=="sax-malformed")xml_body="<misterromdescription><rbf>TestCore</misterromdescription>";
        else if(name=="sax-error")xml_body="<misterromdescription><rbf>TestCore</rbf><";
        else if(name=="sax-open-failure")xml_missing=true;
        else directory_missing=true;
        errno=ENAMETOOLONG;load(wanted_xml);
        check(parser_calls==1&&parser_observed&&rbf_observed&&!rbf_nonnull&&!core_calls,"Ordinary scalar/parser/directory rejection observes actual NULL and no core");
        check(messages==std::vector<std::string>{"No rbf found!"},"Ordinary failure UI remains independent of stale path errno");
        check(streams==(xml_missing?0U:1U),"Only actual disposable parser opens counted");
        if(xml_missing)check(parser_return==0,"Failed fopen produces actual parser zero");
        if(name=="sax-error")check(parser_return==0&&parser_errors==1,"Actual malformed tail produces a real SAX error and zero parser return");
        const bool query=name=="no-match"||name=="directory-failure";
        check(directory_calls==(query?1U:0U)&&directory_closes==(name=="no-match"?1U:0U),"Ordinary error keeps actual directory/close boundary");
        return true;
    }
    if(name=="no-marker"||name=="marker-without-slash"||name=="first-marker"||name=="mixed-case-mra") {
        const std::string relative=name=="no-marker"?"Arcade/Example.mra":name=="marker-without-slash"?"_Only.mra":name=="first-marker"?"_First/inner/_Second/Example.mra":"_Arcade/Example.MrA";
        const std::string root=name=="first-marker"?"/media/fat/_First":name=="mixed-case-mra"?"/media/fat/_Arcade":"/media/fat";
        wanted_xml="/media/fat/"+relative;wanted_directory=root+"/cores";selected_leaf="Arcade-TestCore_20260101.rbf";wanted_core=wanted_directory+'/'+selected_leaf;
        load(relative);admitted();check(std::string(arcade_root)==root&&std::string(mame_root)==root&&actual_directory==wanted_directory&&actual_core==wanted_core,"Terminal MRA classification and first-marker/fallback semantics preserved");
        return true;
    }
    if(name=="captured-root"||name=="borrowed-input") {
        wanted_xml="/media/fat/_Arcade/Example.mra";wanted_directory="/media/fat/_Arcade/cores";selected_leaf="Arcade-TestCore_20260101.rbf";wanted_core=wanted_directory+'/'+selected_leaf;
        if(name=="captured-root") {
            wanted_xml="/media/fat/launch.xml";wanted_directory="/media/fat/Cores";rbf_fragment="Cores/TestCore";selected_leaf="TestCore_20260101.rbf";wanted_core=wanted_directory+'/'+selected_leaf;
            change_storage_at_open=true;load(wanted_xml);
        } else {
            std::strcpy(full_path,"_Arcade/Example.mra");mutate_input_at_root=true;prefix_xml="/media/fat/Changed/Other.xml";
            wrapper_return=xml_load(full_path);wrapper_completed=true;
            check(wrapper_return==0&&hidden==1,"Borrowed caller buffer reaches actual wrapper directly");
            check(std::string(full_path)=="Changed/Other.xml","Declared root provider overwrote the borrowed caller input");
        }
        admitted();check(root_calls==1&&requested_xml==wanted_xml&&handed_xml==wanted_xml&&actual_core==wanted_core,"Selected XML owns input and captures one root before later provider changes");
        return true;
    }
    return false;
}
static void scenario(const std::string &name)
{
    std::strcpy(arcade_root,"PriorArcade");std::strcpy(mame_root,"PriorMame");
    root_before_hex=hex_bytes(arcade_root,1024)+hex_bytes(mame_root,1024);
    if(extended_case(name))return;
    if(name.compare(0,8,"builder-")==0||name=="root-found"||name=="root-empty"||name=="root-cores-bounds"||name=="getter-preservation") { private_cases(name);return; }
    if(name=="root-copy1024") {
        const std::string path="/media/fat/"+relative_xml(1013);wanted_xml=path;
#if HAVE_MRA_PATHS
        arcade_path_error error=ARCADE_PATH_OK;
        root_return=set_arcade_root(path.c_str(),nullptr,true,&error);root_return_observed=true;
        check(!root_return&&error==ARCADE_PATH_TOO_LONG&&errno==ENAMETOOLONG,"Actual root preparation rejects complete1024 bytes");
        check(!games&&!parser_calls&&!directory_calls&&!core_calls,"Known root input failure precedes endpoint queries");
        check(std::string(arcade_root)=="PriorArcade"&&std::string(mame_root)=="PriorMame","Root rejection preserves old root values");return;
#else
        set_arcade_root(path.c_str());throw std::runtime_error("Expected ended root copy");
#endif
    }
    if(name=="menu-absolute1024") {
        const std::string relative=relative_xml(1013);wanted_xml="/media/fat/"+relative;
        const char *produced=getFullPath(relative.c_str());
        check(std::strlen(produced)==1024&&std::string(produced)==wanted_xml,"Actual getFullPath fits its2100 buffer and delivers1024 bytes");
        wrapper_return=xml_load(produced);wrapper_completed=true;
        check(wrapper_return==0&&hidden==1,"Actual completed public rejection and MenuHide");
        rejected("XML path too long!",false);
        check(std::string(arcade_root)=="PriorArcade"&&std::string(mame_root)=="PriorMame","Entry rejection preserves prior roots");return;
    }
    if(name=="nonarcade-parent-clip"||name=="nonarcade-final-clip") {
        wanted_xml="/media/fat/launch.xml";
        const std::string parent=directory(name=="nonarcade-parent-clip"?1015:1000);
        rbf_fragment=parent+"/CoreXYZ";selected_leaf="CoreXYZ_20260101.rbf";
        check(rbf_fragment.size()<=1023,"Nonarcade scalar delivered bytes fit1023 policy");
        wanted_directory="/media/fat/"+parent;wanted_core=wanted_directory+'/'+selected_leaf;
        load(wanted_xml);
        rejected("RBF path too long!",true);
        check(path_observed&&path_input==rbf_fragment,"Actual full fitting scalar bytes reach path preparation");
        if(name=="nonarcade-parent-clip")check(!path_nonnull&&!directory_calls&&!directory_closes,"Whole join rejected before parent query");
        else check(path_nonnull&&path_output==wanted_directory&&actual_directory==wanted_directory&&directory_calls==1&&directory_closes==1,"Fitting parent queried and closed before overlong selected target rejection");
        target_differs=(directory_calls&&actual_directory!=wanted_directory)||(core_calls&&actual_core!=wanted_core);return;
    }
    if(name=="leading-slash") {
        wanted_xml="/media/fat/launch.xml";rbf_fragment="/Cores/TestCore";wanted_directory="/media/fat//Cores";wanted_core=wanted_directory+'/'+selected_leaf;
        load(wanted_xml);admitted();check(actual_directory==wanted_directory&&actual_core==wanted_core,"Inherited nonarcade root/slash/leading-slash semantics");return;
    }
    if(name=="literal-rbf") {
        wanted_xml="/media/fat/launch.xml";rbf_fragment="Cores/Core%\xc3\xa9";selected_leaf="Core%\xc3\xa9_20260101.rbf";
        wanted_directory="/media/fat/Cores";wanted_core=wanted_directory+'/'+selected_leaf;
        load(wanted_xml);admitted();check(path_input==rbf_fragment&&actual_core==wanted_core,"Literal percent and UTF8 RBF field/filename bytes are preserved through parser and core open");return;
    }
    std::string relative="_Arcade/Example.mra";
    if(name=="fit-relative1023"||name=="fit-absolute1023")relative=relative_xml(1012);
    if(name=="relative-joined1024")relative=relative_xml(1013);
    if(name=="literal-name")relative="_Arcade/Name%Literal\xc3\xa9.mra";
    if(name=="usb-relative") { device=1;usbnum=2; }
    const std::string root=getRootDir();wanted_xml=root+'/'+relative;wanted_directory=root+"/_Arcade/cores";selected_leaf="Arcade-TestCore_20260101.rbf";wanted_core=wanted_directory+'/'+selected_leaf;
    if(name=="relative-joined1024")prefix_xml=wanted_xml.substr(0,1023);
    if(name=="ordinary-absolute"||name=="fit-absolute1023") {
        overwrite_shared=true;const char *produced=getFullPath(relative.c_str());check(std::string(produced)==wanted_xml,"Actual producer bytes before helper mutation");
        wrapper_return=xml_load(produced);wrapper_completed=true;
        check(wrapper_return==0&&hidden==1,"Actual borrowed menu producer reaches wrapper directly");
        check(std::string(full_path)=="/unrelated/borrowed-buffer.mra","Declared query boundary overwrites borrowed provider only");
    }
    else load(relative);
    if(name=="relative-joined1024") {
        check(wanted_xml.size()==1024,"Relative join counterexample requires1024 complete bytes");
        rejected("XML path too long!",false);
        check(std::string(arcade_root)=="PriorArcade"&&std::string(mame_root)=="PriorMame","Known join failure preserves prior roots");
        target_differs=(parser_calls&&requested_xml!=wanted_xml)||(core_calls&&actual_core!=wanted_core);return;
    }
    admitted();check(requested_xml==wanted_xml&&handed_xml==wanted_xml&&actual_directory==wanted_directory&&actual_core==wanted_core,"Complete fitting inputs preserve actual target and matcher");
    check(std::string(arcade_root)==root+"/_Arcade"&&std::string(mame_root)==root+"/_Arcade"&&games==1,"Actual Arcade root parsing and MAME fallback");
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;const std::string name=argv[1];
    clear_observations();
    try { scenario(name); }
    catch(const Boundary &error) {
        copy_ended=excluded=true;boundary=error.what();
        try {
            check(name=="menu-absolute1024"||name=="root-copy1024","Only specified pre-copy boundary cases");
            check(!wrapper_completed&&!parser_calls&&!parser_observed&&!directory_calls&&!core_calls&&!games,"Ended copy has no invented later wrapper/parser/query/core outcomes");
            check(std::string(arcade_root)=="PriorArcade"&&std::string(mame_root)=="PriorMame","Ended first copy leaves prior root values intact");
            check(hidden==(name=="menu-absolute1024"?1U:0U),"Only actual prior MenuHide effect observed");
        } catch(const std::exception &e) { std::fprintf(stderr,"Fixture error: %s\n",e.what());return 2; }
    }
    catch(const std::exception &error) { std::fprintf(stderr,"Fixture error: %s\n",error.what());return 2; }
    target_differs=target_differs||(parser_calls&&!wanted_xml.empty()&&requested_xml!=wanted_xml)||(directory_calls&&!wanted_directory.empty()&&actual_directory!=wanted_directory)||(core_calls&&!wanted_core.empty()&&actual_core!=wanted_core);
    std::printf("JSON {\"case\":%s,\"observation_case_completed\":true,\"checks\":%u,\"ended_before_copy\":%s,\"boundary\":%s,\"wrapper_return_observed\":%s,\"wrapper_return\":%d,\"parser_return_observed\":%s,\"parser_return\":%d,\"parser_calls\":%u,\"streams\":%u,\"directory_calls\":%u,\"core_calls\":%u,\"menu_hide_calls\":%u,\"target_differs\":%s,\"wanted_xml\":%s,\"wanted_xml_bytes\":%zu,\"requested_xml\":%s,\"wanted_directory\":%s,\"actual_directory\":%s,\"wanted_core\":%s,\"actual_core\":%s,\"handed_xml\":%s,\"path_input\":%s,\"path_output\":%s,\"events\":%s,",
        quote(name).c_str(),checks,boolean(copy_ended),quote(boundary).c_str(),boolean(wrapper_completed),wrapper_return,boolean(parser_observed),parser_return,parser_calls,streams,directory_calls,core_calls,hidden,boolean(target_differs),quote(wanted_xml).c_str(),wanted_xml.size(),quote(requested_xml).c_str(),quote(wanted_directory).c_str(),quote(actual_directory).c_str(),quote(wanted_core).c_str(),quote(actual_core).c_str(),quote(handed_xml).c_str(),quote(path_input).c_str(),quote(path_output).c_str(),array(events).c_str());
    std::printf("\"failures\":%u,\"failure_labels\":%s,\"messages\":%s,\"directory_closes\":%u,\"path_return_observed\":%s,\"path_result_nonnull\":%s,\"rbf_return_observed\":%s,\"rbf_result_nonnull\":%s,\"root_return_observed\":%s,\"root_return\":%d,\"excluded\":%s,",failures,array(failure_labels).c_str(),array(messages).c_str(),directory_closes,boolean(path_observed),boolean(path_nonnull),boolean(rbf_observed),boolean(rbf_nonnull),boolean(root_return_observed),root_return,boolean(excluded));
    std::printf("\"root_calls\":%u,\"entry_count\":%zu,\"allocation_faults\":%u,\"path_failures\":%s,\"guard_before_hex\":%s,\"guard_after_hex\":%s,\"roots_before_hex\":%s,\"roots_after_hex\":%s,\"arcade_root\":%s,\"mame_root\":%s,",root_calls,entry_index,allocation_faults,array(path_failures).c_str(),quote(guard_before_hex).c_str(),quote(guard_after_hex).c_str(),quote(root_before_hex).c_str(),quote(hex_bytes(arcade_root,1024)+hex_bytes(mame_root,1024)).c_str(),quote(arcade_root).c_str(),quote(mame_root).c_str());
    std::printf("\"core_open_calls\":%u,\"osd_disable_calls\":%u,\"core_return_observed\":%s,\"core_return\":%d,\"core_messages\":%s,\"parser_errors\":%u}\n",core_open_calls,osd_disable_calls,boolean(core_return_observed),core_return,array(core_messages).c_str(),parser_errors);
    return failures?1:0;
}
