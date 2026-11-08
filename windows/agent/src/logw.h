#pragma once
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

// Ligne de journal à partir d'un format large, écrite en UTF-8 comme le reste du journal :
// mêler wprintf et printf sur la même sortie tronquerait les lignes.
inline void logw(const wchar_t* format, ...) {
    wchar_t wide[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(wide, _TRUNCATE, format, args);
    va_end(args);
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), n, nullptr, nullptr);
    std::fputs(utf8.c_str(), stdout);
    std::fflush(stdout);
}
