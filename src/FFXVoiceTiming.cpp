#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

namespace {
using WaitInit = void (__cdecl*)(void*, int*, void*);
WaitInit originalWaitInit = nullptr;
bool installed = false;

constexpr DWORD tableRva = 0x851D60;
constexpr DWORD waitInitRva = 0x45FD20;
constexpr DWORD waitUpdateRva = 0x45FF70;
constexpr int originalVoiceWait = 5;
constexpr int firstEligibleTick = 1;

void __cdecl initializeVoiceWait(void* worker, int* countdown, void* stack)
{
    originalWaitInit(worker, countdown, stack);
    // The original update checks the pause gates and completes when the count reaches zero.
    if (*countdown == originalVoiceWait)
        *countdown = firstEligibleTick;
}

bool install(HMODULE module)
{
    wchar_t exe[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (length < 8 || length >= MAX_PATH || lstrcmpiW(exe + length - 8, L"\\FFX.exe") != 0)
        return false;

    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        pe->OptionalHeader.SizeOfImage < tableRva + 16)
        return false;

    constexpr BYTE initializer[] = {0x55,0x8B,0xEC,0xFF,0x75,0x10,0xFF,0x75,0x08,0xE8,
                                    0xD2,0xE1,0x00,0x00,0x8B,0x4D,0x0C,0x83,0xC4,0x08,0x89,0x01,0x5D,0xC3};
    if (std::memcmp(base + waitInitRva, initializer, sizeof initializer) != 0)
        return false;

    auto* slot = reinterpret_cast<void**>(base + tableRva);
    if (slot[0] != base + waitInitRva || slot[1] != base + waitUpdateRva || slot[2] || slot[3])
        return false;

    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &oldProtection))
        return false;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(module), &pinned)) {
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof *slot, oldProtection, &ignored);
        return false;
    }

    originalWaitInit = reinterpret_cast<WaitInit>(slot[0]);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&initializeVoiceWait));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof *slot, oldProtection, &ignored);
    return true;
}
}

struct ModuleVersion { int major, minor, patch; };

extern "C" __declspec(dllexport) const char* FF10HgetName()
{
    return installed ? "FFX Voice Timing (voice countdown shortened)" : "FFX Voice Timing (inactive)";
}

extern "C" __declspec(dllexport) ModuleVersion FF10HgetVer()
{
    return {0, 1, 0};
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        installed = install(module);
    }
    return TRUE;
}
