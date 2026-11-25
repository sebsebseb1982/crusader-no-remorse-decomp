#include "platform/process.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <string>
#include <vector>

namespace crusader {

#ifdef _WIN32

// @orig none: port's self-update
std::filesystem::path current_executable() {
    std::wstring path(512, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        if (path.size() >= 32768) return {};
        path.resize(path.size() * 2);
    }
}

// @orig none: port's self-update
bool relaunch(const std::filesystem::path& exe, int, char**) {
    // The command line as it was typed (wide characters kept), run by the file.
    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info{};
    const std::wstring file = exe.wstring();
    if (!CreateProcessW(file.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

#else

// @orig none: port's self-update
std::filesystem::path current_executable() {
    std::vector<char> path(4096);
    const ssize_t n = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (n <= 0) return {};
    return std::filesystem::path(std::string(path.data(), static_cast<std::size_t>(n)));
}

// @orig none: port's self-update
bool relaunch(const std::filesystem::path& exe, int argc, char** argv) {
    const std::string file = exe.string();
    std::vector<char*> args;
    args.push_back(const_cast<char*>(file.c_str()));
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    args.push_back(nullptr);
    execv(file.c_str(), args.data());
    return false;  // only reached when it failed
}

#endif

}  // namespace crusader
