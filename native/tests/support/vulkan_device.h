// The test helper for every test that needs the GPU backend (N2-26; the other WPs include it).
//
//     auto backend = mh4n::test::RequireBackend();
//     if (!backend) return;   // no Vulkan device: the case has been skipped with a message
//
// Without a Vulkan loader or a device RequireBackend skips the test with a message; under
// MH4N_REQUIRE_VULKAN=1 (set by CI's Linux job) it fails the test instead: that variable means only "a Vulkan device must
// exist". A case that needs VK_EXT_headless_surface skips with a message where the loader lacks it (a real GPU's driver stack
// may not list it) and fails for that reason only under MH4N_REQUIRE_HEADLESS_SURFACE=1 (CI's Linux job, where the extension is
// the lavapipe stack's and its absence means the setup broke). It logs the device's name.
//
// A case that has nothing to assert on this device (no device, no headless surface, no D24S8 attachment, ...) cannot be marked skipped
// at run time: doctest 2.4.11 has no such call, and its own summary counts the case as passed. So every run-time skip goes through
// SkipCase / SkipOrFail / SkipOrFailHeadless, which records {test case, why}, and a doctest listener registered by this header prints,
// at the end of the run, "skipped at run time: N" and one line "  <test case>: <why>" per entry (N = 0 prints one line), and a line
// "skipped by decorator: <test case>" for every case registered with doctest::skip (doctest also calls the listener for a decorated case that a --test-case filter excluded, so such a case is printed too). Read that block to know what ran: doctest's own
// summary still counts a run-time skip as passed.
//
// The three plume test binaries include this header too (for the listener and SkipCase).
// When the validation layer is enabled (VK_INSTANCE_LAYERS names it, or MH4N_RHI_VALIDATION=1) it
// installs a debug messenger that fails the running test on any warning or error message.
//
// Include this header from one translation unit per test binary, after <doctest/doctest.h>'s users.
#pragma once

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gpu/backend/rhi.h"
#include "gpu/backend/vulkan_access.h"

namespace mh4n::test {

namespace rhi = ::mh4n::gpu::rhi;

struct RunTimeSkip {
    std::string test_case;
    std::string why;
};

// The state is one inline object, guarded by a mutex (a case may skip from a thread).
struct SkipRegistry {
    std::mutex mutex;
    std::string current_case = "(outside a test case)";
    std::vector<RunTimeSkip> skips;
};

inline SkipRegistry& Skips() {
    static SkipRegistry registry;
    return registry;
}

// Records a run-time skip of the running case. Does not return from the case: the caller returns.
inline void RecordSkip(const std::string& why) {
    SkipRegistry& registry = Skips();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.skips.push_back({registry.current_case, why});
}

// The case has nothing to assert on this device: say so (a message, and the line in the end-of-run block) and let the caller return.
inline void SkipCase(const std::string& why) {
    WARN_MESSAGE(false, "skipped: " << why);
    RecordSkip(why);
}

// A listener, not a reporter: it runs beside the reporter in use, so doctest's own output and counts are unchanged.
class SkipListener : public doctest::IReporter {
public:
    explicit SkipListener(const doctest::ContextOptions&) {}

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {
        SkipRegistry& registry = Skips();
        std::lock_guard<std::mutex> lock(registry.mutex);
        std::printf("skipped at run time: %zu\n", registry.skips.size());
        for (const RunTimeSkip& skip : registry.skips) {
            std::printf("  %s: %s\n", skip.test_case.c_str(), skip.why.c_str());
        }
        std::fflush(stdout);
    }
    void test_case_start(const doctest::TestCaseData& data) override {
        SkipRegistry& registry = Skips();
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.current_case = data.m_name;
    }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData& data) override {
        if (data.m_skip) {  // a decorated case; also printed when a --test-case filter excluded it
            std::printf("skipped by decorator: %s\n", data.m_name);
            std::fflush(stdout);
        }
    }
};

REGISTER_LISTENER("mh4n_skips", 1, SkipListener);

inline bool VulkanRequired() {
    const char* value = std::getenv("MH4N_REQUIRE_VULKAN");
    return value != nullptr && std::string(value) == "1";
}

inline bool HeadlessSurfaceRequired() {
    const char* value = std::getenv("MH4N_REQUIRE_HEADLESS_SURFACE");
    return value != nullptr && std::string(value) == "1";
}

inline bool ValidationEnabled() {
    const char* layers = std::getenv("VK_INSTANCE_LAYERS");
    const char* force = std::getenv("MH4N_RHI_VALIDATION");
    return (layers != nullptr && std::string(layers).find("validation") != std::string::npos) || (force != nullptr && std::string(force) == "1");
}

// Skips the running case with a message, or fails it under MH4N_REQUIRE_VULKAN=1. Returns normally only
// when skipping; the caller then returns.
inline void SkipOrFail(const std::string& why) {
    if (VulkanRequired()) {
        FAIL("MH4N_REQUIRE_VULKAN=1 and there is no usable Vulkan device: " << why);
    }
    SkipCase(why);
}

// Skips the running case with a message because the loader lacks VK_EXT_headless_surface, or fails it under
// MH4N_REQUIRE_HEADLESS_SURFACE=1 (and only under that variable). Returns normally only when skipping; the caller then returns.
inline void SkipOrFailHeadless(const std::string& why) {
    if (HeadlessSurfaceRequired()) {
        FAIL("MH4N_REQUIRE_HEADLESS_SURFACE=1 and VK_EXT_headless_surface is missing: " << why);
    }
    SkipCase(why);
}

// An instance with the debug messenger of the helper (see above) and the options given. nullptr after
// SkipOrFail when the instance cannot be made.
inline std::shared_ptr<rhi::Instance> RequireInstance(rhi::InstanceOptions options = {}) {
    if (ValidationEnabled() && !options.debug_messages) {
        options.debug_messages = [](rhi::DebugSeverity severity, std::string_view message) {
            FAIL_CHECK("validation " << (severity == rhi::DebugSeverity::Error ? "error" : "warning") << ": " << std::string(message));
        };
    }
    rhi::Result<std::shared_ptr<rhi::Instance>> instance = rhi::CreateInstance(options);
    if (!instance.ok()) {
        SkipOrFail(instance.error());
        return nullptr;
    }
    return *instance;
}

// A backend on `instance` (made with RequireInstance when null). nullptr after SkipOrFail.
inline std::shared_ptr<rhi::Backend> RequireBackend(const rhi::BackendOptions& options = {}, std::shared_ptr<rhi::Instance> instance = nullptr) {
    if (instance == nullptr) {
        instance = RequireInstance();
        if (instance == nullptr) {
            return nullptr;
        }
    }
    rhi::Result<std::shared_ptr<rhi::Backend>> backend = rhi::CreateBackend(instance, options);
    if (!backend.ok()) {
        SkipOrFail(backend.error());
        return nullptr;
    }
    const rhi::Capabilities& caps = (*backend)->Caps();
    std::printf("device: %s (%s)\n", caps.device_name.c_str(), caps.driver_name.c_str());
    std::fflush(stdout);
    return *backend;
}

// A headless surface (VK_EXT_headless_surface) on an instance made with InstanceOptions::headless_surface.
// Destroy it after the backend that uses it (declare it before the backend).
struct HeadlessSurface {
    std::shared_ptr<rhi::Instance> instance;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    HeadlessSurface() = default;
    HeadlessSurface(const HeadlessSurface&) = delete;
    HeadlessSurface& operator=(const HeadlessSurface&) = delete;
    ~HeadlessSurface() {
        if (surface != VK_NULL_HANDLE && vkDestroySurfaceKHR != nullptr) {
            vkDestroySurfaceKHR(rhi::NativeInstance(*instance), surface, nullptr);
        }
    }
    rhi::SurfaceHandle Handle() const { return rhi::SurfaceFrom(surface); }
};

// nullptr after SkipOrFailHeadless where the loader lacks VK_EXT_headless_surface (after SkipOrFail if the surface cannot be made).
inline std::unique_ptr<HeadlessSurface> MakeHeadlessSurface(std::shared_ptr<rhi::Instance> instance) {
    bool has = false;
    for (const std::string& name : instance->EnabledExtensions()) {
        has = has || name == VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME;
    }
    if (!has || vkCreateHeadlessSurfaceEXT == nullptr) {
        SkipOrFailHeadless("the Vulkan loader lacks VK_EXT_headless_surface (or the instance was made without InstanceOptions::headless_surface)");
        return nullptr;
    }
    auto result = std::make_unique<HeadlessSurface>();
    result->instance = std::move(instance);
    VkHeadlessSurfaceCreateInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    if (vkCreateHeadlessSurfaceEXT(rhi::NativeInstance(*result->instance), &info, nullptr, &result->surface) != VK_SUCCESS) {
        SkipOrFail("vkCreateHeadlessSurfaceEXT failed");
        return nullptr;
    }
    return result;
}

}  // namespace mh4n::test
