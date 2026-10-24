// The patched plume (N2-25): the base cases on its public API, run on whatever Vulkan device the machine has
// (lavapipe in the cloud and in Linux CI). The patterns are ported from
// docs/research/rhi-spike-evidence/test/main.cpp (this repository's code), the shaders are the evidence's
// (native/tests/shaders/rhi_plume, compiled into rhi_plume_shaders.inc by tools/native/spirv_inc.py).
//
// Without a Vulkan device a case skips with a message; with MH4N_REQUIRE_VULKAN=1 it fails instead.
// `control/C5` is a deliberate misuse for tools/native/vk_layer_run.py: it is skipped unless named on the
// command line (`--test-case=control/C5 --no-skip`) and must make the validation layer report
// SYNC-HAZARD-READ-AFTER-WRITE.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gpu/backend/plume/vendor_info.h"
#include "plume_vulkan.h"

#include "rhi_plume_shaders.inc"

using namespace plume;

namespace {

constexpr uint32_t kW = 16;
constexpr uint32_t kH = 16;

#define SPV(a) (const void*)(a), sizeof(a)

bool VulkanRequired() {
    const char* value = std::getenv("MH4N_REQUIRE_VULKAN");
    return value != nullptr && std::string(value) == "1";
}

bool HeadlessSurfaceRequired() {
    const char* value = std::getenv("MH4N_REQUIRE_HEADLESS_SURFACE");
    return value != nullptr && std::string(value) == "1";
}

// A plume instance and device, or a reason there is none.
struct Rig {
    std::unique_ptr<RenderInterface> iface;
    std::unique_ptr<RenderDevice> dev;
    VulkanInterface* vi = nullptr;
    VulkanDevice* vd = nullptr;
    std::string error;

    bool Open(const VulkanInterfaceOptions& options, const VulkanDeviceExtras& extras = {}) {
        VulkanInterfaceOptions opts = options;
        opts.error = &error;
        iface = CreateVulkanInterface(opts);
        if (!iface) {
            return false;
        }
        vi = static_cast<VulkanInterface*>(iface.get());
        VulkanDeviceExtras ex = extras;
        ex.error = &error;
        dev = vi->createDeviceEx("", ex);
        if (!dev) {
            return false;
        }
        vd = static_cast<VulkanDevice*>(dev.get());
        return true;
    }
};

// true: go on. false: no device and not required: the case has been skipped with a message.
// A required device that is missing fails the case.
bool DeviceOk(bool ok, const std::string& error, const char* what) {
    if (ok) {
        return true;
    }
    if (VulkanRequired()) {
        FAIL(what << ": no Vulkan device and MH4N_REQUIRE_VULKAN=1: " << error);
    }
    WARN_MESSAGE(false, "skipped: " << what << ": no Vulkan device (" << error << ")");
    return false;
}

void LogDevice(const Rig& rig) {
    std::printf("rhi_plume_test: device: %s\n", rig.vd->description.name.c_str());
    std::fflush(stdout);
}

bool Has(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool AnyContains(const std::vector<std::string>& names, const std::string& part) {
    return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return n.find(part) != std::string::npos; });
}

// ---- the evidence's scene (main.cpp, struct Scene) ---------------------------------------------------------------

struct Set1 {
    std::vector<RenderDescriptorRange> ranges;
    RenderDescriptorSetDesc sd;
    std::unique_ptr<RenderPipelineLayout> layout;
    std::unique_ptr<RenderDescriptorSet> set;
    void Build(RenderDevice& dev, std::vector<RenderDescriptorRange> r) {
        ranges = std::move(r);
        sd = RenderDescriptorSetDesc(ranges.data(), uint32_t(ranges.size()));
        RenderPipelineLayoutDesc ld;
        ld.descriptorSetDescs = &sd;
        ld.descriptorSetDescsCount = 1;
        layout = dev.createPipelineLayout(ld);
        set = dev.createDescriptorSet(sd);
    }
};

std::unique_ptr<RenderShader> MakeShader(RenderDevice& dev, const void* data, size_t size) {
    return dev.createShader(data, size, "main", RenderShaderFormat::SPIRV);
}

std::unique_ptr<RenderPipeline> MakePipeline(RenderDevice& dev, const RenderPipelineLayout* layout, const RenderShader* vs,
                                             const RenderShader* fs) {
    RenderGraphicsPipelineDesc d;
    d.pipelineLayout = layout;
    d.vertexShader = vs;
    d.pixelShader = fs;
    d.renderTargetCount = 1;
    d.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
    d.renderTargetBlend[0] = RenderBlendDesc::Copy();
    d.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    d.cullMode = RenderCullMode::NONE;
    return dev.createGraphicsPipeline(d);
}

std::unique_ptr<RenderTexture> MakeTarget(RenderDevice& dev) {
    return dev.createTexture(RenderTextureDesc::ColorTarget(kW, kH, RenderFormat::R8G8B8A8_UNORM));
}

std::unique_ptr<RenderFramebuffer> MakeFramebuffer(RenderDevice& dev, const RenderTexture* t) {
    const RenderTexture* a[1] = {t};
    RenderFramebufferDesc d(a, 1);
    return dev.createFramebuffer(d);
}

void DrawState(RenderCommandList* l, const RenderPipeline* p, const RenderPipelineLayout* pl, RenderDescriptorSet* set) {
    l->setPipeline(p);
    l->setGraphicsPipelineLayout(pl);
    if (set != nullptr) {
        l->setGraphicsDescriptorSet(set, 0);
    }
    l->setViewports(RenderViewport(0, 0, float(kW), float(kH)));
    l->setScissors(RenderRect(0, 0, int32_t(kW), int32_t(kH)));
}

struct Scene {
    RenderDevice& dev;
    std::unique_ptr<RenderTexture> X, Y;
    std::unique_ptr<RenderFramebuffer> fbX, fbY;
    std::unique_ptr<RenderShader> triVs, triFs, fsVs, smpFs;
    std::unique_ptr<RenderPipelineLayout> L1;
    Set1 S2;
    std::unique_ptr<RenderSampler> smp;
    std::unique_ptr<RenderPipeline> P1, P2;

    explicit Scene(RenderDevice& d) : dev(d) {
        X = MakeTarget(dev);
        Y = MakeTarget(dev);
        fbX = MakeFramebuffer(dev, X.get());
        fbY = MakeFramebuffer(dev, Y.get());
        triVs = MakeShader(dev, SPV(spv_tri_vert));
        triFs = MakeShader(dev, SPV(spv_tri_frag));
        fsVs = MakeShader(dev, SPV(spv_fs_vert));
        smpFs = MakeShader(dev, SPV(spv_sample_frag));
        RenderPipelineLayoutDesc empty;
        L1 = dev.createPipelineLayout(empty);
        S2.Build(dev, {RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
                       RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1)});
        RenderSamplerDesc sd;
        sd.minFilter = sd.magFilter = RenderFilter::NEAREST;
        sd.mipmapMode = RenderMipmapMode::NEAREST;
        sd.addressU = sd.addressV = sd.addressW = RenderTextureAddressMode::CLAMP;
        smp = dev.createSampler(sd);
        S2.set->setTexture(0, X.get(), RenderTextureLayout::SHADER_READ);
        S2.set->setSampler(1, smp.get());
        P1 = MakePipeline(dev, L1.get(), triVs.get(), triFs.get());
        P2 = MakePipeline(dev, S2.layout.get(), fsVs.get(), smpFs.get());
    }

    bool Valid() const {
        return static_cast<VulkanFramebuffer*>(fbX.get())->vk != VK_NULL_HANDLE &&
               static_cast<VulkanFramebuffer*>(fbY.get())->vk != VK_NULL_HANDLE &&
               static_cast<VulkanGraphicsPipeline*>(P1.get())->vk != VK_NULL_HANDLE &&
               static_cast<VulkanGraphicsPipeline*>(P2.get())->vk != VK_NULL_HANDLE;
    }

    // Draw the triangle into X (cleared blue first), then move X to SHADER_READ at the given stages.
    void RecordProduce(RenderCommandList* l, RenderBarrierStages toRead) {
        l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(X.get(), RenderTextureLayout::COLOR_WRITE));
        l->setFramebuffer(fbX.get());
        DrawState(l, P1.get(), L1.get(), nullptr);
        l->clearColor(0, RenderColor(0.0f, 0.0f, 1.0f, 1.0f));
        l->drawInstanced(3, 1, 0, 0);
        l->barriers(toRead, RenderTextureBarrier(X.get(), RenderTextureLayout::SHADER_READ));
    }

    // A full-screen triangle into Y that samples X.
    void RecordConsume(RenderCommandList* l) {
        l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(Y.get(), RenderTextureLayout::COLOR_WRITE));
        l->setFramebuffer(fbY.get());
        DrawState(l, P2.get(), S2.layout.get(), S2.set.get());
        l->drawInstanced(3, 1, 0, 0);
    }
};

void SubmitAndWait(RenderDevice& dev, RenderCommandQueue* q, RenderCommandList* l) {
    auto f = dev.createCommandFence();
    q->executeCommandLists(l, f.get());
    q->waitForCommandFence(f.get());
}

std::vector<uint8_t> ReadTexture(RenderDevice& dev, RenderCommandQueue* q, RenderTexture* t) {
    auto rb = dev.createBuffer(RenderBufferDesc::ReadbackBuffer(kW * kH * 4));
    auto l = q->createCommandList();
    l->begin();
    l->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(t, RenderTextureLayout::COPY_SOURCE));
    l->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(rb.get(), RenderFormat::R8G8B8A8_UNORM, kW, kH, 1, kW),
                         RenderTextureCopyLocation::Subresource(t));
    l->end();
    SubmitAndWait(dev, q, l.get());
    std::vector<uint8_t> v(kW * kH * 4);
    std::memcpy(v.data(), rb->map(), v.size());
    rb->unmap();
    return v;
}

bool Near1(int a, int b) { return a - b <= 1 && b - a <= 1; }

// The "triangle then clear" image: the upper-left half is (64,128,191,255), the rest the clear colour (0,0,255,255);
// the pixels on the diagonal are not looked at.
int WrongTrianglePixels(const std::vector<uint8_t>& x) {
    int bad = 0;
    for (uint32_t py = 0; py < kH; py++) {
        for (uint32_t px = 0; px < kW; px++) {
            const uint8_t* p = &x[(py * kW + px) * 4];
            if (px + py + 1 == kW) {
                continue;
            }
            const bool inside = (px + py + 1 < kW);
            const int e[4] = {inside ? 64 : 0, inside ? 128 : 0, inside ? 191 : 255, 255};
            for (int c = 0; c < 4; c++) {
                if (!Near1(p[c], e[c])) {
                    bad++;
                    break;
                }
            }
        }
    }
    return bad;
}

// ---- volk-level trace of vkQueueSubmit (as 6.9 did: plume calls Vulkan through volk's function pointers) ------------

struct SubmitTrace {
    std::mutex mutex;
    std::vector<std::vector<VkPipelineStageFlags>> waitMasks;  // one entry per submit with two or more waits
    PFN_vkQueueSubmit real = nullptr;
} gTrace;

VkResult VKAPI_CALL TraceQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    {
        std::lock_guard<std::mutex> lock(gTrace.mutex);
        for (uint32_t i = 0; i < n; i++) {
            if (s[i].waitSemaphoreCount >= 2) {
                gTrace.waitMasks.emplace_back(s[i].pWaitDstStageMask, s[i].pWaitDstStageMask + s[i].waitSemaphoreCount);
            }
        }
    }
    return gTrace.real(q, n, s, f);
}

struct TraceGuard {
    TraceGuard() {
        gTrace.waitMasks.clear();
        gTrace.real = vkQueueSubmit;
        vkQueueSubmit = TraceQueueSubmit;
    }
    ~TraceGuard() { vkQueueSubmit = gTrace.real; }
};

}  // namespace

TEST_CASE("vendor/info") {
    const auto& info = mh4n::gpu::backend::plume::VendorInfo();
    CHECK(info.commit == "0ce184b19270e49236afbb74052a6b267494b86b");
    REQUIRE(info.patches.size() >= 11);
    CHECK(std::string(info.patches[0]).rfind("01-", 0) == 0);
    CHECK(std::string(info.patches[10]).rfind("11-", 0) == 0);
}

TEST_CASE("device/no_surface") {
    Rig rig;
    if (!DeviceOk(rig.Open({}), rig.error, "device/no_surface")) {
        return;
    }
    LogDevice(rig);
    const std::vector<std::string>& instanceNames = rig.vi->enabledExtensionNames;
    const std::vector<std::string>& deviceNames = rig.vd->enabledExtensionNames;
    for (const auto* names : {&instanceNames, &deviceNames}) {
        CHECK_FALSE(AnyContains(*names, "surface"));
        CHECK_FALSE(Has(*names, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        CHECK_FALSE(Has(*names, VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
        CHECK_FALSE(AnyContains(*names, "ray_tracing"));
        CHECK_FALSE(AnyContains(*names, "acceleration_structure"));
        CHECK_FALSE(AnyContains(*names, "pipeline_library"));
        CHECK_FALSE(AnyContains(*names, "deferred_host_operations"));
    }
    // the presentation extensions need a surface
    CHECK_FALSE(Has(deviceNames, VK_KHR_PRESENT_ID_EXTENSION_NAME));
    CHECK_FALSE(Has(deviceNames, VK_KHR_PRESENT_WAIT_EXTENSION_NAME));
    CHECK_FALSE(Has(deviceNames, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME));
    CHECK_FALSE(rig.vd->capabilities.raytracing);
    CHECK_FALSE(rig.vd->capabilities.presentWait);
}

TEST_CASE("device/core_features") {
    // An empty core-feature list enables nothing but robustBufferAccess, which robustness2's robustBufferAccess2
    // needs (VUID-VkPhysicalDeviceRobustness2FeaturesEXT-robustBufferAccess2-04000): the validation layer
    // (tools/native/vk_layer_run.py) reports it when the device is created without it.
    VulkanDeviceExtras extras;
    extras.coreFeatures = VkPhysicalDeviceFeatures{};
    Rig rig;
    if (!DeviceOk(rig.Open({}, extras), rig.error, "device/core_features")) {
        return;
    }
    LogDevice(rig);
    VkPhysicalDeviceFeatures expected = {};
    VkPhysicalDeviceFeatures got = rig.vd->enabledCoreFeatures;
    const bool robust = got.robustBufferAccess == VK_TRUE;
    got.robustBufferAccess = VK_FALSE;
    CHECK(std::memcmp(&got, &expected, sizeof(got)) == 0);  // nothing else is on
    CHECK_FALSE(rig.vd->capabilities.geometryShader);
    if (Has(rig.vd->enabledExtensionNames, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) && rig.vd->nullDescriptorSupported) {
        CHECK(robust);
    }
}

TEST_CASE("device/headless_surface") {
    VulkanInterfaceOptions options;
    options.headlessSurface = true;
    options.debugUtils = true;  // also shows the names are read back from what was really enabled
    Rig probe;
    if (!DeviceOk(probe.Open(options), probe.error, "device/headless_surface")) {
        return;
    }
    if (!Has(probe.vi->enabledExtensionNames, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) {
        if (HeadlessSurfaceRequired()) {
            FAIL("the loader does not offer VK_EXT_headless_surface and MH4N_REQUIRE_HEADLESS_SURFACE=1");
        }
        WARN_MESSAGE(false, "skipped: the loader does not offer VK_EXT_headless_surface");
        return;
    }
    CHECK(Has(probe.vi->enabledExtensionNames, VK_KHR_SURFACE_EXTENSION_NAME));
    CHECK(Has(probe.vi->enabledExtensionNames, VK_EXT_DEBUG_UTILS_EXTENSION_NAME));

    VkHeadlessSurfaceCreateInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    REQUIRE(vkCreateHeadlessSurfaceEXT(probe.vi->instance, &info, nullptr, &surface) == VK_SUCCESS);

    VulkanDeviceExtras extras;
    extras.surface = surface;
    std::string error;
    extras.error = &error;
    auto dev = probe.vi->createDeviceEx("", extras);
    REQUIRE_MESSAGE(dev != nullptr, error);
    auto* vd = static_cast<VulkanDevice*>(dev.get());
    CHECK(Has(vd->enabledExtensionNames, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
    VkBool32 canPresent = VK_FALSE;
    REQUIRE(vkGetPhysicalDeviceSurfaceSupportKHR(vd->physicalDevice, vd->queueFamilyIndices[0], surface, &canPresent) == VK_SUCCESS);
    CHECK(canPresent == VK_TRUE);

    dev.reset();
    vkDestroySurfaceKHR(probe.vi->instance, surface, nullptr);
}

TEST_CASE("device/chosen") {
    Rig rig;
    VulkanInterfaceOptions options;
    std::string error;
    options.error = &error;
    rig.iface = CreateVulkanInterface(options);
    if (!DeviceOk(rig.iface != nullptr, error, "device/chosen")) {
        return;
    }
    rig.vi = static_cast<VulkanInterface*>(rig.iface.get());
    uint32_t count = 0;
    REQUIRE(vkEnumeratePhysicalDevices(rig.vi->instance, &count, nullptr) == VK_SUCCESS);
    if (!DeviceOk(count > 0, "the instance lists no physical device", "device/chosen")) {
        return;
    }
    std::vector<VkPhysicalDevice> devices(count);
    REQUIRE(vkEnumeratePhysicalDevices(rig.vi->instance, &count, devices.data()) == VK_SUCCESS);

    VulkanDeviceExtras extras;
    extras.physicalDeviceIndex = int32_t(count - 1);  // the last one this test enumerates
    std::string deviceError;
    extras.error = &deviceError;
    auto dev = rig.vi->createDeviceEx("", extras);
    REQUIRE_MESSAGE(dev != nullptr, deviceError);
    CHECK(static_cast<VulkanDevice*>(dev.get())->physicalDevice == devices[count - 1]);
    std::printf("rhi_plume_test: device: %s (index %u of %u)\n", dev->getDescription().name.c_str(), count - 1, count);
    std::fflush(stdout);
    dev.reset();

    // an index past the end fails with a message, and the process goes on
    VulkanDeviceExtras past;
    past.physicalDeviceIndex = int32_t(count);
    std::string pastError;
    past.error = &pastError;
    std::fflush(stderr);
    CHECK(rig.vi->createDeviceEx("", past) == nullptr);
    CHECK_FALSE(pastError.empty());
}

TEST_CASE("sync/A1_render_then_sample") {
    Rig rig;
    if (!DeviceOk(rig.Open({}), rig.error, "sync/A1")) {
        return;
    }
    LogDevice(rig);
    Scene s(*rig.dev);
    REQUIRE(s.Valid());
    auto q = rig.dev->createCommandQueue(RenderCommandListType::DIRECT);
    auto l = q->createCommandList();
    l->begin();
    s.RecordProduce(l.get(), RenderBarrierStage::GRAPHICS);
    s.RecordConsume(l.get());
    l->end();
    SubmitAndWait(*rig.dev, q.get(), l.get());
    const auto x = ReadTexture(*rig.dev, q.get(), s.X.get());
    const auto y = ReadTexture(*rig.dev, q.get(), s.Y.get());
    CHECK(WrongTrianglePixels(x) == 0);
    CHECK(x == y);
}

TEST_CASE("sync/C3_two_waits_each_with_a_stage") {
    Rig rig;
    if (!DeviceOk(rig.Open({}), rig.error, "sync/C3")) {
        return;
    }
    LogDevice(rig);
    Scene s(*rig.dev);
    REQUIRE(s.Valid());
    RenderDevice& dev = *rig.dev;
    auto q1 = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto q2 = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto s1 = dev.createCommandSemaphore();
    auto s2 = dev.createCommandSemaphore();
    auto f = dev.createCommandFence();

    // submission 0: an independent producer, Z cleared on q1, signals s2
    auto Z = MakeTarget(dev);
    auto fbZ = MakeFramebuffer(dev, Z.get());
    auto lz = q1->createCommandList();
    lz->begin();
    lz->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(Z.get(), RenderTextureLayout::COLOR_WRITE));
    lz->setFramebuffer(fbZ.get());
    lz->clearColor(0, RenderColor(1, 0, 0, 1));
    lz->end();
    RenderCommandSemaphore* sig2[1] = {s2.get()};
    const RenderCommandList* zl[1] = {lz.get()};
    q1->executeCommandLists(zl, 1, nullptr, 0, sig2, 1, nullptr);

    // submission 1: X rendered on q1, signals s1
    auto l1 = q1->createCommandList();
    auto l2 = q2->createCommandList();
    l1->begin();
    s.RecordProduce(l1.get(), RenderBarrierStage::GRAPHICS);
    l1->end();
    l2->begin();
    s.RecordConsume(l2.get());
    l2->end();
    RenderCommandSemaphore* sig1[1] = {s1.get()};
    const RenderCommandList* a[1] = {l1.get()};
    q1->executeCommandLists(a, 1, nullptr, 0, sig1, 1, nullptr);

    // submission 2 on q2: waits on both, each with its own stage mask
    TraceGuard trace;
    RenderCommandSemaphore* waits[2] = {s1.get(), s2.get()};
    const RenderBarrierStages stages[2] = {RenderBarrierStage::GRAPHICS, RenderBarrierStage::COMPUTE};
    const RenderCommandList* b[1] = {l2.get()};
    q2->executeCommandListsEx(b, 1, waits, 2, stages, nullptr, nullptr, 0, nullptr, f.get());
    q2->waitForCommandFence(f.get());

    {
        std::lock_guard<std::mutex> lock(gTrace.mutex);
        REQUIRE(gTrace.waitMasks.size() == 1);
        const auto& masks = gTrace.waitMasks[0];
        REQUIRE(masks.size() == 2);
        // each wait has its own mask: the first blocks the fragment shader (the consumer), the second compute only
        CHECK((masks[0] & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) != 0);
        CHECK(masks[1] == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        CHECK(masks[0] != masks[1]);
    }
    const auto x = ReadTexture(dev, q2.get(), s.X.get());
    const auto y = ReadTexture(dev, q2.get(), s.Y.get());
    CHECK(WrongTrianglePixels(x) == 0);
    CHECK(x == y);
}

// A uniform buffer can be bound at a non-zero byte offset through the structured view of setBuffer (the code
// reading is in docs/native/rhi.md, "Binding a buffer at an offset"): the offset is firstElement * stride and the
// range is the explicit bufferSize. N2-27's per-draw sets rely on it.
TEST_CASE("descriptors/buffer_offset") {
    Rig rig;
    if (!DeviceOk(rig.Open({}), rig.error, "descriptors/buffer_offset")) {
        return;
    }
    LogDevice(rig);
    RenderDevice& dev = *rig.dev;
    const uint64_t alignment = std::max<uint64_t>(rig.vd->physicalDeviceProperties.limits.minUniformBufferOffsetAlignment, 16);
    const uint64_t offsets[2] = {alignment * 2, alignment * 5};
    const uint64_t size = alignment * 8;

    auto ubo = dev.createBuffer(RenderBufferDesc::UploadBuffer(size, RenderBufferFlag::CONSTANT));
    {
        auto* bytes = static_cast<uint8_t*>(ubo->map());
        std::memset(bytes, 0xEE, size);  // everything outside the two blocks is a recognisable filler
        const uint32_t first[4] = {7, 8, 9, 10};
        const uint32_t second[4] = {21, 22, 23, 24};
        std::memcpy(bytes + offsets[0], first, 16);
        std::memcpy(bytes + offsets[1], second, 16);
        ubo->unmap();
    }
    auto out = dev.createBuffer(RenderBufferDesc::ReadbackBuffer(16, RenderBufferFlag::STORAGE));

    Set1 set;
    set.Build(dev, {RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, 0, 1),
                    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER, 1, 1)});
    auto cs = MakeShader(dev, SPV(spv_ubo_offset_comp));
    auto pipeline = dev.createComputePipeline(RenderComputePipelineDesc(set.layout.get(), cs.get(), 1, 1, 1));
    REQUIRE(static_cast<VulkanComputePipeline*>(pipeline.get())->vk != VK_NULL_HANDLE);
    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);

    const uint32_t expected[2][4] = {{7, 8, 9, 10}, {21, 22, 23, 24}};
    for (int i = 0; i < 2; i++) {
        // offset = firstElement * structureByteStride (stride 1: bytes); range = bufferSize, the 16 bytes of the block
        const RenderBufferStructuredView view(1, uint32_t(offsets[i]));
        set.set->setBuffer(0, ubo.get(), 16, &view);
        set.set->setBuffer(1, out.get());
        auto l = q->createCommandList();
        l->begin();
        l->setPipeline(pipeline.get());
        l->setComputePipelineLayout(set.layout.get());
        l->setComputeDescriptorSet(set.set.get(), 0);
        l->dispatch(1, 1, 1);
        l->end();
        SubmitAndWait(dev, q.get(), l.get());  // the set is rewritten only when no submission uses it
        uint32_t got[4] = {};
        std::memcpy(got, out->map(), 16);
        out->unmap();
        CHECK(std::memcmp(got, expected[i], 16) == 0);
    }
}

// A control for the layer harness: the consumer waits on a timeline value at the default stage
// (COLOR_ATTACHMENT_OUTPUT) while the producer's last barrier covers no stage. The pixels are right on lavapipe;
// the validation layer must report SYNC-HAZARD-READ-AFTER-WRITE.
TEST_CASE("control/C5" * doctest::skip()) {
    Rig rig;
    if (!DeviceOk(rig.Open({}), rig.error, "control/C5")) {
        return;
    }
    if (!rig.vd->capabilities.timelineSemaphore) {
        WARN_MESSAGE(false, "skipped: no timeline semaphores");
        return;
    }
    LogDevice(rig);
    Scene s(*rig.dev);
    REQUIRE(s.Valid());
    RenderDevice& dev = *rig.dev;
    auto q1 = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto q2 = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto f = dev.createCommandFence();
    auto l1 = q1->createCommandList();
    auto l2 = q2->createCommandList();
    l1->begin();
    s.RecordProduce(l1.get(), RenderBarrierStage::NONE);
    l1->end();
    l2->begin();
    s.RecordConsume(l2.get());
    l2->end();
    auto timeline = dev.createTimelineSemaphore(0);
    REQUIRE(timeline != nullptr);
    RenderCommandSemaphore* ts[1] = {timeline.get()};
    const uint64_t one = 1;
    const RenderCommandList* a[1] = {l1.get()};
    const RenderCommandList* b[1] = {l2.get()};
    q1->executeCommandListsEx(a, 1, nullptr, 0, nullptr, nullptr, ts, 1, &one, nullptr);
    q2->executeCommandListsEx(b, 1, ts, 1, nullptr, &one, nullptr, 0, nullptr, f.get());
    q2->waitForCommandFence(f.get());
    CHECK(true);
}
