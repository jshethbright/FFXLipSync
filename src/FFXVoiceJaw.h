#pragma once
#include <cmath>

namespace VoiceJaw {
constexpr int castSize = 8;
constexpr int otherSpeaker = castSize;
constexpr int slotCount = castSize + 1;

int speaker(const char* name) {
    if (!name) return 0;
    const char* leaf = name;
    for (const char* p = name; *p; ++p)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    for (int i = 0; i < 6; ++i)
        if (leaf[i] < '0' || leaf[i] > '9') return 0;
    if (leaf[6] < 'a' || leaf[6] > 'z' || leaf[7] < 'a' || leaf[7] > 'z') return 0;
    const char* end = leaf + 8;
    while (*end >= '0' && *end <= '9') ++end;
    if (*end && std::strcmp(end, ".aif") && std::strcmp(end, ".wav")) return 0;
    constexpr char tags[castSize][3] = {"", "td", "yn", "an", "km", "wk", "rr", "rk"};
    for (int i = 1; i < castSize; ++i)
        if (leaf[6] == tags[i][0] && leaf[7] == tags[i][1]) return i;
    return otherSpeaker;
}

struct Envelope {
    float peak = 0.10f;
    float opening = 0;
    unsigned sample(float rms, DWORD elapsed) {
        const auto decay = std::exp(-static_cast<float>(elapsed) / 850.0f);
        peak = (std::max)(0.065f, (std::max)(rms, peak * decay));
        const auto noise = (std::max)(0.0035f, 0.055f * peak);
        const auto strength = (std::min)(1.0f, (std::max)(0.0f, (rms - noise) / (peak - noise)));
        const auto target = 54.0f * std::sqrt(strength);
        const float timeConstant = target > opening ? 23.0f : 64.0f;
        opening += (target - opening) * (1.0f - std::exp(-static_cast<float>(elapsed) / timeConstant));
        return static_cast<unsigned>(opening + 0.5f);
    }
};

using EventStart = int (WINAPI*)(void*);
using EventStop = int (WINAPI*)(void*, bool);
using EventInfo = int (WINAPI*)(void*, int*, char**, void*);
using GetHandle = int (WINAPI*)(void*, void**);
using Member = int (WINAPI*)(void*);
using ChannelPlay = int (WINAPI*)(void*, int, void*, bool, void**);
using SoundName = int (WINAPI*)(void*, char*, int);
using GroupChannel = int (WINAPI*)(void*, int, void**);
using ChannelFlag = int (WINAPI*)(void*, int*);
using Wave = int (WINAPI*)(void*, float*, int, int);

EventStart originalStart = nullptr;
EventStop originalStop = nullptr;
Member originalUpdate = nullptr;
Member originalUnload = nullptr;
Member originalRelease = nullptr;
ChannelPlay originalPlay = nullptr;
Member originalSoundRelease = nullptr;
EventInfo eventInfo = nullptr;
GetHandle eventGroup = nullptr;
GetHandle currentSound = nullptr;
SoundName soundName = nullptr;
GroupChannel groupChannel = nullptr;
ChannelFlag groupChannelCount = nullptr;
ChannelFlag isPlaying = nullptr;
ChannelFlag getPaused = nullptr;
Wave getWave = nullptr;

struct Voice {
    void* event = nullptr;
    void* channel = nullptr;
    void* sound = nullptr;
    DWORD start = 0;
    DWORD ended = 0;
    Envelope envelope{};
};

SRWLOCK lock = SRWLOCK_INIT;
Voice voices[slotCount]{};
volatile LONG openings[slotCount]{};
DWORD previousUpdate = 0;
bool actorBindingAvailable = false;

struct Cue {
    BYTE* actor = nullptr;
    DWORD at = 0;
};
Cue cues[12]{};
PVOID volatile boundActor = nullptr;
bool otherBindingAmbiguous = false;

void* readBoundActor() {
    return InterlockedCompareExchangePointer(&boundActor, nullptr, nullptr);
}

void setBoundActor(void* actor) {
    InterlockedExchangePointer(&boundActor, actor);
}

void recordMouthCue(BYTE* actor, bool enabled, DWORD now) {
    AcquireSRWLockExclusive(&lock);
    Cue* free = nullptr;
    for (auto& cue : cues) {
        const auto age = now - cue.at;
        if (cue.actor == actor) {
            if (enabled) { cue.at = now; }
            else {
                cue = {};
                if (readBoundActor() == actor) setBoundActor(nullptr);
            }
            ReleaseSRWLockExclusive(&lock);
            return;
        }
        if (!free && (!cue.actor || age > 300)) free = &cue;
    }
    if (enabled && free) *free = {actor, now};
    if (!enabled && readBoundActor() == actor) setBoundActor(nullptr);
    ReleaseSRWLockExclusive(&lock);
}

void* uniqueMouthCue(DWORD started, DWORD now) {
    void* match = nullptr;
    for (const auto& cue : cues) {
        if (!cue.actor || now - cue.at > 260) continue;
        const auto delta = static_cast<LONG>(cue.at - started);
        if (delta < -170 || delta > 170) continue;
        if (match && match != cue.actor) return nullptr;
        match = cue.actor;
    }
    return match;
}

void startVoice(int who, void* event, void* channel, void* sound, DWORD now) {
    if (who == otherSpeaker) {
        otherBindingAmbiguous =
            (voices[who].event || voices[who].channel) && !voices[who].ended &&
            (voices[who].event != event || voices[who].channel != channel);
        setBoundActor(nullptr);
    }
    voices[who] = Voice{event, channel, sound, now};
    InterlockedExchange(&openings[who], 1);
}

void publish(int who, int opening) {
    InterlockedExchange(&openings[who], opening + 1);
}

int readOpening(int who) {
    return InterlockedCompareExchange(&openings[who], 0, 0) - 1;
}

void reset(int who) {
    if (who == otherSpeaker) {
        setBoundActor(nullptr);
        otherBindingAmbiguous = false;
    }
    voices[who] = Voice{};
    publish(who, -1);
}

int WINAPI start(void* event) {
    char* name = nullptr;
    const int who = eventInfo(event, nullptr, &name, nullptr) == 0 ? speaker(name) : 0;
    const int result = originalStart(event);
    if (result == 0 && who && (who != otherSpeaker || actorBindingAvailable)) {
        AcquireSRWLockExclusive(&lock);
        startVoice(who, event, nullptr, nullptr, GetTickCount());
        ReleaseSRWLockExclusive(&lock);
    }
    return result;
}

int WINAPI play(void* system, int index, void* sound, bool paused, void** destination) {
    void* channel = nullptr;
    auto* out = destination ? destination : &channel;
    const int result = originalPlay(system, index, sound, paused, out);
    if (result == 0 && *out && sound) {
        char name[96]{};
        const int who = soundName(sound, name, sizeof name) == 0 ? speaker(name) : 0;
        if (who && (who != otherSpeaker || actorBindingAvailable)) {
            AcquireSRWLockExclusive(&lock);
            startVoice(who, nullptr, *out, sound, GetTickCount());
            ReleaseSRWLockExclusive(&lock);
        }
    }
    return result;
}

int WINAPI stop(void* event, bool immediate) {
    AcquireSRWLockExclusive(&lock);
    for (int who = 1; who < slotCount; ++who) {
        if (voices[who].event == event) {
            voices[who].channel = nullptr;
            voices[who].event = nullptr;
            voices[who].ended = GetTickCount();
            publish(who, 0);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return originalStop(event, immediate);
}

void clearVoices() {
    AcquireSRWLockExclusive(&lock);
    for (int who = 1; who < slotCount; ++who) reset(who);
    for (auto& cue : cues) cue = {};
    ReleaseSRWLockExclusive(&lock);
}
int WINAPI unload(void* system) {
    clearVoices();
    return originalUnload(system);
}
int WINAPI release(void* system) {
    clearVoices();
    return originalRelease(system);
}
int WINAPI releaseSound(void* sound) {
    AcquireSRWLockExclusive(&lock);
    for (int who = 1; who < slotCount; ++who)
        if (voices[who].sound == sound) reset(who);
    ReleaseSRWLockExclusive(&lock);
    return originalSoundRelease(sound);
}

void sampleVoice(int who, DWORD now, DWORD elapsed) {
    auto& voice = voices[who];
    if (voice.ended) {
        if (now - voice.ended >= 200) reset(who);
        return;
    }
    if (!voice.channel && voice.event) {
        void* group = nullptr;
        void* channel = nullptr;
        void* sound = nullptr;
        int count = 0;
        if (eventGroup(voice.event, &group) == 0 && group &&
            groupChannelCount(group, &count) == 0) {
            for (int i = 0; i < (std::min)(count, 8); ++i) {
                if (groupChannel(group, i, &channel) != 0 || !channel ||
                    currentSound(channel, &sound) != 0 || !sound) continue;
                char name[96]{};
                const bool hasName = soundName(sound, name, sizeof name) == 0 &&
                    name[0] && std::strcmp(name, "(null)") != 0;
                // Bank-backed FEV events can expose an unnamed sample. A single channel
                // in an already identified voice event's own group is unambiguous.
                if ((hasName && speaker(name) == who) || (!hasName && count == 1)) {
                    voice.channel = channel;
                    voice.sound = sound;
                    break;
                }
            }
        }
        if (!voice.channel && now - voice.start >= 300) reset(who);
    }
    if (!voice.channel) return;
    if (who == otherSpeaker && !otherBindingAmbiguous && now - voice.start <= 170)
        setBoundActor(uniqueMouthCue(voice.start, now));

    void* sound = nullptr;
    int playing = 0, paused = 0;
    if (currentSound(voice.channel, &sound) != 0 || sound != voice.sound ||
        isPlaying(voice.channel, &playing) != 0 || !playing ||
        getPaused(voice.channel, &paused) != 0) {
        voice.channel = nullptr;
        voice.ended = now;
        publish(who, 0);
        return;
    }

    float frame[256]{};
    if (!paused && getWave(voice.channel, frame, 256, 0) != 0) {
        reset(who);
        return;
    }
    float power = 0;
    for (float sample : frame) power += sample * sample;
    if (std::isfinite(power))
        publish(who, static_cast<int>(voice.envelope.sample(std::sqrt(power / 256.0f), elapsed)));
}

int WINAPI update(void* system) {
    const int result = originalUpdate(system);
    AcquireSRWLockExclusive(&lock);
    const DWORD now = GetTickCount();
    const DWORD elapsed = now - previousUpdate;
    if (elapsed >= 8) {
        previousUpdate = now;
        for (int who = 1; who < slotCount; ++who)
            sampleVoice(who, now, (std::min)(elapsed, DWORD{50}));
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}

template<typename T>
T getExport(HMODULE dll, const char* name) {
    const auto address = GetProcAddress(dll, name);
    T value{};
    static_assert(sizeof address == sizeof value, "Function pointer size");
    std::memcpy(&value, &address, sizeof value);
    return value;
}

template<typename T>
bool hookImport(BYTE* base, const IMAGE_NT_HEADERS32* nt, const char* library,
                const char* symbol, T replacement, T& original, DWORD** changed) {
    const DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + rva);
    for (auto* item = imports; item->Name; ++item) {
        if (lstrcmpiA(reinterpret_cast<const char*>(base + item->Name), library) ||
            !item->OriginalFirstThunk) continue;
        auto* lookup = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + item->OriginalFirstThunk);
        auto* live = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + item->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++live) {
            if (IMAGE_SNAP_BY_ORDINAL32(lookup->u1.Ordinal)) continue;
            auto* record = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + lookup->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(record->Name), symbol)) continue;
            auto* cell = &live->u1.Function;
            const auto expected = getExport<T>(GetModuleHandleA(library), symbol);
            if (!expected || reinterpret_cast<DWORD>(expected) != *cell) return false;
            DWORD previous = 0;
            if (!VirtualProtect(cell, sizeof(*cell), PAGE_READWRITE, &previous)) return false;
            original = expected;
            *changed = cell;
            *cell = reinterpret_cast<DWORD>(replacement);
            DWORD ignored = 0;
            VirtualProtect(cell, sizeof(*cell), previous, &ignored);
            return true;
        }
    }
    return false;
}

bool install(BYTE* base, const IMAGE_NT_HEADERS32* nt) {
    const auto core = GetModuleHandleA("fmodex.dll");
    const auto event = GetModuleHandleA("fmod_event.dll");
    if (!core || !event) return false;
    eventInfo = getExport<EventInfo>(event, "FMOD_Event_GetInfo");
    eventGroup = getExport<GetHandle>(event, "FMOD_Event_GetChannelGroup");
    currentSound = getExport<GetHandle>(core, "FMOD_Channel_GetCurrentSound");
    soundName = getExport<SoundName>(core, "FMOD_Sound_GetName");
    groupChannel = getExport<GroupChannel>(core, "FMOD_ChannelGroup_GetChannel");
    groupChannelCount = getExport<ChannelFlag>(core, "FMOD_ChannelGroup_GetNumChannels");
    isPlaying = getExport<ChannelFlag>(core, "FMOD_Channel_IsPlaying");
    getPaused = getExport<ChannelFlag>(core, "FMOD_Channel_GetPaused");
    getWave = getExport<Wave>(core, "FMOD_Channel_GetWaveData");
    if (!eventInfo || !eventGroup || !currentSound || !soundName || !groupChannel || !groupChannelCount ||
        !isPlaying || !getPaused || !getWave) return false;

    DWORD* cells[7]{};
    const bool success =
        hookImport(base, nt, "fmod_event.dll", "?start@Event@FMOD@@QAG?AW4FMOD_RESULT@@XZ", start, originalStart, cells) &&
        hookImport(base, nt, "fmod_event.dll", "?stop@Event@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z", stop, originalStop, cells + 1) &&
        hookImport(base, nt, "fmod_event.dll", "?update@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@XZ", update, originalUpdate, cells + 2) &&
        hookImport(base, nt, "fmod_event.dll", "?unload@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@XZ", unload, originalUnload, cells + 3) &&
        hookImport(base, nt, "fmod_event.dll", "?release@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@XZ", release, originalRelease, cells + 4) &&
        hookImport(base, nt, "fmodex.dll", "?playSound@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PAVSound@2@_NPAPAVChannel@2@@Z",
                   play, originalPlay, cells + 5) &&
        hookImport(base, nt, "fmodex.dll", "?release@Sound@FMOD@@QAG?AW4FMOD_RESULT@@XZ", releaseSound, originalSoundRelease, cells + 6);
    if (!success) {
        const DWORD originals[] = {
            reinterpret_cast<DWORD>(originalStart), reinterpret_cast<DWORD>(originalStop),
            reinterpret_cast<DWORD>(originalUpdate), reinterpret_cast<DWORD>(originalUnload),
            reinterpret_cast<DWORD>(originalRelease), reinterpret_cast<DWORD>(originalPlay),
            reinterpret_cast<DWORD>(originalSoundRelease)
        };
        for (int i = 0; i < 7; ++i) {
            DWORD previous = 0;
            if (cells[i] && VirtualProtect(cells[i], sizeof(DWORD), PAGE_READWRITE, &previous)) {
                *cells[i] = originals[i];
                DWORD ignored = 0;
                VirtualProtect(cells[i], sizeof(DWORD), previous, &ignored);
            }
        }
    }
    return success;
}
}
