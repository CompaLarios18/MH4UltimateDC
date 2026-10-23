// What the report module's files share. Not a public header: report.h is.
#pragma once

#include <string>

namespace mh4n::app::report {

/// True when `text` holds an absolute path (a drive or UNC path, or a POSIX one of two or more
/// components), by the rule Redact uses.
bool ContainsAbsolutePath(const std::string& text);

}  // namespace mh4n::app::report
