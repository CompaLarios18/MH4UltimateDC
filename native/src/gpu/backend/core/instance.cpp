#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include "gpu/backend/core/impl.h"
#include "mh4n/log.h"

namespace mh4n::gpu::rhi {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* handler = static_cast<DebugMessageHandler*>(user);
    if (handler != nullptr && *handler && data != nullptr && data->pMessage != nullptr) {
        (*handler)((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 ? DebugSeverity::Error
                                                                                   : DebugSeverity::Warning,
                   data->pMessage);
    }
    return VK_FALSE;
}

// The vendored backend reports a failed vkCreateInstance as "... error code 0x%X.": say what the code means.
std::string DescribeInstanceFailure(const std::string& failure) {
    const size_t at = failure.find("vkCreateInstance failed with error code 0x");
    if (at == std::string::npos) {
        return failure;
    }
    const long code = static_cast<s32>(
        std::strtoul(failure.c_str() + at + std::strlen("vkCreateInstance failed with error code 0x"), nullptr, 16));
    const char* meaning = nullptr;
    switch (code) {
    case -9:
        meaning = "no Vulkan driver found (VK_ERROR_INCOMPATIBLE_DRIVER): install the GPU driver or the Vulkan runtime";
        break;
    case -6: meaning = "the Vulkan loader could not initialise (VK_ERROR_INITIALIZATION_FAILED)"; break;
    case -7: meaning = "a requested layer is not present (VK_ERROR_LAYER_NOT_PRESENT)"; break;
    case -8: meaning = "a requested instance extension is not present (VK_ERROR_EXTENSION_NOT_PRESENT)"; break;
    case -1: meaning = "out of host memory (VK_ERROR_OUT_OF_HOST_MEMORY)"; break;
    case -2: meaning = "out of device memory (VK_ERROR_OUT_OF_DEVICE_MEMORY)"; break;
    default: break;
    }
    return meaning != nullptr ? fmt::format("{}: {}", meaning, failure) : failure;
}

} // namespace

Instance::Impl::~Impl() {
    if (messenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr && interface != nullptr) {
        vkDestroyDebugUtilsMessengerEXT(vulkan()->instance, messenger, nullptr);
    }
    interface.reset();
}

Instance::Instance(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Instance::~Instance() = default;

const std::vector<std::string>& Instance::EnabledExtensions() const {
    return impl_->enabled_extensions;
}

Result<std::shared_ptr<Instance>> CreateInstance(const InstanceOptions& options) {
    ::plume::VulkanInterfaceOptions plume_options;
    plume_options.extraInstanceExtensions = options.extensions;
    plume_options.headlessSurface = options.headless_surface;
    plume_options.debugUtils = static_cast<bool>(options.debug_messages);
    plume_options.validationLayer = options.validation_layer;
    std::string failure;
    plume_options.error = &failure;

    std::unique_ptr<::plume::RenderInterface> interface = ::plume::CreateVulkanInterface(plume_options);
    if (interface == nullptr) {
        return Error{fmt::format("cannot make a Vulkan instance: {}",
                                 failure.empty() ? std::string("unknown failure") : DescribeInstanceFailure(failure))};
    }

    auto impl = std::make_unique<Instance::Impl>();
    impl->interface = std::move(interface);
    impl->enabled_extensions = impl->vulkan()->enabledExtensionNames;
    std::sort(impl->enabled_extensions.begin(), impl->enabled_extensions.end());

    const bool has_debug_utils =
        std::find(impl->enabled_extensions.begin(), impl->enabled_extensions.end(),
                  std::string(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) != impl->enabled_extensions.end();
    if (options.debug_messages && has_debug_utils && vkCreateDebugUtilsMessengerEXT != nullptr) {
        impl->debug_handler = std::make_unique<DebugMessageHandler>(options.debug_messages);
        VkDebugUtilsMessengerCreateInfoEXT info = {};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = DebugCallback;
        info.pUserData = impl->debug_handler.get();
        if (vkCreateDebugUtilsMessengerEXT(impl->vulkan()->instance, &info, nullptr, &impl->messenger) != VK_SUCCESS) {
            MH4N_LOG(Warning, "RHI", "rhi: the debug messenger could not be made");
            impl->messenger = VK_NULL_HANDLE;
        }
    }

    MH4N_LOG(Info, "RHI", "rhi: instance made; extensions: {}", core::JoinNames(impl->enabled_extensions));
    return std::make_shared<Instance>(std::move(impl));
}

} // namespace mh4n::gpu::rhi
