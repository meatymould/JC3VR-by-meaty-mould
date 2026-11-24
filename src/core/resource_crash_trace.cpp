#include "core/resource_crash_trace.h"
#include "core/logging.h"
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
namespace jc3vr {
namespace {
HANDLE trace=INVALID_HANDLE_VALUE;
std::uintptr_t image=0;
volatile LONG capturing=0;
// Diagnostic handler never handles/resumes the fault. No log mutex, heap
// allocation, native detour or graphics call in the exception path.
LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
    if(!e || !e->ExceptionRecord || !e->ContextRecord || trace==INVALID_HANDLE_VALUE ||
       !IsResourceCrash(e->ExceptionRecord->ExceptionCode,image,
           reinterpret_cast<std::uintptr_t>(e->ExceptionRecord->ExceptionAddress))) return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedCompareExchange(&capturing,1,0)!=0) return EXCEPTION_CONTINUE_SEARCH;
    const auto& c=*e->ContextRecord;
    char text[4096]{};std::size_t used=0;
    SYSTEMTIME now{};GetLocalTime(&now);
    int n=std::snprintf(text,sizeof(text),
        "\r\nresource-crash %04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu "
        "RIP=%llx RCX=%llx RDI=%llx R12=%llx RSI=%llx RDX=%llx R8=%llx R9=%llx RSP=%llx\r\n",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,
        GetCurrentProcessId(),GetCurrentThreadId(),c.Rip,c.Rcx,c.Rdi,c.R12,c.Rsi,c.Rdx,c.R8,c.R9,c.Rsp);
    if(n>0)used=static_cast<std::size_t>(n);
    const auto read=[&](const char* label,std::uintptr_t address,std::size_t count) {
        std::array<unsigned char,128> bytes{};SIZE_T got=0;
        const BOOL okay=address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),bytes.data(),count,&got);
        const DWORD error=okay?0:GetLastError();
        int size=std::snprintf(text+used,sizeof(text)-used,"%s @%llx read=%d bytes=%llu error=%lu :",
            label,static_cast<unsigned long long>(address),okay,static_cast<unsigned long long>(got),error);
        if(size>0)used+=static_cast<std::size_t>(size);
        for(SIZE_T i=0;i<got && used+4<sizeof(text);++i) {
            size=std::snprintf(text+used,sizeof(text)-used," %02x",bytes[i]);used+=static_cast<std::size_t>(size);
        }
        if(used+3<sizeof(text)){text[used++]='\r';text[used++]='\n';}
    };
    // Capture the heap record the normal Windows minidump omitted. The native
    // consumer resolves tagged [RDI], optionally [untagged+0x28].
    read("native-record",c.Rdi,128);
    std::uintptr_t tagged=0,resolved=0;SIZE_T got=0;
    if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(c.Rdi),&tagged,sizeof(tagged),&got) && got==sizeof(tagged)) {
        resolved=tagged & ~std::uintptr_t{3};
        if(tagged&1) {
            read("indirect-resource",resolved,64);
            if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(resolved+0x28),&resolved,sizeof(resolved),&got) || got!=sizeof(resolved)) resolved=0;
        }
        read("resolved-resource",resolved,64);
    }
    read("pass-context",c.R12,128);
    read("context-bind-slot",c.R12+0x318,32);
    read("consumer-self",c.Rsi,64);
    DWORD written=0;WriteFile(trace,text,static_cast<DWORD>(used),&written,nullptr);
    FlushFileBuffers(trace);
    return EXCEPTION_CONTINUE_SEARCH; // Windows crash reporting remains intact
}
}
void InstallResourceCrashTrace() {
    static std::once_flag once;
    std::call_once(once,[]{
        image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        unsigned char expected[4]{},signature[]{0x0f,0xb7,0x01,0xc3};SIZE_T got=0;
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(image+0xA53C0),expected,4,&got) ||
           got!=4 || std::memcmp(expected,signature,4)) return;
        wchar_t local[MAX_PATH]{};
        const DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
        if(!length || length>=MAX_PATH)return;
        const auto directory=std::filesystem::path(local)/L"JC3VR";
        CreateDirectoryW(directory.c_str(),nullptr);
        trace=CreateFileW((directory/L"resource_crash_trace.log").c_str(),FILE_APPEND_DATA,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(trace==INVALID_HANDLE_VALUE)return;
        if(!AddVectoredExceptionHandler(0,OnException)) {CloseHandle(trace);trace=INVALID_HANDLE_VALUE;return;}
        // Handle/handler intentionally live for process lifetime; JC3VR does
        // not support unloading a live injected DLL and no DllMain cleanup runs.
        TraceRuntime("resource crash trace armed for EXE+A53C0 only; exceptions remain unhandled");
    });
}
}
