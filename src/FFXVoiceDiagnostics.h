#pragma once

#include <windows.h>
#include <cwchar>

namespace VoiceDiagnostics {
inline bool enabled()
{
    constexpr wchar_t settings[] = L"modules\\config\\FFXLipSync.ini";
    wchar_t path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return true;
    auto* filename = std::wcsrchr(path, L'\\');
    if (!filename) return true;
    ++filename;
    const auto available = static_cast<size_t>(path + MAX_PATH - filename);
    if (wcscpy_s(filename, available, settings) != 0) return true;
    return GetPrivateProfileIntW(L"Diagnostics", L"Logging", 1, path) != 0;
}
}
