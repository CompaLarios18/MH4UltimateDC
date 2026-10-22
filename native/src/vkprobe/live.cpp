// The live probe: the loader library and the Vulkan calls (N2-24).
#include "vkprobe/live.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include <fmt/format.h>

#include "mh4n/types.h"

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#if defined(_WIN32)
#include "../common/utf8.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mh4n::vkprobe {

namespace {

// ---- the loader library ---------------------------------------------------------------------

void* OpenLibrary(const std::string& name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryW(utf8::ToWide(name).c_str()));
#else
    return dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* Symbol(void* library, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

// ---- the structure missing from vulkan_core.h --------------------------------------------------
// VK_KHR_portability_subset is in vulkan_beta.h, which is not vendored. The layout is that of the
// specification (VkPhysicalDevicePortabilitySubsetFeaturesKHR, sType 1000163000).
constexpr VkStructureType kPortabilitySubsetFeaturesType = static_cast<VkStructureType>(1000163000);
struct PortabilitySubsetFeatures {
    VkStructureType sType = kPortabilitySubsetFeaturesType;
    void* pNext = nullptr;
    VkBool32 constantAlphaColorBlendFactors = VK_FALSE;
    VkBool32 events = VK_FALSE;
    VkBool32 imageViewFormatReinterpretation = VK_FALSE;
    VkBool32 imageViewFormatSwizzle = VK_FALSE;
    VkBool32 imageView2DOn3DImage = VK_FALSE;
    VkBool32 multisampleArrayImage = VK_FALSE;
    VkBool32 mutableComparisonSamplers = VK_FALSE;
    VkBool32 pointPolygons = VK_FALSE;
    VkBool32 samplerMipLodBias = VK_FALSE;
    VkBool32 separateStencilMaskRef = VK_FALSE;
    VkBool32 shaderSampleRateInterpolationFunctions = VK_FALSE;
    VkBool32 tessellationIsolines = VK_FALSE;
    VkBool32 tessellationPointMode = VK_FALSE;
    VkBool32 triangleFans = VK_FALSE;
    VkBool32 vertexAttributeAccessBeyondStride = VK_FALSE;
};

// ---- entry points -----------------------------------------------------------------------------

struct Api {
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions = nullptr;
    PFN_vkCreateInstance create_instance = nullptr;

    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumerate_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_properties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures get_features = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties get_format_properties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 get_properties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 get_features2 = nullptr;
};

template <typename T>
T LoadGlobal(const Api& api, const char* name) {
    return reinterpret_cast<T>(api.get_instance_proc(nullptr, name));
}
template <typename T>
T LoadInstance(const Api& api, VkInstance instance, const char* name) {
    return reinterpret_cast<T>(api.get_instance_proc(instance, name));
}

bool HasName(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(),
                       [&](const VkExtensionProperties& p) { return std::strcmp(p.extensionName, name) == 0; });
}

const char* DeviceTypeName(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
    default: return "other";
    }
}

Tri Bool(VkBool32 value) {
    return value != VK_FALSE ? Tri::Yes : Tri::No;
}

// Builds the pNext chain of the structures a device may be asked for.
class Chain {
public:
    template <typename T>
    void Add(T& structure) {
        auto* out = reinterpret_cast<VkBaseOutStructure*>(&structure);
        out->pNext = nullptr;
        if (tail_ == nullptr) {
            head_ = out;
        } else {
            tail_->pNext = out;
        }
        tail_ = out;
    }
    void* head() const { return head_; }

private:
    VkBaseOutStructure* head_ = nullptr;
    VkBaseOutStructure* tail_ = nullptr;
};

template <typename T>
T MakeStruct(VkStructureType type) {
    T s;
    std::memset(&s, 0, sizeof(s));
    s.sType = type;
    return s;
}

void ReadDevice(const Api& api, VkPhysicalDevice device, u32 instance_api, bool have_2, Facts* facts) {
    VkPhysicalDeviceProperties props{};
    api.get_properties(device, &props);
    facts->device_name = props.deviceName;
    facts->device_type = DeviceTypeName(props.deviceType);
    facts->api_version = props.apiVersion;
    facts->vendor_id = props.vendorID;
    facts->device_id = props.deviceID;

    // the device extensions
    std::vector<VkExtensionProperties> extensions;
    u32 count = 0;
    if (api.enumerate_device_extensions(device, nullptr, &count, nullptr) == VK_SUCCESS) {
        extensions.resize(count);
        if (api.enumerate_device_extensions(device, nullptr, &count, extensions.data()) == VK_SUCCESS) {
            extensions.resize(count);
            facts->extensions_known = true;
            for (const VkExtensionProperties& e : extensions) {
                facts->extensions.insert(e.extensionName);
            }
        }
    }
    const auto has = [&](const char* name) { return HasName(extensions, name); };
    // what the device and the instance both offer decides which core structures may be chained
    const u32 effective_api = std::min(props.apiVersion, instance_api);

    // VkPhysicalDeviceFeatures
    VkPhysicalDeviceFeatures base{};
    api.get_features(device, &base);
    auto& f = facts->features;
    f["logicOp"] = Bool(base.logicOp);
    f["geometryShader"] = Bool(base.geometryShader);
    f["fragmentStoresAndAtomics"] = Bool(base.fragmentStoresAndAtomics);
    f["shaderClipDistance"] = Bool(base.shaderClipDistance);
    f["textureCompressionBC"] = Bool(base.textureCompressionBC);

    if (have_2) {
        // Only structures the device can answer are chained: those of a core version it has, and
        // those of an extension it lists. Asking for another is invalid use.
        Chain chain;
        VkPhysicalDeviceFeatures2 features2 = MakeStruct<VkPhysicalDeviceFeatures2>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        chain.Add(features2);

        auto vk12 = MakeStruct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        auto timeline = MakeStruct<VkPhysicalDeviceTimelineSemaphoreFeatures>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
        const bool use_vk12 = effective_api >= VK_API_VERSION_1_2;
        const bool use_timeline = !use_vk12 && has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        if (use_vk12) chain.Add(vk12);
        if (use_timeline) chain.Add(timeline);

        auto vk13 = MakeStruct<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        auto dynamic_rendering = MakeStruct<VkPhysicalDeviceDynamicRenderingFeatures>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
        const bool use_vk13 = effective_api >= VK_API_VERSION_1_3;
        const bool use_dynamic = !use_vk13 && has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        if (use_vk13) chain.Add(vk13);
        if (use_dynamic) chain.Add(dynamic_rendering);

        auto interlock = MakeStruct<VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT);
        const bool use_interlock = has(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
        if (use_interlock) chain.Add(interlock);

        auto barycentric = MakeStruct<VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR);
        const bool use_barycentric = has(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) ||
                                     has("VK_NV_fragment_shader_barycentric");  // same layout and sType value
        if (use_barycentric) chain.Add(barycentric);

        auto uint8 = MakeStruct<VkPhysicalDeviceIndexTypeUint8FeaturesEXT>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT);
        const bool use_uint8 = has(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME) || has("VK_KHR_index_type_uint8");
        if (use_uint8) chain.Add(uint8);

        auto border = MakeStruct<VkPhysicalDeviceCustomBorderColorFeaturesEXT>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
        const bool use_border = has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        if (use_border) chain.Add(border);

        auto rasterization_order = MakeStruct<VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT);
        const bool use_rasterization_order = has(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME) ||
                                             has("VK_ARM_rasterization_order_attachment_access");
        if (use_rasterization_order) chain.Add(rasterization_order);

        auto pipeline_library = MakeStruct<VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT);
        const bool use_pipeline_library = has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        if (use_pipeline_library) chain.Add(pipeline_library);

        auto eds1 = MakeStruct<VkPhysicalDeviceExtendedDynamicStateFeaturesEXT>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT);
        const bool use_eds1 = has(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
        if (use_eds1) chain.Add(eds1);
        auto eds2 = MakeStruct<VkPhysicalDeviceExtendedDynamicState2FeaturesEXT>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT);
        const bool use_eds2 = has(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
        if (use_eds2) chain.Add(eds2);
        auto eds3 = MakeStruct<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
        const bool use_eds3 = has(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
        if (use_eds3) chain.Add(eds3);

        PortabilitySubsetFeatures subset;
        const bool use_subset = has("VK_KHR_portability_subset");
        if (use_subset) chain.Add(subset);

        api.get_features2(device, &features2);

        const auto merge = [&](const char* name, VkBool32 value) {
            Tri& slot = f[name];
            if (Bool(value) == Tri::Yes || slot == Tri::Unknown) {
                slot = Bool(value);
            }
        };
        if (use_vk12) merge("timelineSemaphore", vk12.timelineSemaphore);
        if (use_timeline) merge("timelineSemaphore", timeline.timelineSemaphore);
        if (use_vk13) merge("dynamicRendering", vk13.dynamicRendering);
        if (use_dynamic) merge("dynamicRendering", dynamic_rendering.dynamicRendering);
        if (use_interlock) {
            merge("fragmentShaderPixelInterlock", interlock.fragmentShaderPixelInterlock);
            merge("fragmentShaderSampleInterlock", interlock.fragmentShaderSampleInterlock);
        }
        if (use_barycentric) merge("fragmentShaderBarycentric", barycentric.fragmentShaderBarycentric);
        if (use_uint8) merge("indexTypeUint8", uint8.indexTypeUint8);
        if (use_border) merge("customBorderColors", border.customBorderColors);
        if (use_rasterization_order) {
            merge("rasterizationOrderColorAttachmentAccess", rasterization_order.rasterizationOrderColorAttachmentAccess);
        }
        if (use_pipeline_library) merge("graphicsPipelineLibrary", pipeline_library.graphicsPipelineLibrary);
        if (use_eds1) merge("extendedDynamicState", eds1.extendedDynamicState);
        if (use_eds2) merge("extendedDynamicState2", eds2.extendedDynamicState2);
        if (use_eds3) {
#define MH4N_EDS3(member) merge("extendedDynamicState3" #member, eds3.extendedDynamicState3##member)
            MH4N_EDS3(TessellationDomainOrigin);
            MH4N_EDS3(DepthClampEnable);
            MH4N_EDS3(PolygonMode);
            MH4N_EDS3(RasterizationSamples);
            MH4N_EDS3(SampleMask);
            MH4N_EDS3(AlphaToCoverageEnable);
            MH4N_EDS3(AlphaToOneEnable);
            MH4N_EDS3(LogicOpEnable);
            MH4N_EDS3(ColorBlendEnable);
            MH4N_EDS3(ColorBlendEquation);
            MH4N_EDS3(ColorWriteMask);
            MH4N_EDS3(RasterizationStream);
            MH4N_EDS3(ConservativeRasterizationMode);
            MH4N_EDS3(ExtraPrimitiveOverestimationSize);
            MH4N_EDS3(DepthClipEnable);
            MH4N_EDS3(SampleLocationsEnable);
            MH4N_EDS3(ColorBlendAdvanced);
            MH4N_EDS3(ProvokingVertexMode);
            MH4N_EDS3(LineRasterizationMode);
            MH4N_EDS3(LineStippleEnable);
            MH4N_EDS3(DepthClipNegativeOneToOne);
            MH4N_EDS3(ViewportWScalingEnable);
            MH4N_EDS3(ViewportSwizzle);
            MH4N_EDS3(CoverageToColorEnable);
            MH4N_EDS3(CoverageToColorLocation);
            MH4N_EDS3(CoverageModulationMode);
            MH4N_EDS3(CoverageModulationTableEnable);
            MH4N_EDS3(CoverageModulationTable);
            MH4N_EDS3(CoverageReductionMode);
            MH4N_EDS3(RepresentativeFragmentTestEnable);
            MH4N_EDS3(ShadingRateImageEnable);
#undef MH4N_EDS3
        }
        if (use_subset) merge("triangleFans", subset.triangleFans);

        // the driver (VkPhysicalDeviceDriverProperties: core in 1.2, else VK_KHR_driver_properties)
        if (effective_api >= VK_API_VERSION_1_2 || has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
            Chain props_chain;
            VkPhysicalDeviceProperties2 props2 = MakeStruct<VkPhysicalDeviceProperties2>(
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
            auto driver = MakeStruct<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
            props_chain.Add(props2);
            props_chain.Add(driver);
            api.get_properties2(device, &props2);
            facts->driver_name = driver.driverName;
            facts->driver_info = driver.driverInfo;
        }
    }

    // formats: the depth-stencil formats and BC7, with optimal tiling
    const struct {
        const char* name;
        VkFormat format;
    } formats[] = {{"D24_UNORM_S8_UINT", VK_FORMAT_D24_UNORM_S8_UINT},
                   {"D32_SFLOAT_S8_UINT", VK_FORMAT_D32_SFLOAT_S8_UINT},
                   {"BC7_UNORM_BLOCK", VK_FORMAT_BC7_UNORM_BLOCK}};
    for (const auto& format : formats) {
        VkFormatProperties properties{};
        api.get_format_properties(device, format.format, &properties);
        FormatSupport support;
        support.depth_stencil_attachment =
            Bool((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);
        support.sampled_image = Bool((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0);
        facts->formats[format.name] = support;
    }
}

}  // namespace

std::vector<std::string> DefaultLoaderNames() {
#if defined(_WIN32)
    return {"vulkan-1.dll"};
#elif defined(__APPLE__)
    return {"libvulkan.1.dylib",
            "libvulkan.dylib",
            "libMoltenVK.dylib",
            "/opt/homebrew/lib/libvulkan.1.dylib",
            "/usr/local/lib/libvulkan.1.dylib",
            "/opt/homebrew/lib/libMoltenVK.dylib",
            "/usr/local/lib/libMoltenVK.dylib"};
#else
    return {"libvulkan.so.1", "libvulkan.so"};
#endif
}

bool ProbeLive(const std::vector<std::string>& loader_names, std::vector<Facts>* devices, std::string* error) {
    devices->clear();
    Api api;
    void* library = nullptr;
    std::string tried;
    for (const std::string& name : loader_names) {
        void* handle = OpenLibrary(name);
        if (handle != nullptr) {
            auto* proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(Symbol(handle, "vkGetInstanceProcAddr"));
            if (proc != nullptr) {
                library = handle;
                api.get_instance_proc = proc;
                break;
            }
        }
        tried += (tried.empty() ? "" : ", ") + name;
    }
    if (library == nullptr) {
        *error = "no Vulkan loader found (tried " + (tried.empty() ? std::string("nothing") : tried) +
                 "): install the GPU driver or the Vulkan runtime";
        return false;
    }

    api.enumerate_instance_version = LoadGlobal<PFN_vkEnumerateInstanceVersion>(api, "vkEnumerateInstanceVersion");
    api.enumerate_instance_extensions =
        LoadGlobal<PFN_vkEnumerateInstanceExtensionProperties>(api, "vkEnumerateInstanceExtensionProperties");
    api.create_instance = LoadGlobal<PFN_vkCreateInstance>(api, "vkCreateInstance");
    if (api.create_instance == nullptr || api.enumerate_instance_extensions == nullptr) {
        *error = "the Vulkan loader has no vkCreateInstance";
        return false;
    }

    // The newest version this program's headers know (1.3), or the loader's if older. A loader
    // without vkEnumerateInstanceVersion is a Vulkan 1.0 one.
    u32 loader_version = VK_API_VERSION_1_0;
    if (api.enumerate_instance_version != nullptr) {
        api.enumerate_instance_version(&loader_version);
    }
    const u32 app_api = std::min(loader_version, static_cast<u32>(VK_API_VERSION_1_3));

    std::vector<VkExtensionProperties> instance_extensions;
    u32 count = 0;
    if (api.enumerate_instance_extensions(nullptr, &count, nullptr) == VK_SUCCESS) {
        instance_extensions.resize(count);
        if (api.enumerate_instance_extensions(nullptr, &count, instance_extensions.data()) != VK_SUCCESS) {
            instance_extensions.clear();
        } else {
            instance_extensions.resize(count);
        }
    }
    std::vector<const char*> enabled;
    VkInstanceCreateFlags flags = 0;
    // MoltenVK and other portability drivers are only listed when the instance asks for them
    if (HasName(instance_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    bool props2_extension = false;
    if (app_api < VK_API_VERSION_1_1 && HasName(instance_extensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
        enabled.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        props2_extension = true;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "mh4n_vkprobe";
    app.apiVersion = app_api;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.flags = flags;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<u32>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    VkInstance instance = nullptr;
    const VkResult created = api.create_instance(&info, nullptr, &instance);
    if (created != VK_SUCCESS || instance == nullptr) {
        *error = fmt::format("vkCreateInstance failed (VkResult {})", static_cast<int>(created));
        return false;
    }

    api.destroy_instance = LoadInstance<PFN_vkDestroyInstance>(api, instance, "vkDestroyInstance");
    api.enumerate_devices = LoadInstance<PFN_vkEnumeratePhysicalDevices>(api, instance, "vkEnumeratePhysicalDevices");
    api.get_properties = LoadInstance<PFN_vkGetPhysicalDeviceProperties>(api, instance, "vkGetPhysicalDeviceProperties");
    api.get_features = LoadInstance<PFN_vkGetPhysicalDeviceFeatures>(api, instance, "vkGetPhysicalDeviceFeatures");
    api.get_format_properties =
        LoadInstance<PFN_vkGetPhysicalDeviceFormatProperties>(api, instance, "vkGetPhysicalDeviceFormatProperties");
    api.enumerate_device_extensions =
        LoadInstance<PFN_vkEnumerateDeviceExtensionProperties>(api, instance, "vkEnumerateDeviceExtensionProperties");
    const char* suffix = app_api >= VK_API_VERSION_1_1 ? "" : "KHR";
    if (app_api >= VK_API_VERSION_1_1 || props2_extension) {
        api.get_properties2 = LoadInstance<PFN_vkGetPhysicalDeviceProperties2>(
            api, instance, (std::string("vkGetPhysicalDeviceProperties2") + suffix).c_str());
        api.get_features2 = LoadInstance<PFN_vkGetPhysicalDeviceFeatures2>(
            api, instance, (std::string("vkGetPhysicalDeviceFeatures2") + suffix).c_str());
    }
    const bool complete = api.destroy_instance != nullptr && api.enumerate_devices != nullptr &&
                          api.get_properties != nullptr && api.get_features != nullptr &&
                          api.get_format_properties != nullptr && api.enumerate_device_extensions != nullptr;
    if (!complete) {
        *error = "the Vulkan loader lacks core instance functions";
        if (api.destroy_instance != nullptr) {
            api.destroy_instance(instance, nullptr);
        }
        return false;
    }
    const bool have_2 = api.get_properties2 != nullptr && api.get_features2 != nullptr;

    std::vector<VkPhysicalDevice> physical;
    count = 0;
    VkResult listed = api.enumerate_devices(instance, &count, nullptr);
    if (listed == VK_SUCCESS && count > 0) {
        physical.resize(count);
        listed = api.enumerate_devices(instance, &count, physical.data());
        if (listed == VK_SUCCESS || listed == VK_INCOMPLETE) {
            physical.resize(count);
        } else {
            physical.clear();
        }
    }
    for (VkPhysicalDevice device : physical) {
        Facts facts;
        ReadDevice(api, device, app_api, have_2, &facts);
        devices->push_back(std::move(facts));
    }
    api.destroy_instance(instance, nullptr);
    if (devices->empty()) {
        *error = "the Vulkan loader loaded but lists no device (no driver installed for this machine's GPU?)";
        return false;
    }
    return true;
}

}  // namespace mh4n::vkprobe
