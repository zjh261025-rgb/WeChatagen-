#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include "MinHook.h"
#include "voice_probe.h"
#include "voice_sender_workspace.h"

extern "C" {
    void VoiceProbeEntry();
    void* VoiceProbeTrampoline=nullptr;
    uint32_t VoiceProbeUseXsave=0,VoiceProbeMaskLo=0,VoiceProbeMaskHi=0;
}
namespace {
constexpr uintptr_t ctorRva=0x619B3B0,bodyVtableRva=0x8DD0028,destructorRva=0x619B7C0;
constexpr size_t dumpBytes=96,maxXstateBytes=16384;
struct RawContext {
    uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rbx,rdx,rcx,rax,flags;
};
static_assert(sizeof(RawContext)==128&&offsetof(RawContext,rcx)==104);
enum ReadStatus:DWORD {ReadOk=0,NullPointer=1,Overflow=2,QueryFailed=3,NotCommitted=4,Guarded=5,NotReadable=6,ReadFault=7};
struct Dump {
    DWORD status=NullPointer,exception=0;
    unsigned count=0;
    unsigned char bytes[dumpBytes]{};
};
struct Observation {
    SYSTEMTIME utc{};
    DWORD pid=0,tid=0,hit=0,exception=0;
    uint64_t rsp=0,rcx=0,rdx=0,r8=0,r9=0,extraSlot=0,extra=0;
    Dump inputA{},inputB{},stackExtra{};
    // [v2] 指向内容的快照：目标字符串 / 音频缓冲 / Extra 数据
    Dump stringA{},audioData{},extraData{};
    DWORD extraPointerStatus=NullPointer;
};
struct Slot { volatile LONG ready=0;Observation value{}; };
Slot slots[3]{};
volatile LONG attempted=0,captureEnabled=0,hits=0,active=0,stopReason=0,installDone=0,workerDone=0,reportedLogError=0;
HANDLE signalEvent=nullptr,logFile=INVALID_HANDLE_VALUE;
void* target=nullptr;
SRWLOCK logLock=SRWLOCK_INIT;
wchar_t markerPath[32768]{},logPath[32768]{},modulePath[32768]{};
unsigned char versionBuffer[65536]{};
enum Stop:LONG {None=0,LimitReached=1,MemoryFailure=2,LogFailure=3,MarkerRemoved=4,InstallFailure=5};

void RequestStop(LONG reason) noexcept {
    InterlockedCompareExchange(&stopReason,reason,None);
    if(signalEvent)SetEvent(signalEvent);
}
void LogFallback() noexcept {
    if(InterlockedCompareExchange(&reportedLogError,1,0)==0)
        OutputDebugStringA("[WeChatAgent] voice-probe log unavailable; capture abandoned, inspect marker and ACL.\n");
}
void Stamp(const SYSTEMTIME& t,char* out,size_t size) noexcept {
    sprintf_s(out,size,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
}
bool Persist(const char* text) noexcept {
    DWORD written=0;bool ok=false;LARGE_INTEGER zero{};
    AcquireSRWLockExclusive(&logLock);
    if(logFile!=INVALID_HANDLE_VALUE){
        const auto size=static_cast<DWORD>(strlen(text));
        ok=SetFilePointerEx(logFile,zero,nullptr,FILE_END)&&WriteFile(logFile,text,size,&written,nullptr)&&written==size&&FlushFileBuffers(logFile);
    }
    ReleaseSRWLockExclusive(&logLock);
    if(!ok){LogFallback();RequestStop(LogFailure);}
    return ok;
}
bool Status(const char* event,LONG code=0) noexcept {
    SYSTEMTIME time{};GetSystemTime(&time);char utc[40]{},line[640]{};Stamp(time,utc,sizeof(utc));
    sprintf_s(line,"{\"utc\":\"%s\",\"pid\":%lu,\"tid\":%lu,\"hit\":0,\"event\":\"%s\",\"code\":%ld,\"registers\":null,\"pointers\":null}\r\n",utc,GetCurrentProcessId(),GetCurrentThreadId(),event,code);
    return Persist(line);
}
bool MarkerPresent() noexcept {
    const DWORD attrs=GetFileAttributesW(markerPath);
    return attrs!=INVALID_FILE_ATTRIBUTES&&!(attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
bool OpenLog() noexcept {
    wchar_t state[32768]{},directory[32768]{};
    if(swprintf_s(state,L"%s\\state",kWeChatAgentVoiceWorkspace)<0||swprintf_s(directory,L"%s\\logs",state)<0||
       swprintf_s(logPath,L"%s\\voice-probe.log",directory)<0)return false;
    if(!CreateDirectoryW(state,nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;
    if(!CreateDirectoryW(directory,nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;
    logFile=CreateFileW(logPath,GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    return logFile!=INVALID_HANDLE_VALUE;
}
DWORD Readable(uintptr_t address,size_t size) noexcept {
    if(!address)return NullPointer;
    if(address>UINTPTR_MAX-size)return Overflow;
    const uintptr_t end=address+size;
    while(address<end){
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<const void*>(address),&info,sizeof(info)))return QueryFailed;
        if(info.State!=MEM_COMMIT)return NotCommitted;
        if(info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return Guarded;
        if(!(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return NotReadable;
        const auto region=reinterpret_cast<uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-region||region+info.RegionSize<=address)return Overflow;
        address=region+info.RegionSize;
    }
    return ReadOk;
}
void Copy(uintptr_t address,size_t size,Dump& result) noexcept {
    result.status=Readable(address,size);
    if(result.status!=ReadOk)return;
    // VirtualQuery is a snapshot, not a lifetime guarantee; the actual read still needs SEH.
    __try {
        auto* source=reinterpret_cast<const volatile unsigned char*>(address);
        for(size_t i=0;i<size;++i)result.bytes[i]=source[i];
        result.count=static_cast<unsigned>(size);
    }__except(EXCEPTION_EXECUTE_HANDLER){
        result.status=ReadFault;result.exception=GetExceptionCode();result.count=0;
    }
}
void Capture(const RawContext* raw,Observation& out,DWORD hit) noexcept {
    GetSystemTime(&out.utc);out.pid=GetCurrentProcessId();out.tid=GetCurrentThreadId();out.hit=hit;
    out.rcx=raw->rcx;out.rdx=raw->rdx;out.r8=raw->r8;out.r9=raw->r9;
    out.rsp=reinterpret_cast<uintptr_t>(raw)+sizeof(RawContext);
    out.extraSlot=out.rsp+0x28;
    Copy(out.extraSlot,sizeof(uintptr_t),out.stackExtra);
    if(out.stackExtra.status==ReadOk){
        memcpy(&out.extra,out.stackExtra.bytes,sizeof(out.extra));
        out.extraPointerStatus=Readable(out.extra,1);
    }
    Copy(out.rdx,dumpBytes,out.inputA);Copy(out.r8,dumpBytes,out.inputB);
    // [v2] rdx 按 MSVC std::string 解释：+0x10=size、+0x18=capacity，capacity>15 时正文在 *(void**)rdx。
    if(out.inputA.status==ReadOk){
        uintptr_t text=0;size_t size=0;
        memcpy(&text,out.inputA.bytes,sizeof(text));
        memcpy(&size,out.inputA.bytes+0x10,sizeof(size));
        if(size<=4096){const uintptr_t address=size>15?text:out.rdx;const size_t want=size>63?64:(size?size:1);Copy(address,want,out.stringA);}
    }
    // [v2] r8 指向音频缓冲描述符：+0x00=数据指针、+0x08=长度、+0x10=容量。只看前 64 字节，确认 silk 头。
    if(out.inputB.status==ReadOk){
        uintptr_t buffer=0;size_t size=0;
        memcpy(&buffer,out.inputB.bytes,sizeof(buffer));
        memcpy(&size,out.inputB.bytes+0x10,sizeof(size)); // MSVC std::string: size@+0x10, capacity@+0x18
        if(size<=64u*1024u*1024u){const size_t want=size>63?64:(size?size:1);Copy(buffer,want,out.audioData);}
    }
    if(out.extraPointerStatus==ReadOk)Copy(out.extra,64,out.extraData);
}
void Hex(const Dump& dump,char* text) noexcept {
    static constexpr char digits[]="0123456789abcdef";
    for(unsigned i=0;i<dump.count;++i){text[i*2]=digits[dump.bytes[i]>>4];text[i*2+1]=digits[dump.bytes[i]&15];}
    text[dump.count*2]='\0';
}
bool WriteObservation(const Observation& s) noexcept {
    char utc[40]{},a[193]{},b[193]{},extra[28]="null",line[4096]{};
    char sa[129]{},ad[129]{},ed[129]{};
    Stamp(s.utc,utc,sizeof(utc));Hex(s.inputA,a);Hex(s.inputB,b);
    Hex(s.stringA,sa);Hex(s.audioData,ad);Hex(s.extraData,ed);
    if(s.stackExtra.status==ReadOk)sprintf_s(extra,"\"0x%016llX\"",s.extra);
    sprintf_s(line,
        "{\"utc\":\"%s\",\"pid\":%lu,\"tid\":%lu,\"hit\":%lu,\"event\":\"hit\",\"exception\":%lu,"
        "\"registers\":{\"rcx\":\"0x%016llX\",\"rdx\":\"0x%016llX\",\"r8\":\"0x%016llX\",\"r9\":\"0x%016llX\",\"r9d\":%lu,\"entryRsp\":\"0x%016llX\",\"extraSlot\":\"0x%016llX\",\"stackExtra\":%s},"
        "\"pointers\":{\"rdx\":{\"readable\":%s,\"status\":%lu,\"exception\":%lu,\"bytes\":%u,\"hex\":\"%s\"},"
        "\"r8\":{\"readable\":%s,\"status\":%lu,\"exception\":%lu,\"bytes\":%u,\"hex\":\"%s\"},"
        "\"extraSlot\":{\"readable\":%s,\"status\":%lu,\"exception\":%lu},\"extraPointer\":{\"readable\":%s,\"status\":%lu}},"
        "\"content\":{\"stringA\":{\"status\":%lu,\"bytes\":%u,\"hex\":\"%s\"},"
        "\"audioData\":{\"status\":%lu,\"bytes\":%u,\"hex\":\"%s\"},"
        "\"extraData\":{\"status\":%lu,\"bytes\":%u,\"hex\":\"%s\"}}}\r\n",
        utc,s.pid,s.tid,s.hit,s.exception,s.rcx,s.rdx,s.r8,s.r9,static_cast<DWORD>(s.r9),s.rsp,s.extraSlot,extra,
        s.inputA.status==ReadOk?"true":"false",s.inputA.status,s.inputA.exception,s.inputA.count,a,
        s.inputB.status==ReadOk?"true":"false",s.inputB.status,s.inputB.exception,s.inputB.count,b,
        s.stackExtra.status==ReadOk?"true":"false",s.stackExtra.status,s.stackExtra.exception,
        s.extraPointerStatus==ReadOk?"true":"false",s.extraPointerStatus,
        s.stringA.status,s.stringA.count,sa,
        s.audioData.status,s.audioData.count,ad,
        s.extraData.status,s.extraData.count,ed);
    return Persist(line);
}
bool CpuStateReady() noexcept {
    int regs[4]{};__cpuid(regs,1);
    if((regs[2]&(1<<26))&&(regs[2]&(1<<27))){
        const auto mask=_xgetbv(0);__cpuidex(regs,0xD,0);
        const auto size=static_cast<unsigned>(regs[1]);
        if(size<512||size>maxXstateBytes){Status("xstate_size_unsupported",static_cast<LONG>(size));return false;}
        VoiceProbeMaskLo=static_cast<uint32_t>(mask);VoiceProbeMaskHi=static_cast<uint32_t>(mask>>32);VoiceProbeUseXsave=1;
    }
    return true;
}
bool Executable(uintptr_t address,HMODULE module) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))&&info.AllocationBase==module&&info.State==MEM_COMMIT&&
        !(info.Protect&(PAGE_GUARD|PAGE_NOACCESS))&&(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
bool Ready() noexcept {
    auto module=GetModuleHandleW(L"Weixin.dll");if(!module){Status("module_absent");return false;}
    const auto base=reinterpret_cast<uintptr_t>(module);
    __try{
        auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x100000){Status("bad_dos_header");return false;}
        auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(pe->Signature!=IMAGE_NT_SIGNATURE||pe->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||pe->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||
           pe->OptionalHeader.SizeOfImage<=bodyVtableRva+sizeof(uintptr_t)||base>UINTPTR_MAX-pe->OptionalHeader.SizeOfImage){Status("bad_pe_header");return false;}
        if(!Executable(base+ctorRva,module)||!Executable(base+destructorRva,module)){Status("target_not_executable");return false;}
        const unsigned char expected[]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x83,0xEC,0x68,0x48,0x8D,0x6C,0x24,0x60};
        Dump code{},table{};Copy(base+ctorRva,sizeof(expected),code);Copy(base+bodyVtableRva,sizeof(uintptr_t),table);
        uintptr_t first=0;if(table.status==ReadOk)memcpy(&first,table.bytes,sizeof(first));
        if(code.status!=ReadOk||memcmp(code.bytes,expected,sizeof(expected))){Status("signature_mismatch");return false;}
        if(table.status!=ReadOk||first!=base+destructorRva){Status("vtable_mismatch");return false;}
        const DWORD n=GetModuleFileNameW(module,modulePath,static_cast<DWORD>(sizeof(modulePath)/sizeof(modulePath[0])));
        DWORD unused=0,size=0;if(!n||n>=static_cast<DWORD>(sizeof(modulePath)/sizeof(modulePath[0]))||(size=GetFileVersionInfoSizeW(modulePath,&unused))==0||size>sizeof(versionBuffer)||
           !GetFileVersionInfoW(modulePath,0,size,versionBuffer)){Status("version_read_failed");return false;}
        VS_FIXEDFILEINFO* version=nullptr;UINT bytes=0;
        if(!VerQueryValueW(versionBuffer,L"\\",reinterpret_cast<void**>(&version),&bytes)||bytes<sizeof(*version)||!version||
           version->dwFileVersionMS!=MAKELONG(1,4)||version->dwFileVersionLS!=MAKELONG(27,10)){Status("version_mismatch");return false;}
        target=reinterpret_cast<void*>(base+ctorRva);return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){Status("ready_read_fault",static_cast<LONG>(GetExceptionCode()));return false;}
}
MH_STATUS Disable() noexcept {
    __try{return MH_DisableHook(target);}
    __except(EXCEPTION_EXECUTE_HANDLER){Status("disable_exception",static_cast<LONG>(GetExceptionCode()));return MH_UNKNOWN;}
}
void Drain() noexcept {
    for(auto& slot:slots)if(InterlockedCompareExchange(&slot.ready,2,1)==1)WriteObservation(slot.value);
}
DWORD WINAPI Worker(LPVOID) noexcept {
    __try {
        while(!InterlockedCompareExchange(&installDone,0,0))WaitForSingleObject(signalEvent,1000);
        for(;;){
            const DWORD wait=WaitForSingleObject(signalEvent,1000);
            if(wait==WAIT_FAILED){Status("worker_wait_failed",GetLastError());RequestStop(InstallFailure);}
            Drain();
            if(!MarkerPresent())RequestStop(MarkerRemoved);
            if(InterlockedCompareExchange(&stopReason,0,0)!=None)break;
        }
        InterlockedExchange(&captureEnabled,0);
        const auto status=Disable();
        Status(status==MH_OK||status==MH_ERROR_DISABLED?"hook_disabled":"disable_failed",static_cast<LONG>(status));
        // Do not free trampolines or the queue: another thread may already be in this detour.
        const auto until=GetTickCount64()+2000;
        while(InterlockedCompareExchange(&active,0,0)&&GetTickCount64()<until){WaitForSingleObject(signalEvent,10);Drain();}
        Drain();Status("probe_stopped",InterlockedCompareExchange(&stopReason,0,0));
    }__except(EXCEPTION_EXECUTE_HANDLER){
        InterlockedExchange(&captureEnabled,0);Status("worker_exception",static_cast<LONG>(GetExceptionCode()));Disable();
    }
    AcquireSRWLockExclusive(&logLock);
    if(logFile!=INVALID_HANDLE_VALUE){CloseHandle(logFile);logFile=INVALID_HANDLE_VALUE;}
    ReleaseSRWLockExclusive(&logLock);
    // Keep the event valid for late callbacks; its single handle is reclaimed on process exit.
    InterlockedExchange(&workerDone,1);return 0;
}
void Arm() noexcept {
    if(!CpuStateReady())return;
    signalEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!signalEvent){Status("event_create_failed",GetLastError());return;}
    const auto created=MH_CreateHook(target,reinterpret_cast<void*>(&VoiceProbeEntry),&VoiceProbeTrampoline);
    if(created!=MH_OK||!VoiceProbeTrampoline){Status("create_hook_failed",static_cast<LONG>(created));return;}
    // The existing deferred initializer pins this DLL. No loader polling or second MH_Initialize.
    HANDLE worker=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
    if(!worker){Status("worker_create_failed",GetLastError());return;}
    CloseHandle(worker);
    InterlockedExchange(&captureEnabled,1);
    const auto enabled=MH_EnableHook(target);
    if(enabled!=MH_OK){InterlockedExchange(&captureEnabled,0);Status("enable_hook_failed",static_cast<LONG>(enabled));RequestStop(InstallFailure);}
    else if(!Status("hook_armed"))RequestStop(LogFailure);
    InterlockedExchange(&installDone,1);SetEvent(signalEvent);
}
void InitializeInner(void* testTarget=nullptr) noexcept {
    if(swprintf_s(markerPath,L"%s\\state\\hook\\voice-probe.enable",kWeChatAgentVoiceWorkspace)<0||!MarkerPresent())return;
    if(InterlockedCompareExchange(&attempted,1,0)!=0)return;
    if(!OpenLog()){LogFallback();return;}
    if(!Status("probe_initializing"))return;
#ifdef VOICE_PROBE_TEST
    if(testTarget)target=testTarget;else
#else
    (void)testTarget;
#endif
    if(!Ready())return;
    Arm();
}
}
extern "C" void VoiceProbeObserve(const RawContext* raw) noexcept {
    if(!InterlockedCompareExchange(&captureEnabled,0,0))return;
    InterlockedIncrement(&active);
    if(!InterlockedCompareExchange(&captureEnabled,0,0)){InterlockedDecrement(&active);return;}
    const LONG hit=InterlockedIncrement(&hits);
    if(hit>=1&&hit<=3){
        auto& slot=slots[hit-1];
        __try{Capture(raw,slot.value,static_cast<DWORD>(hit));}
        __except(EXCEPTION_EXECUTE_HANDLER){slot.value.hit=hit;slot.value.exception=GetExceptionCode();RequestStop(MemoryFailure);}
        InterlockedExchange(&slot.ready,1);
        if(slot.value.inputA.status!=ReadOk||slot.value.inputB.status!=ReadOk||slot.value.stackExtra.status!=ReadOk||slot.value.extraPointerStatus!=ReadOk)RequestStop(MemoryFailure);
        if(hit==3)RequestStop(LimitReached);
        SetEvent(signalEvent);
    }else RequestStop(LimitReached);
    InterlockedDecrement(&active);if(signalEvent)SetEvent(signalEvent);
}
namespace WeChatAgentVoiceProbe {
void Initialize() noexcept {
    __try{InitializeInner();}
    __except(EXCEPTION_EXECUTE_HANDLER){
        InterlockedExchange(&captureEnabled,0);Status("initialize_exception",static_cast<LONG>(GetExceptionCode()));
        RequestStop(InstallFailure);InterlockedExchange(&installDone,1);if(signalEvent)SetEvent(signalEvent);
    }
}
}
#ifdef VOICE_PROBE_TEST
extern "C" void VoiceProbeTestInitialize(void* p) noexcept {InitializeInner(p);}
extern "C" LONG VoiceProbeTestWorkerDone() noexcept {return InterlockedCompareExchange(&workerDone,0,0);}
extern "C" void VoiceProbeTestStop() noexcept {RequestStop(MarkerRemoved);}
#endif
