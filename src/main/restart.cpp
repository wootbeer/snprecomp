// Restart (issue 22): a button in the menu's header between Quit and Close,
// and a choice in the quit prompt (lib-patches/RecompFrontend/0023). It quits
// as Quit does, and just before the process ends (main's end or the quit
// watchdog, src/main/main.cpp) starts the game again with the same command
// line and working folder. Handy after changing mods. Not on Android, where
// the app is started by the system.
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#elif !defined(__ANDROID__)
#include <spawn.h>
extern char** environ;
#endif

#include "recompui/recompui.h"
#include "ultramodern/ultramodern.hpp"

#include "snp_restart.h"

namespace {
    std::atomic_bool requested = false;
    std::atomic_bool relaunched = false;
#if !defined(_WIN32) && !defined(__ANDROID__)
    std::vector<std::string> arguments;
#endif
}

void snp::restart::install(int argc, char** argv) {
#if defined(__ANDROID__)
    (void)argc;
    (void)argv;
#else
#if !defined(_WIN32)
    arguments.assign(argv, argv + argc);
#else
    (void)argc;
    (void)argv;
#endif
    recompui::set_restart_callback([]() {
        requested = true;
        ultramodern::quit();
    });
#endif
}

void snp::restart::relaunch_if_requested() {
    if (!requested.load() || relaunched.exchange(true)) {
        return;
    }
#if defined(_WIN32)
    wchar_t exe[MAX_PATH * 4];
    DWORD length = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    if (length == 0 || length >= std::size(exe)) {
        std::fprintf(stderr, "Restart: couldn't find the game's exe\n");
        return;
    }
    // CreateProcessW may write to the command line, so a copy.
    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(exe, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    else {
        std::fprintf(stderr, "Restart: couldn't start the game again (error %lu)\n", GetLastError());
    }
#elif !defined(__ANDROID__)
    std::vector<char*> argv;
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t pid;
    int error = posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, argv.data(), environ);
    if (error != 0) {
        std::fprintf(stderr, "Restart: couldn't start the game again (error %d)\n", error);
    }
#endif
}
