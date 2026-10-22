// The text of a new mh4u.ini: the port's recommended settings, each with what it does. Standard
// library only, so that every executable can write the file (frontend.cpp, the Azahar app's
// runtime, the native runtime's).
#pragma once

#include <string>

namespace Port {

/// What a new options file says.
std::string OptionsTemplateText();

} // namespace Port
