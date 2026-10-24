// The device: probing every physical device, choosing one by capability, enabling only what the
// capabilities use, applying the mask, and the Backend's read-only accessors (N2-26).
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <set>

#include <fmt/format.h>

#include "gpu/backend/core/impl.h"
#include "mh4n/log.h"

namespace mh4n::gpu::rhi {

namespace {

constexpr u32 MakeApi(u32 major, u32 minor) {
    return (major << 22) | (minor << 12);
}

// Everything the choice and the capabilities need to know about one physical device.
struct Probe {
    core::DeviceCandidate candidate;
    Capabilities caps;  // before the mask
    std::set<std::string> extensions;
    VkPhysicalDeviceFeatures features = {};
    VkPhysicalDeviceLimits limits = {};
    bool present_id = false;
    bool present_wait = false;
    bool can_present = false;
    u32 queue_family_count = 0;
};

core::DeviceType ToDeviceType(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return core::DeviceType::Discrete;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return core::DeviceType::Integrated;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return core::DeviceType::Virtual;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return core::DeviceType::Cpu;
    default: return core::DeviceType::Other;
    }
}

struct Chain {
    void* head = nullptr;
    template <typename T>
    void Add(T& structure, VkStructureType type) {
        structure.sType = type;
        structure.pNext = head;
        head = &structure;
    }
};

u32 CountBits(const Capabilities& caps, CapabilityMask mask) {
    u32 bits = 0;
    for (u32 i = 0; i < kCapCount; ++i) {
        const Cap cap = static_cast<Cap>(i);
        bits += (caps.Has(cap) && !mask.Has(cap)) ? 1 : 0;
    }
    return bits;
}

Probe ProbePhysicalDevice(VkPhysicalDevice device, u32 index, VkSurfaceKHR surface) {
    Probe probe;
    Capabilities& caps = probe.caps;
    caps.device_index = index;

    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(device, &props);
    probe.limits = props.limits;
    vkGetPhysicalDeviceFeatures(device, &probe.features);
    caps.device_name = props.deviceName;
    caps.api_version = props.apiVersion;
    caps.vendor_id = props.vendorID;
    caps.timestamp_period = props.limits.timestampPeriod;
    caps.max_texel_buffer_elements = props.limits.maxTexelBufferElements;
    caps.geometry_shader = probe.features.geometryShader != VK_FALSE;

    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> listed(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, listed.data());
    for (const VkExtensionProperties& extension : listed) {
        probe.extensions.insert(extension.extensionName);
    }
    const auto has = [&](const char* name) { return probe.extensions.count(name) != 0; };
    const u32 api = props.apiVersion;
    caps.portability_subset = has(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);

    // Properties: the driver's id and name, the portability subset's stride alignment.
    VkPhysicalDeviceDriverProperties driver = {};
    VkPhysicalDevicePortabilitySubsetPropertiesKHR subset_props = {};
    if (api >= MakeApi(1, 1)) {
        Chain chain;
        if (api >= MakeApi(1, 2) || has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
            chain.Add(driver, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
        }
        if (caps.portability_subset) {
            chain.Add(subset_props, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_PROPERTIES_KHR);
        }
        VkPhysicalDeviceProperties2 props2 = {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = chain.head;
        vkGetPhysicalDeviceProperties2(device, &props2);
    }
    caps.driver_id = static_cast<u32>(driver.driverID);
    caps.driver_name = driver.driverName;
    caps.stride_alignment = (caps.portability_subset && subset_props.minVertexInputBindingStrideAlignment > 0)
                                ? subset_props.minVertexInputBindingStrideAlignment
                                : 1;
    caps.apple = core::IsAppleDriver(caps.driver_id, caps.driver_name, caps.vendor_id);
    caps.primitive_restart_always_on = caps.apple;  // inference (MoltenVK always restarts; KosmicKrisp: unknown)

    // Features: only the structures the device can answer are chained.
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock = {};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT uint8 = {};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border = {};
    VkPhysicalDevicePortabilitySubsetFeaturesKHR subset = {};
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering = {};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT library = {};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = {};
    VkPhysicalDevicePresentIdFeaturesKHR present_id = {};
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait = {};
    const bool have_timeline = api >= MakeApi(1, 2) || has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    const bool have_dynamic_rendering = api >= MakeApi(1, 3) || has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    const bool have_present = has(VK_KHR_PRESENT_ID_EXTENSION_NAME) && has(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    if (api >= MakeApi(1, 1)) {
        Chain chain;
        if (have_timeline) chain.Add(timeline, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
        if (has(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME)) chain.Add(interlock, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT);
        if (has(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME)) chain.Add(uint8, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT);
        if (has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME)) chain.Add(border, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
        if (caps.portability_subset) chain.Add(subset, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR);
        if (have_dynamic_rendering) chain.Add(dynamic_rendering, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
        if (has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME)) chain.Add(library, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT);
        if (has(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)) chain.Add(eds3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
        if (have_present) {
            chain.Add(present_id, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
            chain.Add(present_wait, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
        }
        VkPhysicalDeviceFeatures2 features2 = {};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = chain.head;
        vkGetPhysicalDeviceFeatures2(device, &features2);
        probe.features = features2.features;
    }
    caps.timeline = timeline.timelineSemaphore != VK_FALSE;
    caps.interlock = interlock.fragmentShaderPixelInterlock != VK_FALSE;
    caps.index_uint8 = uint8.indexTypeUint8 != VK_FALSE;
    caps.custom_border_color = border.customBorderColors != VK_FALSE;
    // Fans are core on a full device; the portability subset says whether it has them.
    caps.triangle_fans = caps.portability_subset ? subset.triangleFans != VK_FALSE : true;
    caps.logic_op = probe.features.logicOp != VK_FALSE;
    caps.dynamic_rendering = dynamic_rendering.dynamicRendering != VK_FALSE;
    caps.pipeline_library = library.graphicsPipelineLibrary != VK_FALSE;
    caps.eds3_blend = eds3.extendedDynamicState3ColorBlendEnable && eds3.extendedDynamicState3ColorBlendEquation &&
                      eds3.extendedDynamicState3ColorWriteMask && eds3.extendedDynamicState3LogicOpEnable;
    probe.present_id = present_id.presentId != VK_FALSE;
    probe.present_wait = present_wait.presentWait != VK_FALSE;
    caps.present_wait = probe.present_id && probe.present_wait;

    // Formats.
    const auto format_has = [&](VkFormat format, VkFormatFeatureFlags needed) {
        VkFormatProperties format_props = {};
        vkGetPhysicalDeviceFormatProperties(device, format, &format_props);
        return (format_props.optimalTilingFeatures & needed) == needed;
    };
    caps.d24s8 = format_has(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    caps.bc7 = probe.features.textureCompressionBC != VK_FALSE && format_has(VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);

    // Memory: the host can write device memory directly when a host-visible, device-local type sits in
    // a heap as large as the biggest device-local heap (unified memory, resizable BAR).
    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    VkDeviceSize biggest_device_heap = 0;
    for (u32 i = 0; i < memory.memoryHeapCount; ++i) {
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            biggest_device_heap = std::max(biggest_device_heap, memory.memoryHeaps[i].size);
        }
    }
    constexpr VkMemoryPropertyFlags kDirect = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
        if ((memory.memoryTypes[i].propertyFlags & kDirect) == kDirect &&
            memory.memoryHeaps[memory.memoryTypes[i].heapIndex].size >= biggest_device_heap) {
            caps.direct_upload = true;
        }
    }

    // Queues: the family plume wants (graphics, compute and transfer), and whether one can present.
    vkGetPhysicalDeviceQueueFamilyProperties(device, &probe.queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(probe.queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &probe.queue_family_count, families.data());
    constexpr VkQueueFlags kWanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    bool graphics = false;
    probe.can_present = (surface == VK_NULL_HANDLE);
    for (u32 i = 0; i < families.size(); ++i) {
        if ((families[i].queueFlags & kWanted) != kWanted) {
            continue;
        }
        graphics = true;
        if (surface != VK_NULL_HANDLE) {
            VkBool32 supported = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supported) == VK_SUCCESS && supported) {
                probe.can_present = true;
            }
        }
    }

    if (surface != VK_NULL_HANDLE && !has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        probe.can_present = false;  // a surface needs the swapchain extension
    }

    core::DeviceCandidate& candidate = probe.candidate;
    candidate.index = index;
    candidate.name = caps.device_name;
    candidate.driver_name = caps.driver_name;
    candidate.type = ToDeviceType(props.deviceType);
    candidate.has_graphics_queue = graphics;
    candidate.fragment_stores_and_atomics = probe.features.fragmentStoresAndAtomics != VK_FALSE;
    candidate.can_present = probe.can_present;
    return probe;
}

// Turns the bits of the mask off.
void ApplyMask(Capabilities& caps, CapabilityMask mask) {
    if (mask.Has(Cap::D24S8)) caps.d24s8 = false;
    if (mask.Has(Cap::Interlock)) caps.interlock = false;
    if (mask.Has(Cap::IndexUint8)) caps.index_uint8 = false;
    if (mask.Has(Cap::CustomBorderColor)) caps.custom_border_color = false;
    if (mask.Has(Cap::TriangleFans)) caps.triangle_fans = false;
    if (mask.Has(Cap::LogicOp)) caps.logic_op = false;
    if (mask.Has(Cap::DynamicRendering)) caps.dynamic_rendering = false;
    if (mask.Has(Cap::PipelineLibrary)) caps.pipeline_library = false;
    if (mask.Has(Cap::Eds3Blend)) caps.eds3_blend = false;
    if (mask.Has(Cap::Bc7)) caps.bc7 = false;
    if (mask.Has(Cap::Timeline)) caps.timeline = false;
    if (mask.Has(Cap::PresentWait)) caps.present_wait = false;
    if (mask.Has(Cap::DirectUpload)) caps.direct_upload = false;
    caps.masked = mask;
}

}  // namespace

// ---- Backend::Impl -------------------------------------------------------------------------------

Backend::Impl::~Impl() {
    if (device != nullptr && device->vk != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->vk);
    }
    // Members go in reverse order: the other modules' slots, the framebuffers, the lists, the semaphore,
    // the queue, the device, then the instance.
}

Backend::Backend(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Backend::~Backend() = default;

const Capabilities& Backend::Caps() const {
    return impl_->caps;
}
bool Backend::PresentWaitAvailable() const {
    return impl_->present_wait_available;
}
bool Backend::PresentWaitMeasuresDisplay() const {
    return impl_->present_wait_measures_display;
}
bool Backend::DisplayTimingAvailable() const {
    return impl_->display_timing_available;
}
const std::vector<std::string>& Backend::EnabledExtensions() const {
    return impl_->enabled_extensions;
}
bool Backend::HasSurface() const {
    return impl_->surface != VK_NULL_HANDLE;
}

// ---- CreateBackend -------------------------------------------------------------------------------

Result<std::shared_ptr<Backend>> CreateBackend(std::shared_ptr<Instance> instance, const BackendOptions& options) {
    if (instance == nullptr) {
        return Error{"CreateBackend: no instance"};
    }
    ::plume::VulkanInterface* interface = instance->impl().vulkan();
    const VkSurfaceKHR surface = VulkanSurface(options.surface);

    Result<CapabilityMask> combined = core::CombineMasks(std::getenv("MH4N_RHI_MASK"), options.mask);
    if (!combined.ok()) {
        return Error{combined.error()};
    }
    const CapabilityMask mask = *combined;

    // Every physical device, in the order of vkEnumeratePhysicalDevices on the instance.
    u32 count = 0;
    vkEnumeratePhysicalDevices(interface->instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (count > 0) {
        vkEnumeratePhysicalDevices(interface->instance, &count, devices.data());
        devices.resize(count);
    }
    if (devices.empty()) {
        return Error{"the Vulkan loader lists no device (no driver installed for this machine's GPU?)"};
    }
    std::vector<Probe> probes;
    std::vector<core::DeviceCandidate> candidates;
    for (u32 i = 0; i < devices.size(); ++i) {
        Probe probe = ProbePhysicalDevice(devices[i], i, surface);
        Capabilities masked = probe.caps;
        ApplyMask(masked, mask);
        probe.candidate.capability_bits = CountBits(probe.caps, mask);
        candidates.push_back(probe.candidate);
        probes.push_back(std::move(probe));
    }

    std::string wanted = options.device;
    if (wanted.empty()) {
        if (const char* env = std::getenv("MH4N_VULKAN_DEVICE")) {
            wanted = env;
        }
    }
    const core::DeviceChoice choice = core::ChooseDevice(candidates, wanted, surface != VK_NULL_HANDLE);
    for (const std::string& line : choice.log) {
        MH4N_LOG(Info, "RHI", "rhi: {}", line);
    }
    if (!choice.ok) {
        return Error{choice.error};
    }
    const Probe& probe = probes[choice.index];

    // What to enable: the core features the capabilities use, and the optional extensions that are kept
    // (descriptor indexing, scalar block layout, robustness2, buffer device address, sampler mirror
    // clamp, sample locations, load/store op none), plus the ones the capabilities use and the mask
    // leaves on. Never ray tracing.
    ::plume::VulkanDeviceExtras extras;
    std::string failure;
    extras.error = &failure;
    extras.surface = surface;
    extras.physicalDeviceIndex = static_cast<int32_t>(choice.index);
    VkPhysicalDeviceFeatures wanted_features = {};
    wanted_features.fragmentStoresAndAtomics = VK_TRUE;
    wanted_features.vertexPipelineStoresAndAtomics = VK_TRUE;
    wanted_features.geometryShader = VK_TRUE;
    wanted_features.shaderClipDistance = VK_TRUE;
    wanted_features.samplerAnisotropy = VK_TRUE;
    wanted_features.imageCubeArray = VK_TRUE;
    wanted_features.logicOp = mask.Has(Cap::LogicOp) ? VK_FALSE : VK_TRUE;
    wanted_features.textureCompressionBC = mask.Has(Cap::Bc7) ? VK_FALSE : VK_TRUE;
    extras.coreFeatures = wanted_features;
    std::vector<std::string> extensions = {
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME,
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME,
        VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME,
        VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME,
    };
    if (!mask.Has(Cap::Interlock)) extensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    if (!mask.Has(Cap::IndexUint8)) extensions.push_back(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
    if (!mask.Has(Cap::CustomBorderColor)) extensions.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (surface != VK_NULL_HANDLE) {
        if (!mask.Has(Cap::PresentWait)) {
            extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            extensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        }
        extensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    }
    extras.optionalExtensions = extensions;
    extras.timelineSemaphore = !mask.Has(Cap::Timeline);

    std::unique_ptr<::plume::RenderDevice> device_owner = interface->createDeviceEx(std::string(), extras);
    if (device_owner == nullptr) {
        return Error{fmt::format("cannot make the device {} ({}): {}", probe.candidate.index, probe.candidate.Label(),
                                 failure.empty() ? "unknown failure" : failure)};
    }
    auto* device = static_cast<::plume::VulkanDevice*>(device_owner.get());

    auto impl = std::make_shared<Backend::Impl>();
    impl->instance = std::move(instance);
    impl->surface = surface;
    impl->limits = probe.limits;
    impl->physical_device = device->physicalDevice;
    impl->device_owner = std::move(device_owner);
    impl->device = device;
    impl->enabled_extensions = device->enabledExtensionNames;
    std::sort(impl->enabled_extensions.begin(), impl->enabled_extensions.end());

    // The final capabilities: the query, then the mask, then what the device really enabled.
    Capabilities caps = probe.caps;
    ApplyMask(caps, mask);
    const ::plume::RenderDeviceCapabilities& enabled = device->getCapabilities();
    caps.interlock = caps.interlock && enabled.fragmentShaderInterlock;
    caps.index_uint8 = caps.index_uint8 && enabled.indexTypeUint8;
    caps.custom_border_color = caps.custom_border_color && enabled.customBorderColor;
    caps.timeline = caps.timeline && enabled.timelineSemaphore;
    caps.logic_op = caps.logic_op && device->enabledCoreFeatures.logicOp;
    caps.bc7 = caps.bc7 && device->enabledCoreFeatures.textureCompressionBC;
    caps.geometry_shader = device->enabledCoreFeatures.geometryShader != VK_FALSE;
    impl->present_wait_available = surface != VK_NULL_HANDLE && caps.present_wait && enabled.presentWait;
    impl->display_timing_available = surface != VK_NULL_HANDLE && enabled.displayTiming;
    impl->present_wait_measures_display =
        core::PresentWaitMeasuresDisplay(impl->present_wait_available, caps.driver_id, caps.driver_name);
    impl->caps = std::move(caps);

    // The queue (one virtual queue on the direct family), its submit lock, the timeline.
    impl->queue_owner = impl->device_owner->createCommandQueue(::plume::RenderCommandListType::DIRECT);
    if (impl->queue_owner == nullptr) {
        return Error{"cannot make the command queue"};
    }
    impl->queue = static_cast<::plume::VulkanCommandQueue*>(impl->queue_owner.get());
    impl->queue_mutex = impl->queue->queue->mutex.get();
    impl->vk_queue = impl->queue->queue->vk;
    impl->queue_family = impl->queue->familyIndex;
    if (impl->caps.timeline) {
        impl->timeline_owner = impl->device_owner->createTimelineSemaphore(0);
        if (impl->timeline_owner == nullptr) {
            MH4N_LOG(Warning, "RHI", "rhi: the timeline semaphore could not be made; submissions use fences");
            impl->caps.timeline = false;
        } else {
            impl->timeline = static_cast<::plume::VulkanCommandSemaphore*>(impl->timeline_owner.get())->vk;
        }
    }
    if (surface != VK_NULL_HANDLE) {
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(impl->physical_device, impl->queue_family, surface, &supported);
        if (!supported) {
            return Error{"the queue family chosen for the device cannot present to the surface"};
        }
    }

    const Capabilities& c = impl->caps;
    MH4N_LOG(Info, "RHI", "rhi: mask {}", mask.bits != 0 ? mask.ToString() : std::string("none"));
    MH4N_LOG(Info, "RHI", "rhi: device {} \"{}\" driver \"{}\" (id {}) api {}.{}.{}, queue family {}, {}", c.device_index, c.device_name,
             c.driver_name, c.driver_id, (c.api_version >> 22) & 0x7F, (c.api_version >> 12) & 0x3FF, c.api_version & 0xFFF,
             impl->queue_family, surface != VK_NULL_HANDLE ? "with a surface" : "no surface");
    MH4N_LOG(Info, "RHI",
             "rhi: caps d24s8={} interlock={} index_uint8={} custom_border_color={} triangle_fans={} logic_op={} dynamic_rendering={} "
             "pipeline_library={} eds3_blend={} bc7={} timeline={} present_wait={} direct_upload={}",
             c.d24s8, c.interlock, c.index_uint8, c.custom_border_color, c.triangle_fans, c.logic_op, c.dynamic_rendering, c.pipeline_library,
             c.eds3_blend, c.bc7, c.timeline, c.present_wait, c.direct_upload);
    MH4N_LOG(Info, "RHI", "rhi: device extensions enabled: {}", core::JoinNames(impl->enabled_extensions));

    return std::make_shared<Backend>(std::move(impl));
}

}  // namespace mh4n::gpu::rhi
