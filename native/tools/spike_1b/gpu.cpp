#include "gpu.h"

// Vulkan's structures are filled field by field after an sType-only initialiser.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace spike {

namespace {

void Check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed: VkResult " + std::to_string(static_cast<int>(r)));
    }
}

std::string Lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string ApiString(uint32_t v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

} // namespace

Gpu::~Gpu() {
    if (dev_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(dev_);
        vkDestroyDevice(dev_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

void Gpu::Init(const std::string& match) {
    Check(volkInitialize(), "volkInitialize (is there a Vulkan loader?)");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mh4n_spike_1b";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    Check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance (Vulkan 1.3)");
    volkLoadInstance(instance_);

    uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");

    struct Candidate {
        VkPhysicalDevice device;
        DeviceInfo info;
    };
    std::vector<Candidate> candidates;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
        vkGetPhysicalDeviceProperties2(d, &props);
        DeviceInfo info;
        info.name = props.properties.deviceName;
        info.driver_name = driver.driverName;
        info.driver_info = driver.driverInfo;
        info.label = info.name + " (" + info.driver_name + ")";
        info.api = ApiString(props.properties.apiVersion);
        info.vendor = props.properties.vendorID;
        info.device_id = props.properties.deviceID;
        info.discrete = props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        info.timestamp_period_ns = static_cast<double>(props.properties.limits.timestampPeriod);
        candidates.push_back({d, info});
    }

    const Candidate* chosen = nullptr;
    if (match.empty()) {
        for (const Candidate& c : candidates) {
            if (c.info.discrete) {
                chosen = &c;
                break;
            }
        }
        if (chosen == nullptr && !candidates.empty()) {
            chosen = &candidates.front();
        }
    } else {
        const std::string needle = Lower(match);
        for (const Candidate& c : candidates) {
            if (Lower(c.info.label).find(needle) != std::string::npos) {
                chosen = &c;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        std::string list;
        for (size_t i = 0; i < candidates.size(); ++i) {
            list += "\n  device " + std::to_string(i) + ": " + candidates[i].info.label;
        }
        throw std::runtime_error("no Vulkan device matches \"" + match + "\"; devices:" + list);
    }
    phys_ = chosen->device;
    info_ = chosen->info;
    vkGetPhysicalDeviceMemoryProperties(phys_, &memory_);

    // Device extensions: only VK_KHR_pipeline_executable_properties is optional.
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ext_count, exts.data());
    bool has_exec = false;
    for (const VkExtensionProperties& e : exts) {
        if (std::strcmp(e.extensionName, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) == 0) {
            has_exec = true;
        }
    }
    VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR have_exec{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    have13.pNext = has_exec ? &have_exec : nullptr;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &have13};
    vkGetPhysicalDeviceFeatures2(phys_, &features);
    if (!have13.dynamicRendering) {
        throw std::runtime_error("the device lacks dynamicRendering (Vulkan 1.3)");
    }
    has_exec = has_exec && have_exec.pipelineExecutableInfo;
    info_.executable_properties = has_exec;

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &family_count, families.data());
    bool found = false;
    for (uint32_t i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[i].timestampValidBits > 0) {
            family_ = i;
            timestamp_bits_ = families[i].timestampValidBits;
            found = true;
            break;
        }
    }
    if (!found) {
        throw std::runtime_error("no graphics queue family with timestamps");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enable13.dynamicRendering = VK_TRUE;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR enable_exec{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    enable_exec.pipelineExecutableInfo = VK_TRUE;
    enable13.pNext = has_exec ? &enable_exec : nullptr;
    const char* device_extensions[] = {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enable13};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = has_exec ? 1u : 0u;
    dci.ppEnabledExtensionNames = device_extensions;
    Check(vkCreateDevice(phys_, &dci, nullptr, &dev_), "vkCreateDevice");
    volkLoadDevice(dev_);
    vkGetDeviceQueue(dev_, family_, 0, &queue_);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = family_;
    Check(vkCreateCommandPool(dev_, &cpi, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(dev_, &cai, &cmd_), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(vkCreateFence(dev_, &fci, nullptr, &fence_), "vkCreateFence");
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 2;
    Check(vkCreateQueryPool(dev_, &qpi, nullptr, &queries_), "vkCreateQueryPool");

    // One layout for every pipeline: the fragment uniforms, the configuration block, the LUT texel buffer.
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 3;
    dsl.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(dev_, &dsl, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    Check(vkCreatePipelineLayout(dev_, &pli, nullptr, &layout_), "vkCreatePipelineLayout");

    big_color_ = MakeImage(VK_FORMAT_R8G8B8A8_UNORM, kBigWidth, kBigHeight,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
    big_depth_ = MakeImage(VK_FORMAT_D32_SFLOAT, kBigWidth, kBigHeight, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
    small_color_ = MakeImage(VK_FORMAT_R8G8B8A8_UNORM, kSmallSize, kSmallSize,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    small_depth_ = MakeImage(VK_FORMAT_D32_SFLOAT, kSmallSize, kSmallSize,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    readback_ = MakeBuffer(kSmallSize * kSmallSize * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    TransitionAll();
}

uint32_t Gpu::FindMemory(uint32_t type_bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0 && (memory_.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    throw std::runtime_error("no memory type with the wanted properties");
}

Gpu::Buffer Gpu::MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(dev_, &bci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    try {
        mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, props);
    } catch (const std::runtime_error&) {
        // The cached bit is a preference for the read-back buffer only.
        mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, props & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    Check(vkAllocateMemory(dev_, &mai, nullptr, &b.memory), "vkAllocateMemory");
    Check(vkBindBufferMemory(dev_, b.buffer, b.memory, 0), "vkBindBufferMemory");
    if ((props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        Check(vkMapMemory(dev_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    }
    return b;
}

Gpu::Buffer Gpu::MakeDeviceLocalBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer staging = MakeBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));
    Buffer device = MakeBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    BeginCommands();
    VkBufferCopy region{0, 0, size};
    vkCmdCopyBuffer(cmd_, staging.buffer, device.buffer, 1, &region);
    Submit(cmd_);
    vkDestroyBuffer(dev_, staging.buffer, nullptr);
    vkFreeMemory(dev_, staging.memory, nullptr);
    return device;
}

Gpu::Image Gpu::MakeImage(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect) {
    Image img;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(dev_, &ici, nullptr, &img.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, img.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(dev_, &mai, nullptr, &img.memory), "vkAllocateMemory (image)");
    Check(vkBindImageMemory(dev_, img.image, img.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, 1, 0, 1};
    Check(vkCreateImageView(dev_, &vci, nullptr, &img.view), "vkCreateImageView");
    return img;
}

void Gpu::BeginCommands() {
    Check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
    // Everything an earlier submission wrote is visible here (the fences already ordered the host).
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

void Gpu::Submit(VkCommandBuffer cmd) {
    Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    Check(vkResetFences(dev_, 1, &fence_), "vkResetFences");
    Check(vkQueueSubmit(queue_, 1, &si, fence_), "vkQueueSubmit");
    Check(vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

void Gpu::TransitionAll() {
    BeginCommands();
    auto barrier = [&](const Image& img, VkImageAspectFlags aspect, VkImageLayout to, VkAccessFlags access,
                       VkPipelineStageFlags stage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = 0;
        b.dstAccessMask = access;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    const VkPipelineStageFlags color_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags depth_stage =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags color_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    const VkAccessFlags depth_access =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier(big_color_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_access, color_stage);
    barrier(small_color_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_access,
            color_stage);
    barrier(big_depth_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depth_access, depth_stage);
    barrier(small_depth_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depth_access,
            depth_stage);
    Submit(cmd_);
}

VkShaderModule Gpu::MakeModule(const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(dev_, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

VkPipeline Gpu::MakePipeline(VkShaderModule vs, VkShaderModule fs, const SpecConstants* spec,
                             VkPipelineCreateFlags flags, double* milliseconds) {
    VkSpecializationInfo spec_info{};
    if (spec != nullptr) {
        spec_info.mapEntryCount = static_cast<uint32_t>(spec->entries.size());
        spec_info.pMapEntries = spec->entries.data();
        spec_info.dataSize = spec->data.size() * sizeof(uint32_t);
        spec_info.pData = spec->data.data();
    }
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = spec != nullptr ? &spec_info : nullptr;

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic;
    const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    gci.flags = flags;
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &dyn;
    gci.layout = layout_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto start = std::chrono::steady_clock::now();
    const VkResult r = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gci, nullptr, &pipeline);
    const auto stop = std::chrono::steady_clock::now();
    Check(r, "vkCreateGraphicsPipelines");
    if (milliseconds != nullptr) {
        *milliseconds = std::chrono::duration<double, std::milli>(stop - start).count();
    }
    return pipeline;
}

void Gpu::MakeResources(const std::vector<float>& frag_uniforms, const std::vector<float>& lut,
                        const std::vector<std::vector<uint32_t>>& cfg_per_key) {
    frag_ubo_ = MakeDeviceLocalBuffer(frag_uniforms.data(), frag_uniforms.size() * sizeof(float),
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    lut_buffer_ = MakeDeviceLocalBuffer(lut.data(), lut.size() * sizeof(float),
                                        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    VkBufferViewCreateInfo bvi{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    bvi.buffer = lut_buffer_.buffer;
    bvi.format = VK_FORMAT_R32_SFLOAT;
    bvi.offset = 0;
    bvi.range = VK_WHOLE_SIZE;
    Check(vkCreateBufferView(dev_, &bvi, nullptr, &lut_view_), "vkCreateBufferView");

    const uint32_t n = static_cast<uint32_t>(cfg_per_key.size());
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * n},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, n}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = n;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(dev_, &dpi, nullptr, &descriptor_pool_), "vkCreateDescriptorPool");
    std::vector<VkDescriptorSetLayout> layouts(n, set_layout_);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descriptor_pool_;
    dai.descriptorSetCount = n;
    dai.pSetLayouts = layouts.data();
    sets_.assign(n, VK_NULL_HANDLE);
    Check(vkAllocateDescriptorSets(dev_, &dai, sets_.data()), "vkAllocateDescriptorSets");

    for (uint32_t i = 0; i < n; ++i) {
        cfg_ubos_.push_back(MakeDeviceLocalBuffer(cfg_per_key[i].data(), cfg_per_key[i].size() * sizeof(uint32_t),
                                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT));
        VkDescriptorBufferInfo frag{frag_ubo_.buffer, 0, frag_ubo_.size};
        VkDescriptorBufferInfo cfg{cfg_ubos_[i].buffer, 0, cfg_ubos_[i].size};
        VkWriteDescriptorSet w[3] = {};
        for (VkWriteDescriptorSet& x : w) {
            x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            x.dstSet = sets_[i];
            x.descriptorCount = 1;
        }
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &frag;
        w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[1].pBufferInfo = &cfg;
        w[2].dstBinding = 2;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        w[2].pTexelBufferView = &lut_view_;
        vkUpdateDescriptorSets(dev_, 3, w, 0, nullptr);
    }
}

void Gpu::RecordDraw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, const Image& color,
                     const Image& depth, uint32_t w, uint32_t h, bool clear) {
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = color.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{0.5f, 0.0f, 0.5f, 0.0f}};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = depth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, kTriangles, 0, 0);
    vkCmdEndRendering(cmd);
}

double Gpu::TimeDraw(VkPipeline pipeline, VkDescriptorSet set) {
    BeginCommands();
    vkCmdResetQueryPool(cmd_, queries_, 0, 2);
    vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
    RecordDraw(cmd_, pipeline, set, big_color_, big_depth_, kBigWidth, kBigHeight, false);
    vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
    Submit(cmd_);
    uint64_t t[2] = {0, 0};
    Check(vkGetQueryPoolResults(dev_, queries_, 0, 2, sizeof t, t, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
          "vkGetQueryPoolResults");
    uint64_t mask = timestamp_bits_ >= 64 ? ~0ull : ((1ull << timestamp_bits_) - 1);
    const uint64_t ticks = (t[1] - t[0]) & mask;
    return static_cast<double>(ticks) * info_.timestamp_period_ns;
}

std::vector<uint8_t> Gpu::Render(VkPipeline pipeline, VkDescriptorSet set) {
    BeginCommands();
    RecordDraw(cmd_, pipeline, set, small_color_, small_depth_, kSmallSize, kSmallSize, true);
    VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = small_color_.image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_src);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kSmallSize, kSmallSize, 1};
    vkCmdCopyImageToBuffer(cmd_, small_color_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1,
                           &copy);
    VkImageMemoryBarrier back = to_src;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &back);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                         nullptr);
    Submit(cmd_);
    const uint8_t* p = static_cast<const uint8_t*>(readback_.mapped);
    return std::vector<uint8_t>(p, p + kSmallSize * kSmallSize * 4);
}

std::vector<InternalText> Gpu::InternalTexts(VkPipeline pipeline) {
    std::vector<InternalText> out;
    if (!info_.executable_properties) {
        return out;
    }
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = pipeline;
    uint32_t count = 0;
    Check(vkGetPipelineExecutablePropertiesKHR(dev_, &pi, &count, nullptr), "vkGetPipelineExecutablePropertiesKHR");
    std::vector<VkPipelineExecutablePropertiesKHR> props(count);
    for (VkPipelineExecutablePropertiesKHR& p : props) {
        p = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
    }
    Check(vkGetPipelineExecutablePropertiesKHR(dev_, &pi, &count, props.data()),
          "vkGetPipelineExecutablePropertiesKHR");
    for (uint32_t i = 0; i < count; ++i) {
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = pipeline;
        ei.executableIndex = i;
        uint32_t n = 0;
        Check(vkGetPipelineExecutableInternalRepresentationsKHR(dev_, &ei, &n, nullptr),
              "vkGetPipelineExecutableInternalRepresentationsKHR");
        std::vector<VkPipelineExecutableInternalRepresentationKHR> reps(n);
        for (auto& r : reps) {
            r = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR};
        }
        // The first call with no data buffers fills the sizes, the second the data.
        Check(vkGetPipelineExecutableInternalRepresentationsKHR(dev_, &ei, &n, reps.data()),
              "vkGetPipelineExecutableInternalRepresentationsKHR");
        std::vector<std::vector<char>> data(n);
        for (uint32_t j = 0; j < n; ++j) {
            data[j].resize(reps[j].dataSize + 1, 0);
            reps[j].pData = data[j].data();
        }
        Check(vkGetPipelineExecutableInternalRepresentationsKHR(dev_, &ei, &n, reps.data()),
              "vkGetPipelineExecutableInternalRepresentationsKHR");
        for (uint32_t j = 0; j < n; ++j) {
            if (reps[j].isText == VK_FALSE) {
                continue;
            }
            InternalText t;
            t.executable = props[i].name;
            t.name = reps[j].name;
            t.stages = props[i].stages;
            t.text.assign(data[j].data(), reps[j].dataSize);
            out.push_back(std::move(t));
        }
    }
    return out;
}

std::vector<ExecutableStat> Gpu::Statistics(VkPipeline pipeline) {
    std::vector<ExecutableStat> out;
    if (!info_.executable_properties) {
        return out;
    }
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = pipeline;
    uint32_t count = 0;
    Check(vkGetPipelineExecutablePropertiesKHR(dev_, &pi, &count, nullptr), "vkGetPipelineExecutablePropertiesKHR");
    std::vector<VkPipelineExecutablePropertiesKHR> props(count);
    for (VkPipelineExecutablePropertiesKHR& p : props) {
        p = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
    }
    Check(vkGetPipelineExecutablePropertiesKHR(dev_, &pi, &count, props.data()),
          "vkGetPipelineExecutablePropertiesKHR");
    for (uint32_t i = 0; i < count; ++i) {
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = pipeline;
        ei.executableIndex = i;
        uint32_t n = 0;
        Check(vkGetPipelineExecutableStatisticsKHR(dev_, &ei, &n, nullptr), "vkGetPipelineExecutableStatisticsKHR");
        std::vector<VkPipelineExecutableStatisticKHR> stats(n);
        for (VkPipelineExecutableStatisticKHR& s : stats) {
            s = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
        }
        Check(vkGetPipelineExecutableStatisticsKHR(dev_, &ei, &n, stats.data()),
              "vkGetPipelineExecutableStatisticsKHR");
        for (const VkPipelineExecutableStatisticKHR& s : stats) {
            ExecutableStat e;
            e.executable = props[i].name;
            e.stages = props[i].stages;
            e.name = s.name;
            e.description = s.description;
            switch (s.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: e.value = s.value.b32 ? 1.0 : 0.0; break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: e.value = static_cast<double>(s.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: e.value = static_cast<double>(s.value.u64); break;
            default: e.value = s.value.f64; break;
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

} // namespace spike
