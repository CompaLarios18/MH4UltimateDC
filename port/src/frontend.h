// Hook for frontends (Azahar's Qt UI, mh4u_runner): pick the CPU backend for a game.
#pragma once

#include <string>

namespace Core {
class System;
}

namespace Port {

/// Call before Core::System::Load(). If a recompiled code library matches the program in
/// `rom_path`, installs the recompiled CPU backend; otherwise leaves the emulator's own
/// backend in place. Code libraries are looked up in this order:
///   1. the MH4U_CODE environment variable (path to the library),
///   2. "<program id>.<so|dll|dylib>" in Azahar's user directory, under "recomp/".
/// Returns a line for the log describing what was chosen.
std::string PrepareBackendFor(Core::System& system, const std::string& rom_path);

} // namespace Port
