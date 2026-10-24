#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <cstddef>
#include <mutex>
#include <chrono>
#include "global.h"
#ifdef WECHATAGENT_VOICE_TEST
extern "C" uintptr_t VoiceTestModule();
extern "C" uintptr_t VoiceTestResolve(uintptr_t rva);
#endif
namespace WeixinSend {struct InnerStruct2;InnerStruct2* BuildSendParam1(uint64_t);}
#include "voice_sender.h"
#include "json.hpp"
#include "voice_sender_workspace.h"

namespace WeChatAgentVoice {
namespace {
using json=nlohmann::json;
SRWLOCK logLock=SRWLOCK_INIT;
alignas(8) volatile LONG64 sequence=0;
constexpr uint64_t maxBytes=8*1024*1024;
constexpr int64_t maxDurationMs=60000;

struct File {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    File()=default;
    File(const File&)=delete;
    File& operator=(const File&)=delete;
};

std::string Utf8(const std::wstring& value){
    if(value.empty())return {};
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(count<=0)return {};
    std::string text(count,'\0');
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),text.data(),count,nullptr,nullptr)!=count)return {};
    return text;
}
bool Wide(const std::string& text,std::wstring& value){
    if(text.empty())return false;
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(count<=0)return false;
    value.resize(count);
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),value.data(),count)==count;
}
bool Clean(const std::string& text){
    for(unsigned char c:text)if(c<32||c==127)return false;
    return true;
}
bool LocalPath(const std::wstring& text){
    if(text.size()<3||!((text[0]>=L'A'&&text[0]<=L'Z')||(text[0]>=L'a'&&text[0]<=L'z'))||text[1]!=L':'||(text[2]!=L'\\'&&text[2]!=L'/'))return false;
    // Device names, network paths and alternate streams are not audio file inputs.
    return text.find(L':',2)==std::wstring::npos;
}
std::string Now(){
    SYSTEMTIME time{};GetSystemTime(&time);char text[40]{};
    sprintf_s(text,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);
    return text;
}
const std::filesystem::path& StateRoot(){
    // The build script records its workspace; an injected DLL's location is not the data root.
    static const std::filesystem::path root=std::filesystem::path(kWeChatAgentVoiceWorkspace)/L"state";
    return root;
}
std::filesystem::path LogPath(){return StateRoot()/L"logs"/L"voice-sender.log";}

bool Persist(const json& record,DWORD& error) noexcept {
    error=ERROR_SUCCESS;
    try{
        const auto state=StateRoot(),directory=state/L"logs",path=directory/L"voice-sender.log";
        const auto line=record.dump(-1,' ',false,json::error_handler_t::replace)+"\r\n";
        if(!CreateDirectoryW(state.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){error=GetLastError();return false;}
        if(!CreateDirectoryW(directory.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){error=GetLastError();return false;}
        AcquireSRWLockExclusive(&logLock);
        // A flushed record must precede a risky stage; CRT buffers can disappear with the host.
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
        if(file==INVALID_HANDLE_VALUE)error=GetLastError();
        else{
            LARGE_INTEGER zero{};DWORD written=0;
            if(!SetFilePointerEx(file,zero,nullptr,FILE_END))error=GetLastError();
            else if(!WriteFile(file,line.data(),static_cast<DWORD>(line.size()),&written,nullptr))error=GetLastError();
            else if(written!=line.size())error=ERROR_WRITE_FAULT;
            else if(!FlushFileBuffers(file))error=GetLastError();
            CloseHandle(file);
        }
        ReleaseSRWLockExclusive(&logLock);
        return error==ERROR_SUCCESS;
    }catch(...){error=ERROR_WRITE_FAULT;return false;}
}
struct RequestLog {
    std::string id;
    bool live=false,attempted=false,dispatched=false;
    RequestLog(){
        char value[120]{};
        sprintf_s(value,"%lu-%lu-%llu-%lld",GetCurrentProcessId(),GetCurrentThreadId(),GetTickCount64(),InterlockedIncrement64(&sequence));
        id=value;
    }
    bool Stage(const char* event,const json& detail,httplib::Response& response){
        DWORD error=0;
        if(Persist({{"timestampUtc",Now()},{"pid",GetCurrentProcessId()},{"tid",GetCurrentThreadId()},{"requestId",id},{"event",event},{"detail",detail}},error))return true;
        response.status=500;
        response.set_content(json({{"ret",1},{"error","voice_log_write_failed"},{"win32Error",error},{"requestId",id},{"logPath",Utf8(LogPath().wstring())},{"nativeCallAttempted",attempted},{"dispatchAttempted",dispatched},{"deliveryUncertain",dispatched},{"retryAllowed",false}}).dump(),"application/json");
        return false;
    }
    void Reject(httplib::Response& response,int status,const char* error,DWORD code=0){
        json result={{"ret",1},{"error",error},{"requestId",id},{"dryRun",!live},{"nativeCallAttempted",attempted}};
        if(code)result["win32Error"]=code;
        if(!Stage("rejected",result,response))return;
        response.status=status;response.set_content(result.dump(),"application/json");
    }
};

json Plan(const std::string& target,const std::string& path,int64_t duration,uint64_t bytes,bool prefix){
    return {
        {"module","Weixin.dll"},{"targetVersion","4.1.10.27"},{"executable",false},
        {"input",{{"wxidorgid",target},{"path",path},{"durationMs",duration},{"fileBytes",bytes},{"silkHeaderOffset",prefix?1:0},{"durationVerified",false},{"silkFramesVerified",false}}},
        {"candidateCallChain",{"0x54B02A0","0x6199D10","0x619A190","0x619B3B0","0x1649B50","0x1676B20","0x1676DD0","0x1677A30"}},
        {"object",{{"candidateVtableRva","0x8DD0028"},{"candidateOwnerVtableRva","0x8DCFF98"},{"commonTargetOffset","0xB0"},{"typeOffset","0xD8"},{"type",34},{"candidatePayloadOffset","0x708"},{"candidateDurationOffset","0x728"},{"fieldSemanticsVerified",false}}},
        {"dispatch",{{"taskVtableRva","0x84EC9C8"},{"slotOffset","0x10"},{"rva","0x1677A30"}}},
        {"unresolved",{"constructor argument semantics","payload is bytes/path/id","duration unit","extra metadata","native string/allocator ABI","thread and asynchronous ownership"}}
    };
}


// Native strings cross a CRT boundary; only the observed x64 layout crosses it.
struct NativeString {
    union { unsigned char sso[16]; unsigned char* pointer; };
    uint64_t size;
    uint64_t capacity;
};
struct Shared { uintptr_t body,owner; };
struct Param2Block { uint64_t vtable,temp[6],self; };
struct Extra { uint32_t codec,mode,limit; unsigned char reserved[52]; };
static_assert(sizeof(NativeString)==32&&offsetof(NativeString,size)==0x10&&offsetof(NativeString,capacity)==0x18);
static_assert(sizeof(Shared)==16&&sizeof(Extra)==64&&sizeof(Param2Block)==64&&offsetof(Param2Block,self)==0x38);
volatile LONG liveAttemptConsumed=0;   // [repeatable] 累计发送次数
volatile LONG64 liveLastSendMs=0;       // [repeatable] 上次发送时刻（GetTickCount64）
constexpr LONG liveAttemptCap=200;      // [repeatable] 每进程上限，防止无界泄漏
constexpr LONG64 liveCooldownMs=1500;   // [repeatable] 两条之间最小间隔
constexpr uintptr_t voiceFactory=0x619A190,voiceCtor=0x619B3B0;
constexpr uintptr_t voiceOwnerVtable=0x8DCFF98,voiceVtable=0x8DD0028;
constexpr uintptr_t sendMessage=0x1677A30,createParam2=0xDF40,param2Global=0xA0CE0B0;
constexpr uintptr_t taskVtable=0x84EC9C8;
static_assert(offset::send_message==sendMessage&&offset::create_param2==createParam2);
using CreateVoice=Shared*(*)(Shared*,const NativeString*,const NativeString*,const uint32_t*,const Extra*);
using Dispatch=__int64(*)(uint64_t,uint64_t);
using CreateParam2=__int64(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);

std::string Hex(uint64_t value){char text[24]{};sprintf_s(text,"0x%llX",value);return text;}
uintptr_t CurrentBase(){
#ifdef WECHATAGENT_VOICE_TEST
    return VoiceTestModule();
#else
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Weixin.dll"));
#endif
}
uintptr_t FunctionAddress(uintptr_t base,uintptr_t rva){
#ifdef WECHATAGENT_VOICE_TEST
    return VoiceTestResolve(rva);
#else
    return base+rva;
#endif
}
bool Memory(uintptr_t address,size_t length,bool executable=false,bool writable=false){
    if(!address||!length||address+length<address)return false;
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto end=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
    if(address+length>end)return false;
    const DWORD access=info.Protect&0xFF;
    if(executable)return access==PAGE_EXECUTE||access==PAGE_EXECUTE_READ||access==PAGE_EXECUTE_READWRITE||access==PAGE_EXECUTE_WRITECOPY;
    if(writable)return access==PAGE_READWRITE||access==PAGE_WRITECOPY||access==PAGE_EXECUTE_READWRITE||access==PAGE_EXECUTE_WRITECOPY;
    return access==PAGE_READONLY||access==PAGE_READWRITE||access==PAGE_WRITECOPY||access==PAGE_EXECUTE_READ||access==PAGE_EXECUTE_READWRITE||access==PAGE_EXECUTE_WRITECOPY;
}
// Keep SEH in POD-only leaves: C++ destructors and __try cannot share an MSVC frame.
bool Ready(uintptr_t expected) noexcept {
    __try{
        if(!expected||CurrentBase()!=expected||!Memory(expected,sizeof(IMAGE_DOS_HEADER)))return false;
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(expected);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||dos->e_lfanew>0x100000)return false;
        const uintptr_t peAddress=expected+static_cast<uintptr_t>(dos->e_lfanew);
        if(!Memory(peAddress,sizeof(IMAGE_NT_HEADERS64)))return false;
        const auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS64*>(peAddress);
        if(pe->Signature!=IMAGE_NT_SIGNATURE||pe->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||pe->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return false;
        const auto imageBytes=pe->OptionalHeader.SizeOfImage;
        const uintptr_t functions[]={voiceFactory,voiceCtor,sendMessage,createParam2};
        for(const auto rva:functions){
            MEMORY_BASIC_INFORMATION info{};
            if(rva>=imageBytes||!Memory(expected+rva,1,true)||!VirtualQuery(reinterpret_cast<void*>(expected+rva),&info,sizeof(info))||reinterpret_cast<uintptr_t>(info.AllocationBase)!=expected)return false;
        }
        const uintptr_t tables[]={voiceOwnerVtable,voiceVtable,taskVtable,offset::param2_1,offset::param2_2,offset::param2_3};
        for(const auto rva:tables){
            if(rva>=imageBytes||imageBytes-rva<8||!Memory(expected+rva,8))return false;
            const auto first=*reinterpret_cast<const uintptr_t*>(expected+rva);
            if(first<expected||first-expected>=imageBytes||!Memory(first,1,true))return false;
        }
        if(param2Global>=imageBytes||imageBytes-param2Global<8||!Memory(expected+param2Global,8))return false;
        return CurrentBase()==expected;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct Fault { DWORD code=0;uintptr_t address=0; };
int Capture(EXCEPTION_POINTERS* info,Fault* fault) noexcept {
    fault->code=info->ExceptionRecord->ExceptionCode;
    fault->address=reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    // C++ exceptions must unwind to Handle; only native SEH belongs in this leaf.
    return fault->code==0xE06D7363?EXCEPTION_CONTINUE_SEARCH:EXCEPTION_EXECUTE_HANDLER;
}
bool CallFactory(uintptr_t base,Shared* out,const NativeString* target,const NativeString* silk,const uint32_t* duration,const Extra* extra,Shared** returned,Fault* fault) {
    __try{*returned=reinterpret_cast<CreateVoice>(FunctionAddress(base,voiceFactory))(out,target,silk,duration,extra);return true;}
    __except(Capture(GetExceptionInformation(),fault)){return false;}
}
bool PrepareBody(uintptr_t base,Shared* value,Fault* fault) noexcept {
    __try{
        if(!Memory(reinterpret_cast<uintptr_t>(value),sizeof(*value))||!value->owner||value->body!=value->owner+0x10||
           !Memory(value->owner,0x10)||!Memory(value->body,8)||!Memory(value->body+0xD8,4,false,true))return false;
        if(*reinterpret_cast<const uintptr_t*>(value->owner)!=base+voiceOwnerVtable||*reinterpret_cast<const uintptr_t*>(value->body)!=base+voiceVtable)return false;
        // Native caller 0x6199D65 assigns 34 after the factory; the factory alone is incomplete.
        *reinterpret_cast<uint32_t*>(value->body+0xD8)=34;
        return true;
    }__except(Capture(GetExceptionInformation(),fault)){return false;}
}
bool CallFirst(uintptr_t owner,uintptr_t* first,Fault* fault){
    __try{*first=reinterpret_cast<uintptr_t>(WeixinSend::BuildSendParam1(owner));return true;}
    __except(Capture(GetExceptionInformation(),fault)){return false;}
}
bool ReadParam2Global(uintptr_t base,uint64_t* global,Fault* fault) noexcept {
    __try{*global=*reinterpret_cast<const uint64_t*>(base+param2Global);return true;}
    __except(Capture(GetExceptionInformation(),fault)){return false;}
}
bool CallSecond(uintptr_t base,uint64_t* second,Param2Block* p2,Param2Block* p3,Param2Block* p4,uint64_t* buf,uint64_t global,Fault* fault){
    __try{
        reinterpret_cast<CreateParam2>(FunctionAddress(base,createParam2))(
            reinterpret_cast<uint64_t>(second),reinterpret_cast<uint64_t>(p2),reinterpret_cast<uint64_t>(p3),
            reinterpret_cast<uint64_t>(p4),reinterpret_cast<uint64_t>(buf),global);
        return true;
    }__except(Capture(GetExceptionInformation(),fault)){return false;}
}
bool CallDispatch(uintptr_t base,uintptr_t first,uintptr_t second,__int64* returned,Fault* fault){
    __try{*returned=reinterpret_cast<Dispatch>(FunctionAddress(base,sendMessage))(first,second);return true;}
    __except(Capture(GetExceptionInformation(),fault)){return false;}
}

void* Retain(size_t bytes){return HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,bytes);}
bool FillString(NativeString* value,const unsigned char* bytes,size_t count){
    value->size=count;value->capacity=count<=15?15:(count|size_t(15));
    if(value->capacity==15){if(count)memcpy(value->sso,bytes,count);return true;}
    value->pointer=static_cast<unsigned char*>(Retain(static_cast<size_t>(value->capacity)+1));
    if(!value->pointer)return false;
    memcpy(value->pointer,bytes,count);return true;
}
bool LiveFailure(RequestLog& log,httplib::Response& response,int status,const char* error,const Fault& fault={}){
    json detail={{"ret",1},{"error",error},{"dryRun",false},{"nativeCallAttempted",log.attempted},{"dispatchAttempted",log.dispatched},
        {"deliveryUncertain",log.dispatched},{"requestId",log.id},{"retryAllowed",false},{"logPath",Utf8(LogPath().wstring())}};
    if(fault.code){detail["exceptionCode"]=Hex(fault.code);detail["exceptionAddress"]=Hex(fault.address);}
    if(!log.Stage("live_failed",detail,response))return false;
    response.status=status;response.set_content(detail.dump(),"application/json");return false;
}
bool Before(RequestLog& log,httplib::Response& response,uintptr_t base,const char* stage,const json& parameters){
    if(!Ready(base))return LiveFailure(log,response,409,"voice_module_changed_or_not_ready");
    const DWORD gate=GetFileAttributesW((StateRoot()/L"hook"/L"voice-live.enable").c_str());
    if(gate==INVALID_FILE_ATTRIBUTES||(gate&FILE_ATTRIBUTE_DIRECTORY))return LiveFailure(log,response,409,"voice_live_gate_removed_or_unreadable");
    if(!log.Stage(stage,parameters,response))return false;
    // Disk flush can block. Recheck after it so no native call uses a stale module snapshot.
    if(!Ready(base))return LiveFailure(log,response,409,"voice_module_changed_or_not_ready");
    const DWORD latestGate=GetFileAttributesW((StateRoot()/L"hook"/L"voice-live.enable").c_str());
    if(latestGate==INVALID_FILE_ATTRIBUTES||(latestGate&FILE_ATTRIBUTE_DIRECTORY))return LiveFailure(log,response,409,"voice_live_gate_removed_or_unreadable");
    return true;
}

// LIVE ABI experiment: SEH cannot repair heap corruption or stop asynchronous work.
// Remove voice-live.enable to stop later calls. After a crash, stop the host, restore
// the previously deployed DLL, then restart; deleting the marker cannot unload this DLL.
void Live(HANDLE file,const std::string& target,uint32_t duration,uint64_t fileBytes,bool prefixed,
          httplib::Response& response,RequestLog& log,std::timed_mutex& nativeSendMutex){
    log.live=true;
    std::unique_lock<std::timed_mutex> lock(nativeSendMutex,std::defer_lock);
    if(!lock.try_lock_for(std::chrono::seconds(1))){LiveFailure(log,response,429,"voice_sender_busy");return;}
    const LONG seq=InterlockedIncrement(&liveAttemptConsumed);
    if(seq>liveAttemptCap){LiveFailure(log,response,429,"voice_live_attempt_cap_reached");return;}
    const LONG64 nowMs=static_cast<LONG64>(GetTickCount64());
    const LONG64 lastMs=InterlockedCompareExchange64(&liveLastSendMs,0,0);
    if(lastMs&&nowMs-lastMs<liveCooldownMs){LiveFailure(log,response,429,"voice_live_cooldown");return;}
    const uintptr_t base=CurrentBase();
    if(!Ready(base)){LiveFailure(log,response,409,"voice_module_changed_or_not_ready");return;}

    // [repeatable] 不再一次性锁死；改为记录本次发送时刻，配合上面的冷却与进程上限。
    InterlockedExchange64(&liveLastSendMs,nowMs);
    // Retain inputs and wrapper ownership for the entire process. Premature release risks
    // async use-after-free; the native allocator/deleter contract is not established.
    // HeapAlloc buffers are borrowed inputs, never std::string instances/destructors in our CRT.
    auto out=static_cast<Shared*>(Retain(sizeof(Shared)));
    auto nativeTarget=static_cast<NativeString*>(Retain(sizeof(NativeString)));
    auto nativeSilk=static_cast<NativeString*>(Retain(sizeof(NativeString)));
    auto nativeDuration=static_cast<uint32_t*>(Retain(sizeof(uint32_t)));
    auto extra=static_cast<Extra*>(Retain(sizeof(Extra)));
    auto second=static_cast<uint64_t*>(Retain(0xE8));
    auto param2Buffer=static_cast<uint64_t*>(Retain(0x10));
    auto p2=static_cast<Param2Block*>(Retain(sizeof(Param2Block)));
    auto p3=static_cast<Param2Block*>(Retain(sizeof(Param2Block)));
    auto p4=static_cast<Param2Block*>(Retain(sizeof(Param2Block)));
    const size_t normalizedBytes=static_cast<size_t>(fileBytes)+(prefixed?0:1);
    auto payload=static_cast<unsigned char*>(Retain(normalizedBytes+1));
    if(!out||!nativeTarget||!nativeSilk||!nativeDuration||!extra||!second||!param2Buffer||!p2||!p3||!p4||!payload){LiveFailure(log,response,500,"voice_allocation_failed");return;}
    LARGE_INTEGER zero{};DWORD read=0;
    if(!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)||!ReadFile(file,payload+(prefixed?0:1),static_cast<DWORD>(fileBytes),&read,nullptr)||read!=fileBytes){
        LiveFailure(log,response,500,"voice_payload_read_failed");return;
    }
    if(!prefixed)payload[0]=2;
    if(normalizedBytes<=10||payload[0]!=2||memcmp(payload+1,"#!SILK_V3",9)){
        LiveFailure(log,response,400,"invalid_silk_header");return;
    }
    if(!FillString(nativeTarget,reinterpret_cast<const unsigned char*>(target.data()),target.size())||
       !FillString(nativeSilk,payload,normalizedBytes)){LiveFailure(log,response,500,"voice_allocation_failed");return;}
    *nativeDuration=duration;extra->codec=4;extra->mode=3;extra->limit=30000;
    // Mirror the existing helper's executable C++ argument order, whose nearby
    // register comment disagrees with its actual call. Do not "fix" the working file path.
    p2->vtable=base+offset::param2_1;p2->self=reinterpret_cast<uint64_t>(p2);
    p3->vtable=base+offset::param2_2;p3->self=reinterpret_cast<uint64_t>(p3);
    p4->vtable=base+offset::param2_3;p4->self=reinterpret_cast<uint64_t>(p4);
    const json args={{"strategy","A"},{"base",Hex(base)},{"factoryRva","0x619A190"},{"rcxOut",Hex(reinterpret_cast<uintptr_t>(out))},
        {"rdxTarget",Hex(reinterpret_cast<uintptr_t>(nativeTarget))},{"r8Silk",Hex(reinterpret_cast<uintptr_t>(nativeSilk))},
        {"r9DurationPointer",Hex(reinterpret_cast<uintptr_t>(nativeDuration))},{"stackExtraPointer",Hex(reinterpret_cast<uintptr_t>(extra))},
        {"targetBytes",nativeTarget->size},{"silkBytes",nativeSilk->size},{"silkPrefixAdded",!prefixed},
        {"durationMs",duration},{"extra",{4,3,30000}},{"retryAllowed",false}};
    if(!log.Stage("live_prepared",args,response))return;
    if(!Before(log,response,base,"before_voice_factory",args))return;
    Fault fault{};Shared* returned=nullptr;
    log.attempted=true;
    if(!CallFactory(base,out,nativeTarget,nativeSilk,nativeDuration,extra,&returned,&fault)){
        LiveFailure(log,response,500,"voice_factory_exception",fault);return;
    }
    if(returned!=out){LiveFailure(log,response,500,"voice_factory_return_mismatch");return;}
    if(!log.Stage("voice_factory_returned",{{"out",Hex(reinterpret_cast<uintptr_t>(out))},{"body",Hex(out->body)},{"owner",Hex(out->owner)}},response))return;
    if(!PrepareBody(base,out,&fault)){LiveFailure(log,response,500,"voice_object_layout_rejected",fault);return;}
    if(!log.Stage("voice_object_ready",{{"body",Hex(out->body)},{"owner",Hex(out->owner)},{"type",34}},response))return;

    // A mirrors the working file sender: BuildSendParam1 supplies task vtable 0x84EC9C8.
    // A' is deliberately NOT executed: 0x1649B50 is a native task-wrapper candidate,
    // but its prototype/ownership are unverified. Never guess it or fall back after A.
    uintptr_t first=0;
    if(!Before(log,response,base,"before_build_send_param1",{{"helper","WeixinSend::BuildSendParam1"},{"nativeRva",nullptr},
        {"owner",Hex(out->owner)},{"taskVtableRva","0x84EC9C8"} }))return;
    if(!CallFirst(out->owner,&first,&fault)||!first){LiveFailure(log,response,500,"voice_param1_failed",fault);return;}
    // A voice-local copy of BuildSendParam2_Image exposes all six native arguments
    // before the disk flush. The shared helper and all existing senders remain unchanged.
    uint64_t globalValue=0;
    if(!Ready(base)){LiveFailure(log,response,409,"voice_module_changed_or_not_ready");return;}
    if(!ReadParam2Global(base,&globalValue,&fault)){LiveFailure(log,response,500,"voice_param2_global_read_failed",fault);return;}
    if(!Before(log,response,base,"before_build_send_param2",{{"helper","voice-local BuildSendParam2_Image mirror"},{"nativeRva","0xDF40"},
        {"rcxSecond",Hex(reinterpret_cast<uintptr_t>(second))},{"rdxP2",Hex(reinterpret_cast<uintptr_t>(p2))},
        {"r8P3",Hex(reinterpret_cast<uintptr_t>(p3))},{"r9P4",Hex(reinterpret_cast<uintptr_t>(p4))},
        {"stackArg5Buffer",Hex(reinterpret_cast<uintptr_t>(param2Buffer))},{"stackArg6GlobalValue",Hex(globalValue)},
        {"globalValueAddress",Hex(base+param2Global)} }))return;
    if(!CallSecond(base,second,p2,p3,p4,param2Buffer,globalValue,&fault)){LiveFailure(log,response,500,"voice_param2_exception",fault);return;}
    if(!Before(log,response,base,"before_voice_dispatch",{{"nativeRva","0x1677A30"},{"rcxFirst",Hex(first)},
        {"rdxSecond",Hex(reinterpret_cast<uintptr_t>(second))},{"type",34},{"retryAllowed",false}}))return;
    __int64 dispatchReturn=0;
    log.dispatched=true;
    if(!CallDispatch(base,first,reinterpret_cast<uintptr_t>(second),&dispatchReturn,&fault)){
        LiveFailure(log,response,500,"voice_dispatch_exception",fault);return;
    }
    json result={{"ret",0},{"retmsg","accepted_unconfirmed"},{"dryRun",false},{"nativeCallAttempted",true},{"dispatchAttempted",true},
        {"deliveryConfirmed",false},{"confirmation","以 reader 回执为准"},{"retryAllowed",false},{"requestId",log.id},
        {"logPath",Utf8(LogPath().wstring())},{"strategy","A"},{"nativeReturn",Hex(static_cast<uint64_t>(dispatchReturn))}};
    if(!log.Stage("voice_dispatch_returned",result,response))return;
    response.status=200;response.set_content(result.dump(),"application/json");
}

void Process(const httplib::Request& request,httplib::Response& response,RequestLog& log,std::timed_mutex& nativeSendMutex){
    if(!log.Stage("request_received",{{"route","/SendVoiceMsg"},{"bodyBytes",request.body.size()}},response))return;
    const auto data=json::parse(request.body,nullptr,false);
    if(data.is_discarded()||!data.is_object()||!data.contains("wxidorgid")||!data["wxidorgid"].is_string()||!data.contains("path")||!data["path"].is_string()||!data.contains("durationMs")||!data["durationMs"].is_number_integer()){
        log.Reject(response,400,"invalid_voice_request");return;
    }
    const auto target=data["wxidorgid"].get<std::string>(),path=data["path"].get<std::string>();
    const auto& suppliedDuration=data["durationMs"];
    // These are local dry-run limits, not claims about the protocol's accepted range.
    if(suppliedDuration.is_number_unsigned()&&suppliedDuration.get<uint64_t>()>static_cast<uint64_t>(maxDurationMs)){
        log.Reject(response,400,"invalid_duration_ms");return;
    }
    const int64_t duration=suppliedDuration.get<int64_t>();
    std::wstring wideTarget,widePath;
    if(target.empty()||target.size()>256||path.empty()||path.size()>4096||!Clean(target)||!Clean(path)||!Wide(target,wideTarget)||!Wide(path,widePath)||!LocalPath(widePath)||duration<1||duration>maxDurationMs){
        log.Reject(response,400,"invalid_voice_parameters");return;
    }
    const auto gate=StateRoot()/L"hook"/L"voice-live.enable";
    const DWORD attributes=GetFileAttributesW(gate.c_str());
    const DWORD gateError=attributes==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
    if(attributes==INVALID_FILE_ATTRIBUTES&&gateError!=ERROR_FILE_NOT_FOUND&&gateError!=ERROR_PATH_NOT_FOUND){
        log.Reject(response,500,"voice_live_gate_unreadable",gateError);return;
    }
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)){
        log.Reject(response,409,"voice_live_gate_is_directory");return;
    }
    const bool liveRequested=attributes!=INVALID_FILE_ATTRIBUTES;
    if(!log.Stage("parameters_validated",{{"wxidorgid",target},{"path",path},{"durationMs",duration},{"liveRequested",liveRequested}},response))return;
    File file;
    file.value=CreateFileW(widePath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file.value==INVALID_HANDLE_VALUE){log.Reject(response,400,"voice_file_open_failed",GetLastError());return;}
    BY_HANDLE_FILE_INFORMATION info{};
    if(GetFileType(file.value)!=FILE_TYPE_DISK||!GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){
        log.Reject(response,400,"voice_input_not_regular_file");return;
    }
    const uint64_t bytes=(static_cast<uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    if(bytes<=9||bytes>maxBytes){log.Reject(response,400,"invalid_voice_file_size");return;}
    unsigned char header[10]{};DWORD count=0;
    if(!ReadFile(file.value,header,sizeof(header),&count,nullptr)){log.Reject(response,400,"voice_file_read_failed",GetLastError());return;}
    const bool plain=count>=9&&!memcmp(header,"#!SILK_V3",9);
    // Some Tencent SILK containers precede the same nine-byte header with 0x02.
    const bool prefixed=count>=10&&header[0]==2&&!memcmp(header+1,"#!SILK_V3",9);
    if((!plain&&!prefixed)||bytes<=static_cast<uint64_t>(prefixed?10:9)){
        log.Reject(response,400,"invalid_silk_header");return;
    }
    if(!log.Stage("silk_header_validated",{{"fileBytes",bytes},{"headerOffset",prefixed?1:0},{"framesVerified",false}},response))return;
    if(liveRequested){Live(file.value,target,static_cast<uint32_t>(duration),bytes,prefixed,response,log,nativeSendMutex);return;}
    auto plan=Plan(target,path,duration,bytes,prefixed);
    json result={{"ret",0},{"retmsg","dry_run"},{"dryRun",true},{"liveRequested",liveRequested},{"nativeCallAttempted",false},{"requestId",log.id},{"logPath",Utf8(LogPath().wstring())},{"plan",plan}};
    if(!log.Stage("dry_run_plan",result,response))return;
    response.status=200;
    response.set_content(result.dump(),"application/json");
}
}

void Handle(const httplib::Request& request,httplib::Response& response,std::timed_mutex& nativeSendMutex){
    RequestLog log;
    try{Process(request,response,log,nativeSendMutex);}
    catch(...){
        if(log.live)LiveFailure(log,response,500,"voice_live_cpp_exception");
        else log.Reject(response,500,"voice_dry_run_failed");
    }
}
}

