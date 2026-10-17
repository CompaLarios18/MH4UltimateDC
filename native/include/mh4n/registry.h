// Self-registration (N1-01). A module adds a value to the registry of its type from a static
// object in one of its own files, so no shared list needs editing:
//
//     MH4N_REGISTER(gpu::GpuFactory, "null", [](Host&, const Options&) { ... });
//
// Every module is linked whole (see native/CMakeLists.txt), so the static object always runs.
// Registration happens during static initialisation, on one thread; lookups afterwards are
// read-only and need no lock. The type must not contain a comma (use an alias).
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>

namespace mh4n {

namespace detail {
// Logs the duplicate at Critical and aborts.
[[noreturn]] void DuplicateRegistration(const char* type, std::string_view name);
} // namespace detail

template <class T>
class Registry {
public:
    using Map = std::map<std::string, T, std::less<>>;

    // The registry of T. (The constructor is public so a test can use a private one.)
    static Registry& Instance() {
        static Registry instance;
        return instance;
    }

    // False, and nothing added, if `name` is taken.
    bool TryAdd(std::string name, T value) {
        if (map_.find(name) != map_.end()) {
            return false;
        }
        map_.emplace(std::move(name), std::move(value));
        return true;
    }

    // A duplicate name is an error at start-up: it is logged and the program aborts.
    void Add(std::string name, T value) {
        if (map_.find(name) != map_.end()) {
            detail::DuplicateRegistration(typeid(T).name(), name);
        }
        map_.emplace(std::move(name), std::move(value));
    }

    const T* Find(std::string_view name) const {
        auto it = map_.find(name);
        return it == map_.end() ? nullptr : &it->second;
    }

    // Every entry, in name order.
    const Map& All() const {
        return map_;
    }

private:
    Map map_;
};

namespace detail {
template <class T>
struct Registrar {
    Registrar(std::string_view name, T value) {
        Registry<T>::Instance().Add(std::string(name), std::move(value));
    }
};
} // namespace detail

} // namespace mh4n

#define MH4N_CONCAT_IMPL(a, b) a##b
#define MH4N_CONCAT(a, b) MH4N_CONCAT_IMPL(a, b)
#define MH4N_UNIQUE(prefix) MH4N_CONCAT(prefix, __COUNTER__)

#define MH4N_REGISTER(T, name, ...)                                                                \
    [[maybe_unused]] static const ::mh4n::detail::Registrar<T> MH4N_UNIQUE(mh4n_registrar_) {      \
        name, __VA_ARGS__                                                                          \
    }
