#pragma once
#include <cstddef>
#include <filesystem>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace vr {

#ifdef _WIN32
// std::filesystem::path(const std::string&) on Windows interprets the
// bytes using the current ANSI codepage (CP_ACP), NOT UTF-8 -- so any
// std::string holding UTF-8 text (e.g. Cyrillic player names decoded from
// a GoldSrc demo, which are always plain UTF-8/ASCII bytes on the wire)
// gets mangled into mojibake or '?' the moment it's turned into a path,
// regardless of what encoding is later used to open the file. The fix is
// to convert UTF-8 -> UTF-16 explicitly and build the path from that wide
// string, which is always interpreted correctly.
inline std::filesystem::path PathFromUtf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    return std::filesystem::path(wide);
}
#else
inline std::filesystem::path PathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(utf8); // POSIX paths are just bytes; UTF-8 passes through untouched
}
#endif

// Returns the UTF-8-encoded directory the running executable lives in
// (not the current working directory, which can be anything depending on
// how the user launched/drag-dropped onto the exe). Used so output always
// gets written next to voiceripper.exe itself regardless of where the
// input demo/zip/folder was.
inline std::string GetExecutableDirUtf8() {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) return {};
    std::filesystem::path exePath(std::wstring(buf, len));
    std::filesystem::path dir = exePath.parent_path();

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return {};
    std::string out(static_cast<size_t>(utf8Len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, out.data(), utf8Len, nullptr, nullptr);
    return out;
#else
    return std::filesystem::current_path().string();
#endif
}

} // namespace vr
