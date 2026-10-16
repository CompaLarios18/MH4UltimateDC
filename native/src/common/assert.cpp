#include "mh4n/assert.h"

#include <cstdlib>

#include "mh4n/log.h"

namespace mh4n::detail {

void AssertFail(const char* file, int line, const char* what, std::string message) {
    std::string text = what;
    if (!message.empty()) {
        text += ": ";
        text += message;
    }
    // Critical bypasses the filter on purpose: an assertion must always be seen.
    LogWrite(LogLevel::Critical, "Assert", file, line, text);
    std::abort();
}

} // namespace mh4n::detail
