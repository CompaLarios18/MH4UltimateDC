// The spike's Vulkan harness (N2-35): volk, one device, dynamic rendering, no RHI. Everything the metrics need:
// pipeline creation without a VkPipelineCache, timestamp queries around 20 full-screen triangles into a
// 3440x1440 target, a small target to read back, and VK_KHR_pipeline_executable_properties where the driver
// has it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "volk.h"

namespace spike {

constexpr uint32_t kBigWidth = 3440;
constexpr uint32_t kBigHeight = 1440;
constexpr uint32_t kSmallSize = 256;
constexpr uint32_t kTriangles = 20;

struct SpecConstants {
    std::vector<VkSpecializationMapEntry> entries;
    std::vector<uint32_t> data;
};

struct ExecutableStat {
    std::string executable; // the driver's name for the executable
    VkShaderStageFlags stages = 0;
    std::string name;
    std::string description;
    double value = 0.0;
};

struct InternalText {
    std::string executable;
    std::string name;
    VkShaderStageFlags stages = 0;
    std::string text;
};

struct DeviceInfo {
    std::string label;       // "<device name> (<driver name>)", what MH4N_VULKAN_DEVICE matches
    std::string name;
    std::string driver_name;
    std::string driver_info;
    std::string api;
    uint32_t vendor = 0;
    uint32_t device_id = 0;
    bool discrete = false;
    bool executable_properties = false; // VK_KHR_pipeline_executable_properties enabled
    double timestamp_period_ns = 0.0;
};

class Gpu {
public:
    ~Gpu();
    // Picks the device whose label contains `match` (case-insensitive); an empty `match` takes the first
    // discrete device and says so. Throws std::runtime_error with the list of devices when nothing matches.
    void Init(const std::string& match);
    const DeviceInfo& info() const { return info_; }

    VkShaderModule MakeModule(const std::vector<uint32_t>& spirv);

    // vkCreateGraphicsPipelines with no pipeline cache. `spec` is applied to the fragment stage when non-null.
    // `milliseconds` receives the time of the call alone.
    // `flags` is added to the pipeline's create flags (the capture bits of VK_KHR_pipeline_executable_properties);
    // the timed pipelines are created with none.
    VkPipeline MakePipeline(VkShaderModule vs, VkShaderModule fs, const SpecConstants* spec,
                            VkPipelineCreateFlags flags, double* milliseconds);

    // Allocates the uniform buffers, the LUT texel buffer and one descriptor set per key.
    void MakeResources(const std::vector<float>& frag_uniforms, const std::vector<float>& lut,
                       const std::vector<std::vector<uint32_t>>& cfg_per_key);
    VkDescriptorSet KeySet(size_t key) const { return sets_[key]; }

    // GPU time in nanoseconds of kTriangles full-screen triangles into the big target.
    double TimeDraw(VkPipeline pipeline, VkDescriptorSet set);
    // The same draw into the small target, read back as RGBA8.
    std::vector<uint8_t> Render(VkPipeline pipeline, VkDescriptorSet set);

    std::vector<ExecutableStat> Statistics(VkPipeline pipeline);
    // The text representations (the driver's disassembly, where it gives one) of a pipeline created with
    // VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR.
    std::vector<InternalText> InternalTexts(VkPipeline pipeline);

    void Destroy(VkPipeline p) { if (p != VK_NULL_HANDLE) vkDestroyPipeline(dev_, p, nullptr); }
    void Destroy(VkShaderModule m) { if (m != VK_NULL_HANDLE) vkDestroyShaderModule(dev_, m, nullptr); }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    uint32_t FindMemory(uint32_t type_bits, VkMemoryPropertyFlags want) const;
    Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props);
    Buffer MakeDeviceLocalBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    Image MakeImage(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void Submit(VkCommandBuffer cmd);
    void BeginCommands();
    void RecordDraw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, const Image& color,
                    const Image& depth, uint32_t w, uint32_t h, bool clear);
    void TransitionAll();

    DeviceInfo info_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets_;
    Buffer frag_ubo_, lut_buffer_, readback_;
    std::vector<Buffer> cfg_ubos_;
    VkBufferView lut_view_ = VK_NULL_HANDLE;
    Image big_color_, big_depth_, small_color_, small_depth_;
    uint32_t timestamp_bits_ = 64;
};

} // namespace spike
