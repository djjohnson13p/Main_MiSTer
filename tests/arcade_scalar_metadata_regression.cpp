#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <dirent.h>
#include <strings.h>
#include "sxmlc.h"
#include "arcade_scalar_types.inc"

struct Boundary : std::runtime_error { using std::runtime_error::runtime_error; };
struct Parse {
    std::string api, document, result, context, rbf_output;
    bool open_failure=false, helper_completed=false, parser_observed=false, result_nonnull=false, stops_are_real=true;
    int parser_return=-999;
    unsigned streams=0, stops=0, roots=0, arcade_roots=0, errors=0, path_calls=0;
    std::vector<std::string> events, directories, overrides;
};
static std::vector<Parse> parses;
static Parse *active;
static bool fail_open;
static size_t entry_index;
static unsigned checks, failures;
static std::vector<std::string> failure_labels;
using Callback = decltype(SAX_Callbacks::all_event);
static Callback forwarded;

static std::string quote(const std::string &s)
{
    std::string out="\""; const char hex[]="0123456789abcdef";
    for(unsigned char c:s) {
        if(c=='"'||c=='\\') { out+='\\'; out+=c; }
        else if(c=='\n') out+="\\n";
        else if(c=='\r') out+="\\r";
        else if(c=='\t') out+="\\t";
        else if(c<32||c>=127) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=c;
    }
    return out+'"';
}
static std::string hexbytes(const std::string &s)
{
    const char hex[]="0123456789abcdef"; std::string out;
    for(unsigned char c:s) { out+=hex[c>>4]; out+=hex[c&15]; }
    return out;
}
static std::string bytes(const std::string &s)
{
    return "{\"value\":"+quote(s)+",\"length\":"+std::to_string(s.size())+",\"hex\":"+quote(hexbytes(s))+"}";
}
static std::string array(const std::vector<std::string> &values,bool raw=false)
{
    std::string out="[";
    for(size_t i=0;i<values.size();++i) { if(i)out+=','; out+=raw?values[i]:quote(values[i]); }
    return out+"]";
}
static const char *boolean(bool v) { return v?"true":"false"; }
static void check(bool ok,const char *label)
{
    ++checks;
    if(!ok) { ++failures; failure_labels.emplace_back(label); std::printf("FAIL: %s\n",label); }
}
static void reject(const char *label) { throw std::runtime_error(label); }
static const char *getRootDir() { ++active->roots; return "/media/fat"; }
static const char *get_arcade_root(int kind)
{
    if(kind!=1)reject("Unselected arcade root kind");
    ++active->arcade_roots; return "/media/fat/_Arcade/cores";
}
static DIR *fixture_opendir(const char *path)
{
    if(std::strcmp(path,"/media/fat/_Arcade/cores")&&std::strcmp(path,"/media/fat/Cores"))reject("Unselected directory query");
    active->directories.emplace_back(std::string("open:")+path); entry_index=0;
    return reinterpret_cast<DIR*>(uintptr_t(1));
}
static struct dirent *fixture_readdir(DIR *handle)
{
    if(handle!=reinterpret_cast<DIR*>(uintptr_t(1)))reject("Unexpected DIR handle");
    static const char *names[]={"Arcade-TestCore_20260101.rbf","Arcade-TestCore_20260102.rbf",
        "Arcade-Test_20260101.rbf","Arcade-Core_20260101.rbf","TestCore_20260101.rbf",
        "TestCore_20260102.rbf","Arcade-TestCore_20260103.rbf","Other.rbf"};
    static struct dirent entry;
    if(entry_index==sizeof(names)/sizeof(names[0]))return nullptr;
    std::memset(&entry,0,sizeof(entry)); entry.d_type=entry_index==6?DT_DIR:DT_REG;
    std::strcpy(entry.d_name,names[entry_index++]);
    active->directories.emplace_back(std::string("entry:")+entry.d_name+":"+std::to_string(entry.d_type)); return &entry;
}
static int fixture_closedir(DIR *handle)
{
    if(handle!=reinterpret_cast<DIR*>(uintptr_t(1)))reject("Unexpected closedir handle");
    active->directories.emplace_back("close"); return 0;
}
extern "C" FILE *scalar_fixture_fopen(const char *name,const char *mode)
{
    if(std::strcmp(name,"/synthetic/fixture.mra")||std::strcmp(mode,"rt"))reject("Unexpected SAX file endpoint");
    if(fail_open) { errno=ENOENT; return nullptr; }
    FILE *stream=std::tmpfile();
    if(!stream||std::fwrite(active->document.data(),1,active->document.size(),stream)!=active->document.size())reject("Disposable seekable XML stream unavailable");
    std::rewind(stream); ++active->streams; return stream;
}
#if !HAVE_SCALAR_CONTEXTS
static char *guarded_strncpy(char *out,const char *text,size_t capacity)
{
    if(capacity!=1024)reject("Unrecognized legacy strncpy boundary");
    if(std::strlen(text)>=capacity)throw Boundary("legacy-rbf-strncpy-missing-nul-risk-before-copy");
    return std::strncpy(out,text,capacity);
}
#endif
static void observe_override(const char *name,int same)
{
    active->overrides.emplace_back("{\"input\":"+bytes(name)+",\"same_dir\":"+std::to_string(same)+",\"actual_override\":"+bytes(ovr_name)+",\"actual_same_dir\":"+std::to_string(ovr_samedir)+"}");
}
static int xml_scan_rbf(XMLEvent,const XMLNode*,SXML_CHAR*,int,SAX_Data*);
static int xml_read_pre_parse(XMLEvent,const XMLNode*,SXML_CHAR*,int,SAX_Data*);
static std::string context(SAX_Data *sd)
{
#if HAVE_SCALAR_CONTEXTS
    if(forwarded==xml_scan_rbf) {
        const arcade_rbf_scan &s=*static_cast<arcade_rbf_scan*>(sd->user);
        active->rbf_output=s.output;
        return "{\"kind\":"+std::to_string(s.owner.kind)+",\"depth\":"+std::to_string(s.owner.depth)+",\"owner_invalid\":"+boolean(s.owner.invalid)+
            ",\"depth_failed\":"+boolean(s.owner.depth_failed)+",\"invalid\":"+boolean(s.invalid)+",\"used\":"+std::to_string(s.used)+
            ",\"pending\":"+bytes(s.pending)+",\"output\":"+bytes(s.output)+"}";
    }
    const arcade_preparse_scan &s=*static_cast<arcade_preparse_scan*>(sd->user);
    return "{\"kind\":"+std::to_string(s.owner.kind)+",\"depth\":"+std::to_string(s.owner.depth)+",\"owner_invalid\":"+boolean(s.owner.invalid)+
        ",\"depth_failed\":"+boolean(s.owner.depth_failed)+",\"used\":"+std::to_string(s.used)+",\"pending_name\":"+bytes(s.setname)+
        ",\"same_dir_latch\":"+std::to_string(s.samedir)+",\"captured_same\":"+std::to_string(s.captured_same)+
        ",\"found_setname\":"+boolean(s.foundsetname)+",\"found_rotation\":"+boolean(s.foundrotation)+
        ",\"rotation_prefix\":"+bytes(s.rotation.prefix)+",\"rotation_suffix\":"+bytes(s.rotation.suffix)+
        ",\"prefix_used\":"+std::to_string(s.rotation.prefix_used)+",\"suffix_used\":"+std::to_string(s.rotation.suffix_used)+
        ",\"nonempty\":"+boolean(s.rotation.nonempty)+",\"seen_ccw\":"+boolean(s.rotation.seen_ccw)+"}";
#else
    if(forwarded==xml_scan_rbf) {
        const char *out=static_cast<const char*>(sd->user);
        if(!std::memchr(out,0,1024))reject("Legacy RBF unterminated output reached observer");
        active->rbf_output=out;
    }
    return "{\"private_context_available\":false}";
#endif
}
static int observe_callback(XMLEvent event,const XMLNode *node,SXML_CHAR *text,int n,SAX_Data *sd)
{
    const int result=forwarded(event,node,text,n,sd);
    if(!result) {
        ++active->stops;
        if(event!=XML_EVENT_END_NODE||!node||(node->tag_type!=TAG_END&&node->tag_type!=TAG_SELF))active->stops_are_real=false;
    }
    if(event==XML_EVENT_ERROR)++active->errors;
    active->context=context(sd);
    active->events.emplace_back("{\"event\":"+std::to_string(event)+",\"type\":"+(node?std::to_string(node->tag_type):"null")+
        ",\"tag\":"+(node?bytes(node->tag):"null")+",\"text\":"+(text?bytes(text):"null")+",\"callback_return\":"+std::to_string(result)+
        ",\"context\":"+active->context+",\"setname\":"+bytes(arcade_setname)+",\"override\":"+bytes(ovr_name)+
        ",\"same_dir\":"+std::to_string(ovr_samedir)+",\"vertical\":"+boolean(is_vertical)+",\"direction\":"+std::to_string(rotation_dir)+"}");
    return result;
}
static int observed_parse_file(const char *name,const SAX_Callbacks *callbacks,void *user)
{
    if(callbacks->all_event!=xml_scan_rbf&&callbacks->all_event!=xml_read_pre_parse)reject("Unexpected selected callback");
    forwarded=callbacks->all_event; SAX_Callbacks observed=*callbacks; observed.all_event=observe_callback;
    const int result=XMLDoc_parse_file_SAX(name,&observed,user);
    active->parser_observed=true; active->parser_return=result; return result;
}
#include "arcade_scalar_production.inc"

static std::string wrap(const std::string &body) { return "<misterromdescription>"+body+"</misterromdescription>"; }
static void begin(const std::string &api,const std::string &doc,bool no_open=false)
{
    parses.emplace_back(); active=&parses.back(); active->api=api; active->document=doc; active->open_failure=no_open; fail_open=no_open;
}
static void common(int parser_return=1)
{
    check(active->helper_completed&&active->parser_observed,"actual helper/parser calls completed");
    check(active->parser_return==parser_return,"actual parser return matches control");
    check(active->streams==(active->open_failure?0U:1U),"disposable stream count matches open outcome");
}
static void rbf(const std::string &doc,const std::string &name,bool admitted=true,bool arcade=true,bool no_open=false,int parse_return=1)
{
    begin("get_rbf",doc,no_open);
    const char *result=get_rbf("/synthetic/fixture.mra",arcade?1:0);
    active->helper_completed=true; active->result_nonnull=result!=nullptr; active->result=result?result:"";
    common(parse_return);
    check(active->directories.size()==(admitted?10U:0U),"RBF queries occur only after accepted complete nonempty scan");
    check(active->roots==(admitted&&!arcade?1U:0U)&&active->arcade_roots==(admitted&&arcade?1U:0U),"rejected RBF stops before root/path providers");
    check(active->path_calls==(admitted&&!arcade?1U:0U),"nonarcade RBF path helper admission is observed");
    if(admitted)check(active->rbf_output==name,"RBF delivered bytes completed before intact matcher");
    std::string expected;
    if(admitted&&(name=="TestCore"||name=="Test"||name=="Core"||name=="Cores/TestCore")) {
        const std::string leaf=name=="Cores/TestCore"?"TestCore":name;
        expected=arcade?"/media/fat/_Arcade/cores/"+(leaf=="TestCore"?"TestCore_20260102.rbf":"Arcade-"+leaf+"_20260101.rbf"):
                        "/media/fat/Cores/"+leaf+"_20260102.rbf";
    }
    check(active->result==expected&&active->result_nonnull==!expected.empty(),"actual RBF matcher result/null status");
    check(active->overrides.empty(),"RBF wrapper does not invoke name override");
}
static void pre(const std::string &doc,const std::string &name,int same,int dir,bool vertical,unsigned override_count=1,unsigned stops=0,bool no_open=false,int parse_return=1)
{
    begin("arcade_pre_parse",doc,no_open); arcade_pre_parse("/synthetic/fixture.mra"); active->helper_completed=true; common(parse_return);
    check(std::string(arcade_setname)==name,"completed scalar setname bytes");
    check(std::string(ovr_name)==name.substr(0,31)&&ovr_samedir==same,"actual override truncation and same-dir outputs");
    check(active->overrides.size()==override_count,"name override calls occur at valid nonempty real completion");
    check(arcade_get_direction()==dir&&arcade_is_vertical()==vertical,"actual direction and vertical accessor outputs");
    check(active->stops==stops,"actual callback early-stop count");
    check(!active->stops||active->stops_are_real,"callback stops occur only at real scalar completion boundaries");
    check(active->directories.empty()&&!active->roots&&!active->arcade_roots,"preparse executes no directory/root helpers");
}
static void name_only(const std::string &body,const std::string &name,int same=0,unsigned calls=1)
{
    pre(wrap(body),name,same,0,false,calls);
}
static void rotation(const std::string &content,int dir,bool vertical)
{
    pre(wrap("<rotation>"+content+"</rotation>"),"",0,dir,vertical,0);
}
static void private_bounds(bool &excluded)
{
#if HAVE_SCALAR_CONTEXTS
    (void)excluded;
    struct Guard { unsigned char before[8]; char data[kBigTextSize]; unsigned char after[8]; } g;
    std::memset(&g,0xa7,sizeof(g)); g.data[0]=0;
    size_t used=0;
    check(arcade_scalar_append(g.data,used,std::string(1023,'a').c_str(),true)&&used==1023&&g.data[1023]==0,"private append exact fit");
    const Guard saved=g;
    check(!arcade_scalar_append(g.data,used,"b",true)&&used==1023&&!std::memcmp(&g,&saved,sizeof(g)),"private append reject preserves complete bytes and canaries");
    check(arcade_scalar_append(g.data,used,"tail",false)&&used==1023&&!std::memcmp(&g,&saved,sizeof(g)),"private saturation preserves full fitted bytes/canaries");
    used=1024;
    check(!arcade_scalar_append(g.data,used,"",false)&&used==1024&&!std::memcmp(&g,&saved,sizeof(g)),"private corrupt capacity rejected unchanged");
    arcade_scalar_owner owner={ARCADE_SCALAR_SETNAME,SIZE_MAX,false,false};
    check(!arcade_scalar_start(owner,ARCADE_SCALAR_ROTATION)&&owner.depth==SIZE_MAX&&owner.invalid&&owner.depth_failed,"owned depth overflow saturates and poisons");
    check(!arcade_scalar_close(owner,ARCADE_SCALAR_SETNAME)&&owner.depth==SIZE_MAX,"depth-failed owner cannot falsely retire");
    arcade_rotation_scan scan={};
    arcade_rotation_append(scan,("vertical "+std::string(5000,'x')+"counter-clockwise"+std::string(5000,'y')).c_str());
    check(scan.prefix_used==8&&scan.suffix_used==17&&scan.seen_ccw&&!std::strcmp(scan.prefix,"vertical"),"bounded rotation scans every byte despite long middle token");
#else
    excluded=true;
#endif
}
static void scenarios(const std::string &s,bool &excluded)
{
    if(s=="rbf-ordinary")rbf(wrap("<rbf>TestCore</rbf>"),"TestCore");
    else if(s=="rbf-nonarcade")rbf(wrap("<RBF>Cores/TestCore</RBF>"),"Cores/TestCore",true,false);
    else if(s=="rbf-comment")rbf(wrap("<rbf>Test<!--note-->Core</rbf>"),"TestCore");
    else if(s=="rbf-pi")rbf(wrap("<rbf>Test<?note audit?>Core</rbf>"),"TestCore");
    else if(s=="rbf-cdata")rbf(wrap("<rbf>Test<![CDATA[Core]]></rbf>"),"TestCore");
    else if(s=="rbf-special-names")rbf("<!DOCTYPE rbf>"+wrap("<!--rbf--><rbf>Test<!--rbf--><?rbf?><![CDATA[]]>Core</rbf><!--rbf-->"),"TestCore");
    else if(s=="rbf-cdata-only")rbf(wrap("<rbf><![CDATA[TestCore]]></rbf>"),"TestCore");
    else if(s=="rbf-repeat")rbf(wrap("<rbf>Test</rbf><rbf>TestCore</rbf><rbf/><rbf></rbf>"),"TestCore");
    else if(s=="rbf-empty")rbf(wrap("<rbf/><rbf></rbf>"),"",false);
    else if(s=="rbf-missing")rbf(wrap("<setname>Game</setname>"),"",false);
    else if(s=="rbf-fit1023")rbf(wrap("<rbf>"+std::string(1023,'a')+"</rbf>"),std::string(1023,'a'));
    else if(s=="rbf-overflow1024")rbf(wrap("<rbf>"+std::string(1024,'a')+"</rbf>"),"",false);
    else if(s=="rbf-seam-fit")rbf(wrap("<rbf>"+std::string(1000,'a')+"<!--note-->"+std::string(23,'b')+"</rbf>"),std::string(1000,'a')+std::string(23,'b'));
    else if(s=="rbf-seam-reject")rbf(wrap("<rbf>"+std::string(1000,'a')+"<![CDATA["+std::string(24,'b')+"]]></rbf>"),"",false);
    else if(s=="rbf-utf8")rbf(wrap("<rbf>"+std::string(1021,'a')+"\xc3\xa9</rbf>"),std::string(1021,'a')+"\xc3\xa9");
    else if(s=="rbf-entity")rbf(wrap("<rbf>Test&amp;<![CDATA[%Core]]></rbf>"),"Test&amp;%Core");
    else if(s=="rbf-nested") {
        rbf(wrap("<rbf>Test<x/>Core</rbf>"),"",false);
        rbf(wrap("<rbf>Cores/Test<x/>Core</rbf>"),"",false,false);
    }
    else if(s=="rbf-nested-rbf")rbf(wrap("<rbf>Test<rbf>Core</rbf></rbf>"),"",false);
    else if(s=="rbf-wrong")rbf(wrap("<rbf>TestCore</rotation>"),"",false);
    else if(s=="rbf-unmatched")rbf(wrap("<rbf>TestCore</rbf></rbf>"),"",false);
    else if(s=="rbf-unclosed")rbf("<misterromdescription><rbf>TestCore<!--note-->","",false);
    else if(s=="rbf-prior-invalid") {
        rbf(wrap("<rbf>TestCore</rbf><rbf>Bad<x/></rbf>"),"",false);
        rbf(wrap("<rbf>TestCore</rbf><rbf>"+std::string(1000,'a')+"<!--note-->"+std::string(24,'b')+"</rbf>"),"",false);
        rbf("<misterromdescription><rbf>TestCore</rbf><rbf>Bad<!--note-->","",false);
    }
    else if(s=="rbf-error")rbf("<misterromdescription><rbf>TestCore</rbf><","",false,true,false,0);
    else if(s=="rbf-open-recovery") {
        rbf(wrap("<rbf>TestCore</rbf>"),"TestCore");
        rbf(wrap("<rbf>Test</rbf>"),"",false,true,true,0);
        rbf(wrap("<rbf>TestCore</rbf>"),"TestCore");
    }
    else if(s=="name-ordinary")name_only("<SETNAME same_dir=\"1\">AlphaBeta</SETNAME>","AlphaBeta",1);
    else if(s=="name-joined")pre("<!DOCTYPE setname>"+wrap("<!--setname--><setname>Alpha<!--setname--><?setname?><![CDATA[]]><![CDATA[Beta]]></setname><!--setname-->"),"AlphaBeta",0,0,false);
    else if(s=="name-31-32") {
        name_only("<setname>"+std::string(31,'a')+"</setname>",std::string(31,'a'));
        name_only("<setname>"+std::string(32,'b')+"</setname>",std::string(32,'b'));
    }
    else if(s=="name-limit") {
        name_only("<setname>"+std::string(1023,'a')+"</setname>",std::string(1023,'a'));
        name_only("<setname>"+std::string(1024,'b')+"</setname>",std::string(1023,'b'));
    }
    else if(s=="name-saturation")name_only("<setname>"+std::string(1000,'a')+"<!--note-->"+std::string(100,'b')+"<![CDATA[tail]]></setname>",std::string(1000,'a')+std::string(23,'b'));
    else if(s=="name-utf8")name_only("<setname>"+std::string(1022,'a')+"<![CDATA[\xc3\xa9]]></setname>",std::string(1022,'a')+"\xc3");
    else if(s=="name-entity")name_only("<setname> Alpha&amp;<![CDATA[%Beta ]]> </setname>"," Alpha&amp;%Beta  ");
    else if(s=="name-self-repeat") {
        name_only("<setname>First</setname><setname/><setname></setname><setname>Last</setname>","Last",0,2);
        pre(wrap("<rotation>vertical ccw</rotation>"),"Last",0,2,true,0);
        pre("<misterromdescription><setname/><rotation></rotation><", "Last",0,0,true,0,1);
        check(active->errors==0,"both empty completion intentionally leaves malformed remainder unread");
        pre("<misterromdescription><rotation/><setname></setname><", "Last",0,0,true,0,1);
        check(active->errors==0,"reverse both empty completion intentionally leaves malformed remainder unread");
    }
    else if(s=="name-samedir")name_only("<setname SAME_DIR=\"1\"/><setname same_dir=\"0\">First</setname><setname same_dir=\"true\">Last</setname>","Last",1,2);
    else if(s=="name-nested-latch")name_only("<setname>Bad<setname same_dir=\"1\">Inner</setname></setname><setname>Good</setname>","Good",1);
    else if(s=="name-wrong")name_only("<setname>Bad</rotation><setname>Good</setname>","Good");
    else if(s=="name-unclosed")pre("<misterromdescription><setname>Bad<!--note-->","",0,0,false,0);
    else if(s=="name-malformed-depth")name_only("<setname>Bad<x></setname><setname>Good</setname>","",0,0);
    else if(s=="name-error")pre("<misterromdescription><setname>Good</setname><rotation>vertical ccw<!--note--><","Good",0,0,false,1,0,false,0);
    else if(s=="name-recovery")name_only("<setname>Bad<x>ignored</x></setname><setname>Good</setname>","Good");
    else if(s=="name-open-nocallback") {
        pre(wrap("<setname same_dir=\"1\">Good</setname><rotation>vertical CCW</rotation>"),"Good",1,2,true,1,1);
        pre(wrap("<setname>Bad</setname>"),"Good",1,2,true,0,0,true,0);
        pre(wrap("<unrelated/>"),"Good",1,0,true,0);
        pre(wrap("<setname>Recover</setname><rotation>horizontal</rotation>"),"Recover",0,0,false,1,1);
    }
    else if(s=="name-early-stop") {
        pre("<misterromdescription><setname>Good</setname><rotation><!--rotation-->vertical<!--note--> ccw</rotation><", "Good",0,2,true,1,1);
        check(active->errors==0&&active->events.size()>3,"intentional completion leaves malformed tail unread");
        bool real_stop=false;
        for(const std::string &event:active->events)if(event.find("\"callback_return\":0")!=std::string::npos&&event.find("\"type\":"+std::to_string(TAG_END))!=std::string::npos)real_stop=true;
        check(real_stop,"intentional stop occurs on real END boundary");
    }
    else if(s=="name-reverse")pre(wrap("<rotation>vertical ccw</rotation><setname>Good</setname><setname>Unread</setname>"),"Good",0,2,true,1,1);
    else if(s=="rotation-tokens") {
        for(const char *token:{"ccw","CCW","counterclockwise","counter-clockwise"})rotation(std::string("VeRtIcAl ")+token,2,true);
    }
    else if(s=="rotation-case-priority") {
        for(const char *text:{"vertical CW","vertical default","vertical cCw","vertical Counterclockwise","vertical COUNTER-CLOCKWISE"})rotation(text,1,true);
        rotation("vertical CW then ccw",2,true); rotation("vertical CCW then CW",2,true); rotation("horizontal ccw",0,false);
    }
    else if(s=="rotation-prefix-seams") {
        rotation("ver<!--note-->ti<?note?>cal coun<![CDATA[ter-clock]]>wise",2,true);
        rotation("vertical c<!--note-->c<?note?>w",2,true);
        rotation("vertical C<?note?>C<![CDATA[W]]>",2,true);
        rotation("vertical counter<!--note-->clock<?note?>wise",2,true);
    }
    else if(s=="rotation-long-middle")rotation("vertical "+std::string(400,'x')+"counter-clockwise"+std::string(2000,'y'),2,true);
    else if(s=="rotation-long-late")rotation("vertical "+std::string(5000,'x')+"CCW",2,true);
    else if(s=="rotation-whitespace") {
        rotation(" vertical ccw",0,false); rotation("vertica",0,false); rotation("vertical",1,true); rotation("   ",0,false);
    }
    else if(s=="rotation-repeat")pre(wrap("<rotation>vertical ccw</rotation><rotation/><rotation></rotation>"),"",0,2,true,0);
    else if(s=="rotation-nested")pre(wrap("<rotation>vertical<x/> ccw</rotation>"),"",0,0,false,0);
    else if(s=="rotation-recovery") {
        pre(wrap("<rotation>vertical<x/> ccw</rotation><rotation>vertical CCW</rotation>"),"",0,2,true,0);
        pre(wrap("<rotation>vertical ccw</setname><rotation>vertical CW</rotation>"),"",0,1,true,0);
        pre("<misterromdescription><rotation>horizontal<!--note-->","",0,0,true,0);
    }
    else if(s=="rotation-cdata-only")rotation("<![CDATA[vertical counterclockwise]]>",2,true);
    else if(s=="private-bounds")private_bounds(excluded);
    else reject("Unknown finite scenario");
}
static std::string parse_json(const Parse &p)
{
    return "{\"api\":"+quote(p.api)+",\"document\":"+bytes(p.document)+",\"injected_open_failure\":"+boolean(p.open_failure)+
        ",\"helper_completed\":"+boolean(p.helper_completed)+",\"parser_return_observed\":"+boolean(p.parser_observed)+",\"parser_return\":"+std::to_string(p.parser_return)+
        ",\"result_observed\":"+boolean(p.helper_completed&&p.api=="get_rbf")+",\"result_nonnull\":"+boolean(p.result_nonnull)+",\"result\":"+bytes(p.result)+",\"rbf_before_matcher\":"+bytes(p.rbf_output)+
        ",\"streams\":"+std::to_string(p.streams)+",\"callback_stops\":"+std::to_string(p.stops)+",\"parser_errors\":"+std::to_string(p.errors)+
        ",\"stops_are_real\":"+boolean(p.stops_are_real)+
        ",\"root_calls\":"+std::to_string(p.roots)+",\"arcade_root_calls\":"+std::to_string(p.arcade_roots)+
        ",\"get_rbf_path_calls\":"+std::to_string(p.path_calls)+
        ",\"final_context\":"+(p.context.empty()?"null":p.context)+",\"events\":"+array(p.events,true)+",\"overrides\":"+array(p.overrides,true)+",\"directory_events\":"+array(p.directories)+"}";
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    const std::string scenario=argv[1]; bool excluded=false,trapped=false; std::string boundary;
    try { scenarios(scenario,excluded); }
    catch(const Boundary &error) {
        if(scenario!="rbf-overflow1024"||HAVE_SCALAR_CONTEXTS) { std::fprintf(stderr,"Unexpected boundary: %s\n",error.what()); return 2; }
        trapped=excluded=true; boundary=error.what();
        check(active&&!active->helper_completed&&!active->parser_observed&&active->directories.empty(),"historical missing-NUL risk ends before copy and consumer outcomes");
    }
    catch(const std::exception &error) { std::fprintf(stderr,"Fixture error: %s\n",error.what()); return 2; }
    std::vector<std::string> rows; for(const Parse &p:parses)rows.emplace_back(parse_json(p));
    const std::string summary="{\"case\":"+quote(scenario)+",\"case_body_completed\":true,\"checks\":"+std::to_string(checks)+",\"failures\":"+std::to_string(failures)+
        ",\"failure_labels\":"+array(failure_labels)+",\"excluded\":"+boolean(excluded)+",\"trapped\":"+boolean(trapped)+",\"ended_boundary\":"+quote(boundary)+
        ",\"exclusion_reason\":"+quote(trapped?"unsafe historical copy ends; no consumer/API result observed":excluded?"private helper absent from historical source":"")+
        ",\"parses\":"+array(rows,true)+",\"setname\":"+bytes(arcade_setname)+",\"override\":"+bytes(ovr_name)+",\"same_dir\":"+std::to_string(ovr_samedir)+
        ",\"vertical\":"+boolean(is_vertical)+",\"direction\":"+std::to_string(rotation_dir)+"}";
    std::printf("JSON %s\n",summary.c_str()); return failures?1:0;
}
