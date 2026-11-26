#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "FFXVoiceJaw.h"
#include "FFXVoiceDiagnostics.h"

namespace {
constexpr size_t talkEnabled = 0x4D4;
constexpr size_t mouthOpening = 0x4DC;
constexpr size_t mouthTarget = 0x4DE;
constexpr size_t mouthHoldTicks = 0x4E1;
constexpr size_t mouthStep = 0x4E3;
constexpr DWORD updateRva = 0x434583;
constexpr DWORD modeRva = 0xF008D9;
using Random = int (__cdecl*)();
const BYTE* animationMode = nullptr;
bool installed = false;
bool voiceInstalled = false;
using UpdateMouth = void (__cdecl*)(BYTE*);
UpdateMouth originalMouth = nullptr;
volatile LONG matchedActors = 0;
bool logMatches = true;
using SetTalk = BYTE* (__cdecl*)(BYTE*, int);
SetTalk originalSetTalk = nullptr;

BYTE* __cdecl setTalkWithCue(BYTE* actor, int enabled)
{
    BYTE* result = originalSetTalk(actor, enabled);
    const bool isEnabled = actor[talkEnabled] != 0;
    if (!isEnabled || *animationMode == 0)
        VoiceJaw::recordMouthCue(actor, isEnabled, GetTickCount());
    return result;
}

bool installMouthCues(BYTE* base)
{
    struct Call { DWORD rva; BYTE original[5]; };
    constexpr Call sites[] = {
        {0x3A3D7C, {0xE8, 0xEF, 0x09, 0x09, 0}},
        {0x3A3F27, {0xE8, 0x44, 0x08, 0x09, 0}},
        {0x3A403C, {0xE8, 0x2F, 0x07, 0x09, 0}},
        {0x679E52, {0xE8, 0x19, 0xA9, 0xDB, 0xFF}},
        {0x67A241, {0xE8, 0x2A, 0xA5, 0xDB, 0xFF}},
    };
    for (const auto& site : sites)
        if (std::memcmp(base + site.rva, site.original, sizeof site.original) != 0)
            return false;
    constexpr BYTE setter[] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8A, 0x4D, 0x0C, 0x88, 0x88,
        0xD4, 0x04, 0, 0, 0x5D, 0xC3
    };
    if (std::memcmp(base + 0x434770, setter, sizeof setter) != 0)
        return false;

    DWORD protections[_countof(sites)]{};
    size_t writable = 0;
    for (; writable < _countof(sites); ++writable) {
        if (!VirtualProtect(base + sites[writable].rva + 1, sizeof(DWORD),
                            PAGE_EXECUTE_READWRITE, &protections[writable])) break;
    }
    const bool active = writable == _countof(sites);
    if (active) originalSetTalk = reinterpret_cast<SetTalk>(base + 0x434770);
    while (writable) {
        --writable;
        BYTE* operand = base + sites[writable].rva + 1;
        if (active) {
            const auto displacement = reinterpret_cast<std::uintptr_t>(&setTalkWithCue)
                - reinterpret_cast<std::uintptr_t>(operand + sizeof(DWORD));
            std::memcpy(operand, &displacement, sizeof displacement);
        }
        DWORD ignored = 0;
        VirtualProtect(operand, sizeof(DWORD), protections[writable], &ignored);
        if (active) FlushInstructionCache(GetCurrentProcess(), operand, sizeof(DWORD));
    }
    return active;
}

struct RenderJaw {
    BYTE* actor = nullptr;
    uint16_t model = 0;
    LONGLONG tick = 0;
    float opening = 0;
    bool textureOpen = false;
};
RenderJaw renderJaws[32]{};
SRWLOCK renderLock = SRWLOCK_INIT;
double ticksPerMillisecond = 0;
const int16_t* textureThreshold = nullptr;
using SetTexture = void (__cdecl*)(int, int, int);
SetTexture originalTexture = nullptr;

// The native mouth routine calls both hooks synchronously on its skeleton worker.
thread_local BYTE* renderingActor = nullptr;
thread_local float renderingOpening = 0;
thread_local bool renderingTextureOpen = false;

float approachOpening(float current, float target, float elapsed)
{
    const bool opening = target > current;
    const float blend = 1.0f - std::exp(-elapsed / (opening ? 29.0f : 45.0f));
    const float travel = elapsed * (opening ? 0.58f : 0.43f);
    const float delta = (std::clamp)((target - current) * blend, -travel, travel);
    return current + delta;
}

bool chooseTexture(bool open, float value, float threshold)
{
    return open ? value > (std::max)(0.0f, threshold - 3.0f) : value > threshold + 4.0f;
}

bool renderJaw(BYTE* actor, uint16_t model, int voiceOpening, float& opening, bool& texture)
{
    LARGE_INTEGER clock{};
    QueryPerformanceCounter(&clock);
    const auto tick = clock.QuadPart;
    AcquireSRWLockExclusive(&renderLock);
    RenderJaw* state = nullptr;
    auto* oldest = &renderJaws[0];
    for (auto& entry : renderJaws) {
        if (entry.actor == actor && entry.model == model) {
            state = &entry;
            break;
        }
        if (entry.tick < oldest->tick) oldest = &entry;
    }
    const bool procedural = *animationMode == 0 && actor[talkEnabled];
    if (!procedural) {
        if (state) *state = {};
        ReleaseSRWLockExclusive(&renderLock);
        return false;
    }

    const double elapsed = state ? static_cast<double>(tick - state->tick) / ticksPerMillisecond : 0.0;
    if (!state || elapsed > 220.0) {
        if (voiceOpening < 0) {
            if (state) *state = {};
            ReleaseSRWLockExclusive(&renderLock);
            return false;
        }
        if (!state) state = oldest;
        int16_t nativeOpening;
        std::memcpy(&nativeOpening, actor + mouthOpening, sizeof nativeOpening);
        const auto initial = static_cast<float>((std::clamp)(static_cast<int>(nativeOpening), 0, 69));
        *state = RenderJaw{actor, model, tick, initial, initial > *textureThreshold};
    } else {
        state->opening = approachOpening(state->opening, static_cast<float>((std::max)(0, voiceOpening)),
                                         static_cast<float>((std::clamp)(elapsed, 0.0, 50.0)));
        state->tick = tick;
    }

    opening = state->opening;
    texture = state->textureOpen = chooseTexture(state->textureOpen, opening, *textureThreshold);
    if (voiceOpening < 0 && opening < 0.5f) {
        opening = 0;
        texture = false;
        *state = {};
    }
    ReleaseSRWLockExclusive(&renderLock);
    return true;
}

float __cdecl renderOpeningAsFloat(BYTE* actor, int nativeOpening)
{
    return renderingActor == actor ? renderingOpening : static_cast<float>(nativeOpening);
}

void __cdecl renderMouthTexture(int texture, int layer, int frame)
{
    originalTexture(texture, layer, renderingActor ? static_cast<int>(renderingTextureOpen) : frame);
}

void logSpeaker(int who)
{
    const LONG bit = 1 << who;
    if (InterlockedOr(&matchedActors, bit) & bit) return;
    constexpr char messages[VoiceJaw::slotCount][32] = {
        "", "Tidus", "Yuna", "Auron", "Kimahri", "Wakka", "Lulu", "Rikku", "script-selected speaker"
    };
    const auto log = CreateFileA("ffx-voice-jaw.log", FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return;
    static constexpr char prefix[] = "Audio-driven jaw matched: ";
    DWORD written = 0;
    WriteFile(log, prefix, static_cast<DWORD>(sizeof prefix - 1), &written, nullptr);
    WriteFile(log, messages[who], static_cast<DWORD>(std::strlen(messages[who])), &written, nullptr);
    WriteFile(log, "\r\n", 2, &written, nullptr);
    CloseHandle(log);
}

void __cdecl updateMouthFromVoice(BYTE* actor)
{
    uint16_t model;
    std::memcpy(&model, actor, sizeof model);
    const int modelSpeaker = model >= 101 && model <= 107 ? model - 100 : model;
    const bool bound = VoiceJaw::readBoundActor() == actor;
    const int who = bound ? VoiceJaw::otherSpeaker : modelSpeaker;
    const int target = bound || (who > 0 && who < VoiceJaw::castSize) ? VoiceJaw::readOpening(who) : -1;

    float opening = 0;
    bool texture = false;
    if (renderJaw(actor, model, target, opening, texture)) {
        const auto quantized = static_cast<int16_t>(opening + 0.5f);
        std::memcpy(actor + mouthOpening, &quantized, sizeof quantized);
        std::memcpy(actor + mouthTarget, &quantized, sizeof quantized);
        actor[mouthHoldTicks] = 2;
        renderingActor = actor;
        renderingOpening = opening;
        renderingTextureOpen = texture;
        if (logMatches && target >= 0) logSpeaker(who);
    }
    originalMouth(actor);
    renderingActor = nullptr;
}

bool installVoiceMouth(BYTE* base, const IMAGE_NT_HEADERS32* nt)
{
    constexpr BYTE jawSignature[] = {0x57, 0xE8, 0x3A, 0x0F, 0x00, 0x00, 0x57, 0xE8, 0x14, 0x13, 0x00, 0x00};
    constexpr BYTE floatSignature[] = {0x89, 0x4D, 0x08, 0xDB, 0x45, 0x08, 0xD9, 0x5D, 0x08, 0xD9, 0x45, 0x08};
    constexpr BYTE textureSignature[] = {0xE8, 0x77, 0xA2, 0xF4, 0xFF};
    BYTE* jaw = base + 0x4335B0;
    BYTE* floating = base + 0x43463B;
    BYTE* texture = base + 0x434724;
    if (std::memcmp(jaw, jawSignature, sizeof jawSignature) != 0 ||
        std::memcmp(floating, floatSignature, sizeof floatSignature) != 0 ||
        std::memcmp(texture, textureSignature, sizeof textureSignature) != 0)
        return false;

    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return false;
    ticksPerMillisecond = static_cast<double>(frequency.QuadPart) / 1000.0;
    textureThreshold = reinterpret_cast<const int16_t*>(base + 0x849718);

    struct Patch { BYTE* code; size_t size; };
    const Patch patches[] = {{jaw + 2, 4}, {floating, 12}, {texture + 1, 4}};
    DWORD protections[_countof(patches)]{};
    size_t writable = 0;
    for (; writable < _countof(patches); ++writable) {
        if (!VirtualProtect(patches[writable].code, patches[writable].size,
                            PAGE_EXECUTE_READWRITE, &protections[writable])) break;
    }
    const bool active = writable == _countof(patches) && VoiceJaw::install(base, nt);
    if (active) {
        originalMouth = reinterpret_cast<UpdateMouth>(base + 0x4344F0);
        originalTexture = reinterpret_cast<SetTexture>(base + 0x37E9A0);
        const auto jawDisplacement = reinterpret_cast<std::uintptr_t>(&updateMouthFromVoice)
            - reinterpret_cast<std::uintptr_t>(jaw + 6);
        const auto textureDisplacement = reinterpret_cast<std::uintptr_t>(&renderMouthTexture)
            - reinterpret_cast<std::uintptr_t>(texture + 5);
        // Preserve the bone pointer in EDX. The cdecl helper returns its float in ST(0).
        BYTE floatCall[] = {0x52, 0x51, 0x57, 0xE8, 0, 0, 0, 0, 0x83, 0xC4, 0x08, 0x5A};
        const auto floatDisplacement = reinterpret_cast<std::uintptr_t>(&renderOpeningAsFloat)
            - reinterpret_cast<std::uintptr_t>(floating + 8);
        std::memcpy(floatCall + 4, &floatDisplacement, sizeof floatDisplacement);
        std::memcpy(jaw + 2, &jawDisplacement, sizeof jawDisplacement);
        std::memcpy(floating, floatCall, sizeof floatCall);
        std::memcpy(texture + 1, &textureDisplacement, sizeof textureDisplacement);
        VoiceJaw::actorBindingAvailable = installMouthCues(base);
    }
    while (writable) {
        --writable;
        const auto& patch = patches[writable];
        DWORD ignored = 0;
        VirtualProtect(patch.code, patch.size, protections[writable], &ignored);
        if (active) FlushInstructionCache(GetCurrentProcess(), patch.code, patch.size);
    }
    return active;
}

void setMouthCadence(BYTE* actor, unsigned intervalDraw, unsigned stepDraw, BYTE mode)
{
    if (mode == 0 && actor[talkEnabled]) {
        actor[mouthHoldTicks] = static_cast<BYTE>(4 + (intervalDraw & 3));
        actor[mouthStep] = actor[mouthTarget] == 0 ? 8 : 6;
    } else {
        actor[mouthHoldTicks] = static_cast<BYTE>(1 + (intervalDraw & 3));
        actor[mouthStep] = static_cast<BYTE>(1 + ((stepDraw & 255) % 20));
    }
}

void __cdecl updateMouthCadence(BYTE* actor, Random random)
{
    const unsigned intervalDraw = static_cast<unsigned>(random()) >> 5;
    const unsigned stepDraw = static_cast<unsigned>(random()) >> 5;
    setMouthCadence(actor, intervalDraw, stepDraw, *animationMode);
}

bool installMouthCadence()
{
    wchar_t file[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, file, MAX_PATH);
    if (length < 8 || length >= MAX_PATH || lstrcmpiW(file + length - 8, L"\\FFX.exe") != 0)
        return false;

    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
        || nt->OptionalHeader.SizeOfImage <= modeRva)
        return false;

    constexpr BYTE original[] = {
        0xFF, 0xD6, 0xC1, 0xF8, 0x05, 0x24, 0x03, 0xFE, 0xC0, 0x88, 0x87, 0xE1, 0x04,
        0x00, 0x00, 0xFF, 0xD6, 0xC1, 0xF8, 0x05, 0x0F, 0xB6, 0xC0, 0x99, 0xB9, 0x14,
        0x00, 0x00, 0x00, 0xF7, 0xF9, 0xFE, 0xC2, 0x88, 0x97, 0xE3, 0x04, 0x00, 0x00
    };
    BYTE* code = base + updateRva;
    if (std::memcmp(code, original, sizeof original) != 0)
        return false;

    BYTE replacement[sizeof original];
    std::memset(replacement, 0x90, sizeof replacement);
    // push esi (rand), push edi (actor), call updateMouthCadence, add esp, 8.
    constexpr BYTE prefix[] = {0x56, 0x57, 0xE8, 0, 0, 0, 0, 0x83, 0xC4, 0x08};
    std::memcpy(replacement, prefix, sizeof prefix);
    const auto displacement = reinterpret_cast<std::uintptr_t>(&updateMouthCadence)
        - reinterpret_cast<std::uintptr_t>(code + 7);
    std::memcpy(replacement + 3, &displacement, sizeof displacement);

    DWORD previous = 0;
    if (!VirtualProtect(code, sizeof replacement, PAGE_EXECUTE_READWRITE, &previous))
        return false;
    animationMode = base + modeRva;
    std::memcpy(code, replacement, sizeof replacement);
    DWORD ignored = 0;
    VirtualProtect(code, sizeof replacement, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof replacement);
    logMatches = VoiceDiagnostics::enabled();
    voiceInstalled = installVoiceMouth(base, nt);
    return true;
}
}

struct ModuleVersion { int major, minor, patch; };
extern "C" __declspec(dllexport) const char* FF10HgetName()
{
    if (voiceInstalled && VoiceJaw::actorBindingAvailable) return "FFX Voice-Driven Jaw with Script Speaker Cues";
    if (voiceInstalled) return "FFX Voice-Driven Jaw for Main Party";
    return installed ? "FFX Procedural Mouth Smoothing (voice unavailable)" : "FFX Procedural Mouth Smoothing (inactive)";
}

extern "C" __declspec(dllexport) ModuleVersion FF10HgetVer()
{
    return {1, 3, 0};
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HMODULE pinned = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&installed), &pinned))
            installed = installMouthCadence();
    }
    return TRUE;
}

#ifdef FFX_MOUTH_TEST
int main()
{
    if (VoiceJaw::speaker("voice/010204td") != 1 ||
        VoiceJaw::speaker("voice_bank/033704yn.aif") != 2 ||
        VoiceJaw::speaker("032906rr2") != 6 ||
        VoiceJaw::speaker("010503ga") != VoiceJaw::otherSpeaker ||
        VoiceJaw::speaker("x") != 0 ||
        VoiceJaw::speaker("010204td_loop") != 0)
        return 10;

    BYTE first[1]{}, second[1]{};
    VoiceJaw::recordMouthCue(first, true, 1000);
    if (VoiceJaw::uniqueMouthCue(980, 1000) != first ||
        VoiceJaw::uniqueMouthCue(1300, 1300) != nullptr) return 13;
    VoiceJaw::recordMouthCue(second, true, 1010);
    if (VoiceJaw::uniqueMouthCue(980, 1010) != nullptr) return 14;
    VoiceJaw::recordMouthCue(second, false, 1020);
    if (VoiceJaw::uniqueMouthCue(980, 1020) != first) return 15;

    VoiceJaw::Envelope envelope;
    if (envelope.sample(0.0f, 20) != 0 || envelope.sample(0.2f, 20) < 10)
        return 11;
    for (int i = 0; i < 30; ++i) envelope.sample(0.0f, 20);
    if (envelope.sample(0.0f, 20) != 0)
        return 12;
    for (int fps : {30, 60, 120, 240}) {
        const float frame = 1000.0f / static_cast<float>(fps);
        float opening = 0;
        for (int step = 0; step < fps / 10; ++step) {
            const float next = approachOpening(opening, 54, frame);
            if (next <= opening || next - opening > 0.581f * frame) return 16;
            opening = next;
        }
        if (opening < 42 || opening >= 54) return 17;
        for (int step = 0; step < fps / 4; ++step) {
            const float next = approachOpening(opening, 0, frame);
            if (next >= opening || opening - next > 0.431f * frame) return 18;
            opening = next;
        }
        if (opening >= 1.0f) return 19;
    }
    if (chooseTexture(false, 13, 10) || !chooseTexture(false, 15, 10) ||
        !chooseTexture(true, 9, 10) || chooseTexture(true, 6, 10)) return 20;

    BYTE actor[0x500]{};
    for (unsigned draw = 0; draw != 1024; ++draw) {
        actor[talkEnabled] = 1;
        actor[mouthTarget] = 40;
        setMouthCadence(actor, draw, draw, 0);
        if (actor[mouthHoldTicks] != 4 + draw % 4 || actor[mouthStep] != 6)
            return 1;
        actor[mouthTarget] = 0;
        setMouthCadence(actor, draw, draw, 0);
        if (actor[mouthStep] != 8)
            return 2;
        setMouthCadence(actor, draw, draw, 1);
        if (actor[mouthHoldTicks] != 1 + draw % 4 || actor[mouthStep] != 1 + (draw % 256) % 20)
            return 3;
        actor[talkEnabled] = 0;
        setMouthCadence(actor, draw, draw, 0);
        if (actor[mouthHoldTicks] != 1 + draw % 4 || actor[mouthStep] != 1 + (draw % 256) % 20)
            return 4;
    }
    return 0;
}
#endif
