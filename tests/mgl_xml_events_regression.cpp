#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <vector>
#include <strings.h>
#include "sxmlc.h"
#include "mgl_xml_events_types.inc"

static std::string document;
static bool fail_open, parser_completed;
static int parser_result=-999;
static unsigned checks,failures,streams,opens,parser_calls,special_boundaries;
static std::vector<std::string> failure_labels,events,parses;
static std::string quote(const std::string &text)
{
    std::string out="\""; const char hex[]="0123456789abcdef";
    for(unsigned char c:text) {
        if(c=='"' || c=='\\') { out+='\\'; out+=c; }
        else if(c=='\n') out+="\\n";
        else if(c=='\r') out+="\\r";
        else if(c=='\t') out+="\\t";
        else if(c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=c;
    }
    return out+'"';
}
static std::string array(const std::vector<std::string> &values,bool raw=false)
{
    std::string out="[";
    for(size_t i=0;i<values.size();i++) { if(i) out+=','; out+=raw?values[i]:quote(values[i]); }
    return out+"]";
}
static void check(bool ok,const char *label)
{
    checks++;
    if(!ok) { failures++; failure_labels.emplace_back(label); std::fprintf(stderr,"FAIL: %s\n",label); }
}
static std::string item(const mgl_item_struct &value)
{
    return "{\"path\":"+quote(value.path)+",\"delay\":"+std::to_string(value.delay)+",\"type\":"+std::to_string(value.type)+
        ",\"index_or_hold\":"+std::to_string(value.index)+",\"valid\":"+std::to_string(value.valid)+",\"submenu\":"+std::to_string(value.submenu)+",\"action\":"+std::to_string(value.action)+"}";
}
static std::string state()
{
    std::vector<std::string> values;
    for(const auto &value:mgl.item) values.push_back(item(value));
    return "{\"count\":"+std::to_string(mgl.count)+",\"current\":"+std::to_string(mgl.current)+",\"timer\":"+std::to_string(mgl.timer)+
        ",\"state\":"+std::to_string(mgl.state)+",\"done\":"+std::to_string(mgl.done)+",\"slots\":"+array(values,true)+"}";
}
extern "C" FILE *mgl_fixture_fopen(const char *name,const char *mode)
{
    if(std::strcmp(mode,"rt")) throw std::runtime_error("Unexpected actual parser stream mode");
    opens++;
    if(fail_open) { errno=ENOENT; return nullptr; }
    FILE *stream=std::tmpfile();
    if(!stream || std::fwrite(document.data(),1,document.size(),stream)!=document.size()) throw std::runtime_error("Disposable seekable parser stream failed");
    std::rewind(stream); streams++;
    if(std::strcmp(name,"/synthetic/fixture.mgl")) throw std::runtime_error("Unexpected synthetic parser name");
    return stream;
}
static int scan_mgl(XMLEvent,const XMLNode*,SXML_CHAR*,int,SAX_Data*);
static int observe_callback(XMLEvent event,const XMLNode *node,SXML_CHAR *text,int n,SAX_Data *data)
{
    const std::string before=state();
    const int result=scan_mgl(event,node,text,n,data);
    const std::string after=state();
    const bool special=(event==XML_EVENT_START_NODE || event==XML_EVENT_END_NODE) && node &&
        node->tag_type!=TAG_FATHER && node->tag_type!=TAG_SELF && node->tag_type!=TAG_END;
    if(special) {
        special_boundaries++;
        check(before==after,"special boundary preserves all typed MGL fields/slots");
    }
    check(result==1,"intact MGL callback preserves its actual continue result");
    events.push_back("{\"event\":"+std::to_string(event)+",\"type\":"+(node?std::to_string(node->tag_type):"null")+
        ",\"tag\":"+(node?quote(node->tag):"null")+",\"text\":"+(text?quote(text):"null")+",\"callback_return\":"+std::to_string(result)+
        ",\"special\":"+(special?"true":"false")+",\"state_before\":"+before+",\"state_after\":"+after+"}");
    return result;
}
static int observed_parse_file(const char *name,const SAX_Callbacks *callbacks,void *user)
{
    if(callbacks->all_event!=scan_mgl || user) throw std::runtime_error("Actual MGL parser wiring changed");
    SAX_Callbacks observed=*callbacks; observed.all_event=observe_callback;
    parser_calls++;
    parser_result=XMLDoc_parse_file_SAX(name,&observed,user); parser_completed=true; return parser_result;
}
#include "mgl_xml_events_production.inc"

static const std::string file_self="<file delay=\"0x1\" type=\"f\" index=\"02\" path=\"Games/Test &amp; One.bin\"/>";
static const std::string reset_self="<reset delay=\"3\" hold=\"04\"/>";
static std::string root(const std::string &body)
{
    return "<mistergamedescription>"+body+"</mistergamedescription>";
}
static void parse(const std::string &xml,bool inject_open_failure=false)
{
    document=xml; fail_open=inject_open_failure; parser_completed=false; parser_result=-999;
    const unsigned opens_before=opens,streams_before=streams,calls_before=parser_calls;
    mgl_struct *result=mgl_parse("/synthetic/fixture.mgl");
    check(result==mgl_get(),"actual MGL parse/accessor returns same object");
    check(parser_completed && parser_result==(inject_open_failure?0:1),"actual parser status observed for complete valid/open-error input");
    check(opens==opens_before+1 && streams==streams_before+(inject_open_failure?0U:1U) && parser_calls==calls_before+1,"exact actual helper/fopen/disposable stream counts");
    parses.push_back("{\"document\":"+quote(xml)+",\"parse_call_completed\":true,\"helper_pointer_observed\":true,\"accessor_matches\":"+(result==mgl_get()?"true":"false")+
        ",\"parser_return_observed\":"+(parser_completed?"true":"false")+",\"parser_return\":"+std::to_string(parser_result)+",\"open_failure_injected\":"+(inject_open_failure?"true":"false")+",\"state\":"+state()+"}");
}
static void expected_two(char type='F',const char *path="Games/Test & One.bin",int index=2)
{
    check(mgl.count==2,"file/reset count and order remain complete");
    const auto &first=mgl.item[0];
    check(first.action==MGL_ACTION_LOAD && first.delay==1 && first.type==type && first.index==index && first.valid==1 && first.submenu==0 && first.path==std::string(path),"first action retains exact delay/type/index/path/valid/defaults");
    const auto &second=mgl.item[1];
    check(second.action==MGL_ACTION_RESET && second.delay==3 && second.hold==4 && second.valid==1 && second.type==0 && second.submenu==0 && second.path[0]==0,"second action retains exact reset delay/hold/valid/defaults");
    check(mgl.current==0 && mgl.timer==0 && mgl.state==0 && mgl.done==0,"parse leaves scheduler fields at actual initial values");
}
int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    try {
        const std::string name=argv[1];
        bool handled=false;
        for(const std::string role:{"neutral","root","file","reset"}) {
            for(const std::string kind:{"comment","pi","cdata"}) {
                if(name!=role+"-"+kind && name!=role+"-"+kind+"-mixed") continue;
                const std::string payload=role=="neutral"?"note":role=="root"?(name.find("-mixed")!=std::string::npos?"MiStErGaMeDeScRiPtIoN":"mistergamedescription"):role;
                const std::string special=kind=="comment"?"<!--"+payload+"-->":kind=="pi"?"<?"+payload+"?>":"<![CDATA["+payload+"]]>";
                parse(root(file_self+special+reset_self)); expected_two();
                check(special_boundaries==2,"actual special START and END both observed");
                handled=true;
            }
        }
        if(!handled) {
            if(name=="ordinary") { parse(root(file_self+reset_self)); expected_two(); }
            else if(name=="real-paired") {
                parse(root("<file delay=\"0x1\" type=\"f\" index=\"02\" path=\"Games/Test &amp; One.bin\"></file><reset delay=\"3\" hold=\"04\"></reset>")); expected_two();
            } else if(name=="real-self-root") { parse("<mistergamedescription/>"); check(mgl.count==0,"real self-closing root keeps empty list"); }
            else if(name=="real-empty-root") { parse(root("")); check(mgl.count==0,"real empty paired root keeps empty list"); }
            else if(name=="real-incomplete-actions") { parse(root("<file/><reset/>")); check(mgl.count==0,"real incomplete actions retain existing rejection/count policy"); }
            else if(name=="empty-then-valid") {
                parse("<mistergamedescription/>"); check(mgl.count==0,"initial real empty script observed");
                parse(root(file_self+reset_self)); expected_two();
            } else if(name=="valid-empty-recovery") {
                parse(root(file_self+reset_self)); expected_two();
                parse(root("")); check(mgl.count==0,"actual mgl_parse retires earlier complete list on empty script");
                parse(root(file_self+reset_self)); expected_two();
            } else if(name=="real-mixed-case") {
                parse("<MiStErGaMeDeScRiPtIoN><FiLe DeLaY=\"0x1\" TyPe=\"s\" InDeX=\"2\" PaTh=\"Disks/Exact.img\"/><ReSeT DeLaY=\"3\" HoLd=\"04\"/></MiStErGaMeDeScRiPtIoN>"); expected_two('S',"Disks/Exact.img",2);
            } else if(name=="open-error-recovery") {
                parse(root(file_self+reset_self)); expected_two();
                parse("",true); check(mgl.count==0,"actual open-error parse returns cleared list");
                parse(root(file_self+reset_self)); expected_two();
            } else return 2;
        }
        std::printf("JSON %s\n",("{\"case\":"+quote(name)+",\"checks\":"+std::to_string(checks)+",\"failures\":"+std::to_string(failures)+
            ",\"case_body_completed\":true,\"excluded\":false,\"trapped\":false,\"special_boundaries\":"+std::to_string(special_boundaries)+",\"failure_labels\":"+array(failure_labels)+
            ",\"parses\":"+array(parses,true)+",\"events\":"+array(events,true)+",\"final_state\":"+state()+"}").c_str());
        return failures?1:0;
    } catch(const std::exception &error) { std::fprintf(stderr,"Fixture error: %s\n",error.what()); return 2; }
}
