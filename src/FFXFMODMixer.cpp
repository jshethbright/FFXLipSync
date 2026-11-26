#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "FFXVoiceDiagnostics.h"

namespace {
using EventInit = int (WINAPI*)(void*, int, unsigned, void*, unsigned);
using GetCore = int (WINAPI*)(void*, void**);
using SetBuffer = int (WINAPI*)(void*, unsigned, int);
using GetBuffer = int (WINAPI*)(void*, unsigned*, int*);

EventInit originalInit = nullptr;
bool installed = false;

template<typename Function>
Function getExport(HMODULE library, const char* name)
{
    const auto address = GetProcAddress(library, name);
    Function function{};
    static_assert(sizeof function == sizeof address, "Unexpected function pointer size");
    std::memcpy(&function, &address, sizeof function);
    return function;
}

void record(int setResult, int initResult, int getResult, unsigned block, int blocks)
{
    const auto log = CreateFileA("ffx-fmod-mixer.log", FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return;
    char text[192]{};
    const int length = std::snprintf(text, sizeof text,
        "set=%d init=%d query=%d block=%u count=%d\r\n",
        setResult, initResult, getResult, block, blocks);
    DWORD written = 0;
    if (length > 0 && static_cast<size_t>(length) < sizeof text)
        WriteFile(log, text, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(log);
}

int WINAPI initWithSmallerMixer(void* eventSystem, int maxChannels, unsigned flags,
                                void* extraData, unsigned eventFlags)
{
    constexpr unsigned trialBlock = 512;
    constexpr int bufferCount = 4;
    const auto event = GetModuleHandleA("fmod_event.dll");
    const auto core = GetModuleHandleA("fmodex.dll");
    const auto getCore = event ? getExport<GetCore>(event, "FMOD_EventSystem_GetSystemObject") : nullptr;
    const auto set = core ? getExport<SetBuffer>(core, "FMOD_System_SetDSPBufferSize") : nullptr;
    const auto get = core ? getExport<GetBuffer>(core, "FMOD_System_GetDSPBufferSize") : nullptr;
    void* system = nullptr;
    int setResult = -1;
    if (getCore && set && get && getCore(eventSystem, &system) == 0 && system)
        setResult = set(system, trialBlock, bufferCount);
    const int result = originalInit(eventSystem, maxChannels, flags, extraData, eventFlags);
    if (VoiceDiagnostics::enabled()) {
        unsigned block = 0;
        int blocks = 0;
        const int query = system && get ? get(system, &block, &blocks) : -1;
        record(setResult, result, query, block, blocks);
    }
    return result;
}

bool install(HMODULE module)
{
    wchar_t exe[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (length < 8 || length >= MAX_PATH || lstrcmpiW(exe + length - 8, L"\\FFX.exe") != 0)
        return false;

    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    constexpr DWORD siteRva = 0x3068CA;
    constexpr DWORD targetRva = 0x548C76;
    if (pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        pe->OptionalHeader.SizeOfImage <= targetRva + 6)
        return false;
    constexpr BYTE signature[] = {0x6A,0x00,0x6A,0x00,0x6A,0x00,0x68,0x80,0x00,0x00,0x00,
                                  0xFF,0x37,0xE8,0xA7,0x23,0x24,0x00,0x50};
    auto* site = base + siteRva;
    if (std::memcmp(site - 13, signature, sizeof signature) != 0) return false;

    DWORD protection = 0;
    if (!VirtualProtect(site + 1, sizeof(DWORD), PAGE_EXECUTE_READWRITE, &protection))
        return false;
    HMODULE pinned = nullptr;
    const bool active = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned) != 0;
    if (active) {
        originalInit = reinterpret_cast<EventInit>(base + targetRva);
        const auto displacement = reinterpret_cast<std::uintptr_t>(&initWithSmallerMixer) -
                                  reinterpret_cast<std::uintptr_t>(site + 5);
        std::memcpy(site + 1, &displacement, sizeof displacement);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
    }
    DWORD ignored = 0;
    VirtualProtect(site + 1, sizeof(DWORD), protection, &ignored);
    return active;
}
}

struct ModuleVersion { int major, minor, patch; };
extern "C" __declspec(dllexport) ModuleVersion FF10HgetVer() { return {0, 1, 0}; }
extern "C" __declspec(dllexport) const char* FF10HgetName()
{
    return installed ? "FFX FMOD mixer (512 samples)" : "FFX FMOD mixer (inactive)";
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        installed = install(module);
    }
    return TRUE;
}
