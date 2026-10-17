#include "mh4n/registry.h"

#include "mh4n/assert.h"

namespace mh4n::detail {

void DuplicateRegistration(const char* type, std::string_view name) {
    AssertFail(__FILE__, __LINE__, "duplicate registration",
               fmt::format("'{}' is registered twice for {}", name, type));
}

} // namespace mh4n::detail
