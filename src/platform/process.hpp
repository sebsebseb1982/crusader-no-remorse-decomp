#pragma once
// Port addition (@orig none): the running program's file, and starting it again
// (the end of the self-update, core/self_update.hpp).

#include <filesystem>

namespace crusader {

// The file the running program was started from, empty when the system does
// not tell. Read it at start-up: once the file is renamed (an update), Linux
// tells the new name.
std::filesystem::path current_executable();

// Starts `exe` again with the arguments of this run, as they were typed.
// Windows: a new process (the caller then ends). Elsewhere: this process
// becomes it (execv), and the call returns only when that failed. False on
// failure.
bool relaunch(const std::filesystem::path& exe, int argc, char** argv);

}  // namespace crusader
