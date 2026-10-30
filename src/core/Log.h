#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ex {

inline void LogDebugOutput(const char* line) {
#ifdef _WIN32
    OutputDebugStringA(line);
#else
    (void)line;
#endif
}

// Minimal logging facility: writes to the debugger output (DebugView / VS)
// and to stderr, which is attached to the parent console when available.
inline void LogMessage(const char* level, const char* format, ...) {
    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char line[2200];
    snprintf(line, sizeof(line), "[%s] %s\n", level, message);
    LogDebugOutput(line);
    fputs(line, stderr);
    fflush(stderr);
}

inline void LogInfo(const char* format, ...) {
    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char line[2200];
    snprintf(line, sizeof(line), "[info] %s\n", message);
    LogDebugOutput(line);
    fputs(line, stderr);
    fflush(stderr);
}

inline void LogWarn(const char* format, ...) {
    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char line[2200];
    snprintf(line, sizeof(line), "[warn] %s\n", message);
    LogDebugOutput(line);
    fputs(line, stderr);
    fflush(stderr);
}

inline void LogError(const char* format, ...) {
    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char line[2200];
    snprintf(line, sizeof(line), "[error] %s\n", message);
    LogDebugOutput(line);
    fputs(line, stderr);
    fflush(stderr);
}

} // namespace ex
