// The test helper for every test that needs the GPU backend (N2-26; the other WPs include it).
//
//     auto backend = mh4n::test::RequireBackend();
//     if (!backend) return;   // no Vulkan device: the case has been skipped with a message
//
// Without a Vulkan loader or a device RequireBackend skips the test with a message; under
// MH4N_REQUIRE_VULKAN=1 (set by CI's Linux job) it fails the test instead. It logs the device's name.
// When the validation layer is enabled (VK_INSTANCE_LAYERS names it, or MH4N_RHI_VALIDATION=1) it
// installs a debug messenger that fails the running test on any warning or error message.
//
// Include this header from one translation unit per test binary, after <doctest/doctest.h>'s users.
#pragma once

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "gpu/backend/rhi.h"
#include "gpu/backend/vulkan_access.h"

namespace mh4n::test {

namespace rhi = ::mh4n::gpu::rhi;

inline bool VulkanRequired() {
    const char* value = std::getenv("MH4N_REQUIRE_VULKAN");
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
    WARN_MESSAGE(false, "skipped: " << why);
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

// nullptr after SkipOrFail where the loader lacks VK_EXT_headless_surface.
inline std::unique_ptr<HeadlessSurface> MakeHeadlessSurface(std::shared_ptr<rhi::Instance> instance) {
    bool has = false;
    for (const std::string& name : instance->EnabledExtensions()) {
        has = has || name == VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME;
    }
    if (!has || vkCreateHeadlessSurfaceEXT == nullptr) {
        SkipOrFail("the Vulkan loader lacks VK_EXT_headless_surface (or the instance was made without InstanceOptions::headless_surface)");
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
