// The validation patterns of docs/research/rhi-spike-evidence/test/main.cpp (this repository's code, GPL-3.0-or-later)
// ported as tests of the patched plume (N2-29), on its public API, with the result `results-summary.txt` gives for the
// patched build (label "patched"); plus one case for each patch that no pattern covers (10 dynamic stencil state, 11
// D24S8 and the 16-bit colour formats, 12 custom border colours) and for patch 09's optional-extension lists.
//
// native/tests/rhi_plume_test.cpp (N2-25) holds A1, C3, C5 and the device cases; this file holds the rest: A2, B1 to B5,
// B8, C1, C2, C4, D1, D3, E and F, and the controls. The shaders are N2-25's `rhi_plume_shaders.inc` (included, never
// edited); the one shader this file needs and that file lacks (border.frag) is an array below, with its source.
//
// Each case runs on whatever Vulkan device the machine has (lavapipe in the cloud and in Linux CI) and skips with a
// message when there is none; with MH4N_REQUIRE_VULKAN=1 it fails instead. Lavapipe accepts every invalid call, so the
// cases are also run under the validation layer: `python3 tools/native/vk_layer_run.py --build build/native -R
// '^rhi_plume_patterns_test$'` (docs/native/rhi.md).
//
// The controls are cases named `control/<pattern>`, skipped unless named on the command line
// (`--test-case=control/B7 --no-skip`): B7 must make the layer report SYNC-HAZARD-WRITE-AFTER-WRITE, D4
// UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout, D2 VUID-VkImageMemoryBarrier-oldLayout-01197 and
// InvalidImageLayout (counts vary per run) and a ThreadSanitizer report; B6 is the example of a hazard the layer misses
// (storage-image writes between draws in one command buffer: no message, 6.9) and nothing asserts on it.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gpu/backend/plume/vendor_info.h"
#include "plume_vulkan.h"
#include "support/vulkan_device.h"

#include "rhi_plume_shaders.inc"

using namespace plume;

namespace {

constexpr uint32_t kW = 16;
constexpr uint32_t kH = 16;

#define SPV(a) (const void*)(a), sizeof(a)

// border.frag, compiled with `glslangValidator -V --target-env vulkan1.2` (glslang 15.1.0) and printed by
// `python3 tools/native/spirv_inc.py`. It lives here because the allowlist of N2-29 has no shader folder:
//
//   #version 450
//   layout(set=0, binding=0) uniform texture2D tex;
//   layout(set=0, binding=1) uniform sampler smp;
//   layout(location=0) in vec2 uv;
//   layout(location=0) out vec4 o;
//   void main(){ o = texture(sampler2D(tex, smp), uv * 3.0 - 1.0); }
//
// fs.vert's uv runs over 0..1 across the viewport, so the texture's own 0..1 covers the middle sixth to two thirds of
// it and the rest is out of range.
[[maybe_unused]] const uint32_t spv_border_frag[] = {
    0x07230203, 0x00010500, 0x0008000b, 0x0000001e, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0009000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000c, 0x00000010,
    0x00000016, 0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000009, 0x0000006f, 0x00030005, 0x0000000c,
    0x00786574, 0x00030005, 0x00000010, 0x00706d73, 0x00030005, 0x00000016, 0x00007675, 0x00040047,
    0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000c, 0x00000021, 0x00000000, 0x00040047,
    0x0000000c, 0x00000022, 0x00000000, 0x00040047, 0x00000010, 0x00000021, 0x00000001, 0x00040047,
    0x00000010, 0x00000022, 0x00000000, 0x00040047, 0x00000016, 0x0000001e, 0x00000000, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017,
    0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b,
    0x00000008, 0x00000009, 0x00000003, 0x00090019, 0x0000000a, 0x00000006, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00040020, 0x0000000b, 0x00000000, 0x0000000a,
    0x0004003b, 0x0000000b, 0x0000000c, 0x00000000, 0x0002001a, 0x0000000e, 0x00040020, 0x0000000f,
    0x00000000, 0x0000000e, 0x0004003b, 0x0000000f, 0x00000010, 0x00000000, 0x0003001b, 0x00000012,
    0x0000000a, 0x00040017, 0x00000014, 0x00000006, 0x00000002, 0x00040020, 0x00000015, 0x00000001,
    0x00000014, 0x0004003b, 0x00000015, 0x00000016, 0x00000001, 0x0004002b, 0x00000006, 0x00000018,
    0x40400000, 0x0004002b, 0x00000006, 0x0000001a, 0x3f800000, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c,
    0x0004003d, 0x0000000e, 0x00000011, 0x00000010, 0x00050056, 0x00000012, 0x00000013, 0x0000000d,
    0x00000011, 0x0004003d, 0x00000014, 0x00000017, 0x00000016, 0x0005008e, 0x00000014, 0x00000019,
    0x00000017, 0x00000018, 0x00050050, 0x00000014, 0x0000001b, 0x0000001a, 0x0000001a, 0x00050083,
    0x00000014, 0x0000001c, 0x00000019, 0x0000001b, 0x00050057, 0x00000007, 0x0000001d, 0x00000013,
    0x0000001c, 0x0003003e, 0x00000009, 0x0000001d, 0x000100fd, 0x00010038,
};

bool VulkanRequired() {
    const char* value = std::getenv("MH4N_REQUIRE_VULKAN");
    return value != nullptr && std::string(value) == "1";
}

// A plume instance and device, or a reason there is none (duplicated from rhi_plume_test.cpp: the helpers are not shared).
struct Rig {
    std::unique_ptr<RenderInterface> iface;
    std::unique_ptr<RenderDevice> dev;
    VulkanInterface* vi = nullptr;
    VulkanDevice* vd = nullptr;
    std::string error;

    bool Open(const VulkanInterfaceOptions& options = {}, const VulkanDeviceExtras& extras = {}) {
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

    bool IsLavapipe() const { return vd != nullptr && vd->description.name.find("llvmpipe") != std::string::npos; }
};

// true: go on. false: no device and not required: the case has been skipped with a message.
bool DeviceOk(bool ok, const std::string& error, const char* what) {
    if (ok) {
        return true;
    }
    if (VulkanRequired()) {
        FAIL(std::string(what) << ": no Vulkan device and MH4N_REQUIRE_VULKAN=1: " << error);
    }
    mh4n::test::SkipCase(std::string(what) + ": no Vulkan device (" + error + ")");
    return false;
}

// The line the Done-when of N2-29 looks for: the log names the device the cases ran on.
void LogDevice(const Rig& rig, const char* what) {
    std::printf("rhi_plume_patterns_test: %s: device: %s\n", what, rig.vd->description.name.c_str());
    std::fflush(stdout);
}

// A capability the case needs. A device without it skips the case; lavapipe has all of the ones asked here, so under
// MH4N_REQUIRE_VULKAN=1 its absence on lavapipe fails (a skip there would hide a regression).
bool CapabilityOk(const Rig& rig, bool has, const char* capability, const char* what) {
    if (has) {
        return true;
    }
    if (VulkanRequired() && rig.IsLavapipe()) {
        FAIL(std::string(what) << ": lavapipe does not report " << std::string(capability) << " and MH4N_REQUIRE_VULKAN=1");
    }
    mh4n::test::SkipCase(std::string(what) + ": the device has no " + std::string(capability));
    return false;
}

bool Has(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// ---- the evidence's helpers and scene (main.cpp) -------------------------------------------------------------------

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

std::unique_ptr<RenderPipelineLayout> EmptyLayout(RenderDevice& dev) {
    RenderPipelineLayoutDesc ld;
    return dev.createPipelineLayout(ld);
}

std::unique_ptr<RenderPipeline> MakePipeline(RenderDevice& dev, const RenderPipelineLayout* layout, const RenderShader* vs,
                                             const RenderShader* fs, uint32_t renderTargets,
                                             RenderFormat format = RenderFormat::R8G8B8A8_UNORM,
                                             RenderBlendDesc blend = RenderBlendDesc::Copy()) {
    RenderGraphicsPipelineDesc d;
    d.pipelineLayout = layout;
    d.vertexShader = vs;
    d.pixelShader = fs;
    d.renderTargetCount = renderTargets;
    if (renderTargets != 0) {
        d.renderTargetFormat[0] = format;
        d.renderTargetBlend[0] = blend;
    }
    d.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    d.cullMode = RenderCullMode::NONE;
    return dev.createGraphicsPipeline(d);
}

std::unique_ptr<RenderTexture> MakeTarget(RenderDevice& dev, RenderFormat format = RenderFormat::R8G8B8A8_UNORM,
                                          RenderTextureFlags extra = 0) {
    return dev.createTexture(RenderTextureDesc::ColorTarget(kW, kH, format, RenderMultisampling(), nullptr, extra));
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

VkImage VkImageOf(const RenderTexture* t) { return static_cast<const VulkanTexture*>(t)->vk; }

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
        L1 = EmptyLayout(dev);
        S2.Build(dev, {RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
                       RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1)});
        RenderSamplerDesc sd;
        sd.minFilter = sd.magFilter = RenderFilter::NEAREST;
        sd.mipmapMode = RenderMipmapMode::NEAREST;
        sd.addressU = sd.addressV = sd.addressW = RenderTextureAddressMode::CLAMP;
        smp = dev.createSampler(sd);
        S2.set->setTexture(0, X.get(), RenderTextureLayout::SHADER_READ);
        S2.set->setSampler(1, smp.get());
        P1 = MakePipeline(dev, L1.get(), triVs.get(), triFs.get(), 1);
        P2 = MakePipeline(dev, S2.layout.get(), fsVs.get(), smpFs.get(), 1);
    }

    bool Valid() const {
        return static_cast<VulkanFramebuffer*>(fbX.get())->vk != VK_NULL_HANDLE &&
               static_cast<VulkanFramebuffer*>(fbY.get())->vk != VK_NULL_HANDLE &&
               static_cast<VulkanGraphicsPipeline*>(P1.get())->vk != VK_NULL_HANDLE &&
               static_cast<VulkanGraphicsPipeline*>(P2.get())->vk != VK_NULL_HANDLE;
    }

    // Draw the triangle into X (cleared blue first), then move X to SHADER_READ at the given stages.
    void RecordProduce(RenderCommandList* l, RenderBarrierStages toRead = RenderBarrierStage::GRAPHICS) {
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

// The bytes of a texture, copied through a readback buffer in `format` (the texture is moved to COPY_SOURCE).
std::vector<uint8_t> ReadRaw(RenderDevice& dev, RenderCommandQueue* q, RenderTexture* t, RenderFormat format, size_t bytes) {
    auto rb = dev.createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
    auto l = q->createCommandList();
    l->begin();
    l->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(t, RenderTextureLayout::COPY_SOURCE));
    l->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(rb.get(), format, kW, kH, 1, kW),
                         RenderTextureCopyLocation::Subresource(t));
    l->end();
    SubmitAndWait(dev, q, l.get());
    std::vector<uint8_t> v(bytes);
    std::memcpy(v.data(), rb->map(), bytes);
    rb->unmap();
    return v;
}

std::vector<uint8_t> ReadTexture(RenderDevice& dev, RenderCommandQueue* q, RenderTexture* t) {
    return ReadRaw(dev, q, t, RenderFormat::R8G8B8A8_UNORM, kW * kH * 4);
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

// The two checks of every A and C pattern: X is the triangle image and Y, drawn by sampling X, is the same bytes.
void CheckXandY(RenderDevice& dev, RenderCommandQueue* q, Scene& s) {
    const auto x = ReadTexture(dev, q, s.X.get());
    const auto y = ReadTexture(dev, q, s.Y.get());
    CHECK(WrongTrianglePixels(x) == 0);
    CHECK(x == y);
}

// ---- volk-level traces (as 6.9 did: plume calls Vulkan through volk's global function pointers) --------------------
//
// One trace at a time, installed for the length of a case by a guard. The hooks only count or record; they change nothing
// that is passed on.

struct VkTrace {
    std::mutex mutex;
    // E: what the 42 pipelines cost
    int renderPasses = 0;
    int pipelineCreateCalls = 0;
    int pipelinesCreated = 0;
    int pipelinesWithoutCache = 0;
    // D: the (old, new) layout of every image barrier on `watched`, per command buffer, in recording order
    VkImage watched = VK_NULL_HANDLE;
    std::map<VkCommandBuffer, std::vector<std::pair<VkImageLayout, VkImageLayout>>> barriers;

    PFN_vkCreateRenderPass realCreateRenderPass = nullptr;
    PFN_vkCreateGraphicsPipelines realCreateGraphicsPipelines = nullptr;
    PFN_vkCmdPipelineBarrier realCmdPipelineBarrier = nullptr;
};

VkTrace gTrace;

VkResult VKAPI_CALL TraceCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a, VkRenderPass* rp) {
    {
        std::lock_guard<std::mutex> lock(gTrace.mutex);
        gTrace.renderPasses++;
    }
    return gTrace.realCreateRenderPass(d, ci, a, rp);
}

VkResult VKAPI_CALL TraceCreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                                 const VkAllocationCallbacks* a, VkPipeline* p) {
    {
        std::lock_guard<std::mutex> lock(gTrace.mutex);
        gTrace.pipelineCreateCalls++;
        gTrace.pipelinesCreated += int(n);
        if (c == VK_NULL_HANDLE) {
            gTrace.pipelinesWithoutCache += int(n);
        }
    }
    return gTrace.realCreateGraphicsPipelines(d, c, n, ci, a, p);
}

void VKAPI_CALL TraceCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags df,
                                        uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni,
                                        const VkImageMemoryBarrier* im) {
    {
        std::lock_guard<std::mutex> lock(gTrace.mutex);
        for (uint32_t i = 0; i < ni; i++) {
            if (im[i].image == gTrace.watched) {
                gTrace.barriers[cb].emplace_back(im[i].oldLayout, im[i].newLayout);
            }
        }
    }
    gTrace.realCmdPipelineBarrier(cb, src, dst, df, nm, m, nb, b, ni, im);
}

// Installs the hooks it is asked for and puts the pointers back when the case ends (volkLoadDevice, which a second
// device calls, may already have replaced them: then they are left alone).
struct TraceGuard {
    bool passes;
    bool barriers;
    TraceGuard(bool trackPassesAndPipelines, bool trackBarriers, VkImage watchedImage = VK_NULL_HANDLE)
        : passes(trackPassesAndPipelines), barriers(trackBarriers) {
        gTrace.renderPasses = gTrace.pipelineCreateCalls = gTrace.pipelinesCreated = gTrace.pipelinesWithoutCache = 0;
        gTrace.watched = watchedImage;
        gTrace.barriers.clear();
        if (passes) {
            gTrace.realCreateRenderPass = vkCreateRenderPass;
            gTrace.realCreateGraphicsPipelines = vkCreateGraphicsPipelines;
            vkCreateRenderPass = TraceCreateRenderPass;
            vkCreateGraphicsPipelines = TraceCreateGraphicsPipelines;
        }
        if (barriers) {
            gTrace.realCmdPipelineBarrier = vkCmdPipelineBarrier;
            vkCmdPipelineBarrier = TraceCmdPipelineBarrier;
        }
    }
    ~TraceGuard() {
        if (passes) {
            if (vkCreateRenderPass == TraceCreateRenderPass) {
                vkCreateRenderPass = gTrace.realCreateRenderPass;
            }
            if (vkCreateGraphicsPipelines == TraceCreateGraphicsPipelines) {
                vkCreateGraphicsPipelines = gTrace.realCreateGraphicsPipelines;
            }
        }
        if (barriers && vkCmdPipelineBarrier == TraceCmdPipelineBarrier) {
            vkCmdPipelineBarrier = gTrace.realCmdPipelineBarrier;
        }
    }
    TraceGuard(const TraceGuard&) = delete;
    TraceGuard& operator=(const TraceGuard&) = delete;
};

// ---- A2 ---------------------------------------------------------------------------------------------------------------

void RunA2(Rig& rig) {
    Scene s(*rig.dev);
    REQUIRE(s.Valid());
    auto q = rig.dev->createCommandQueue(RenderCommandListType::DIRECT);
    auto l1 = q->createCommandList();
    auto l2 = q->createCommandList();
    l1->begin();
    s.RecordProduce(l1.get());
    l1->end();
    l2->begin();
    s.RecordConsume(l2.get());
    l2->end();
    // list 1 (render X, X to SHADER_READ), then list 2 (render Y sampling X): two vkQueueSubmit calls on one queue
    q->executeCommandLists(l1.get(), nullptr);
    SubmitAndWait(*rig.dev, q.get(), l2.get());
    CheckXandY(*rig.dev, q.get(), s);
}

// ---- B1 to B7: the shadow write, three overlapping full-screen draws that each add 1 -----------------------------------

struct ShadowVariant {
    bool interlock = false;
    bool cas = false;
    bool barrierBetween = false;    // B5: a GENERAL barrier between the draws (ends the pass)
    bool restartNoBarrier = false;  // B6 (control): a new pass for each draw, no barrier
    bool twoLists = false;          // B7 (control): the last two draws in lists of their own, no barrier
    // B1: nothing orders the three read-modify-writes, so the shader guarantees only that every texel was written by 1 to 3 of
    // the draws (it started at 0); the count that reached 3 is logged, not asserted (lavapipe's serial execution gives all 256)
    bool unordered = false;
};

// Returns the number of texels that differ from the expected value (3, or 5 for B7; for `unordered`: the number outside 1 to 3),
// or -1 when the case was skipped.
int RunShadow(Rig& rig, const ShadowVariant& v, const char* name) {
    RenderDevice& dev = *rig.dev;
    if (v.interlock && !CapabilityOk(rig, dev.getCapabilities().fragmentShaderInterlock, "fragmentShaderInterlock", name)) {
        return -1;
    }
    // an RGBA8 colour target with storage usage and the mutable-format flag (patch 01), viewed as R32_UINT
    auto img = MakeTarget(dev, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::STORAGE | RenderTextureFlag::MUTABLE_FORMAT);
    REQUIRE(VkImageOf(img.get()) != VK_NULL_HANDLE);
    auto view = img->createTextureView(RenderTextureViewDesc::Texture2D(RenderFormat::R32_UINT));
    REQUIRE(static_cast<VulkanTextureView*>(view.get())->vk != VK_NULL_HANDLE);
    // a pass with no attachments (patch 01: the framebuffer takes its size from the description)
    RenderFramebufferDesc fbd;
    fbd.width = kW;
    fbd.height = kH;
    auto fb = dev.createFramebuffer(fbd);
    REQUIRE(static_cast<VulkanFramebuffer*>(fb.get())->vk != VK_NULL_HANDLE);

    std::unique_ptr<RenderShader> fs;
    if (v.cas) {
        fs = v.interlock ? MakeShader(dev, SPV(spv_shadow_cas_il)) : MakeShader(dev, SPV(spv_shadow_cas_nil));
    } else {
        fs = v.interlock ? MakeShader(dev, SPV(spv_shadow_store_il)) : MakeShader(dev, SPV(spv_shadow_store_nil));
    }
    auto vs = MakeShader(dev, SPV(spv_fs_vert));
    REQUIRE(static_cast<VulkanShader*>(fs.get())->vk != VK_NULL_HANDLE);
    Set1 set;
    set.Build(dev, {RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_TEXTURE, 0, 1)});
    set.set->setTexture(0, img.get(), RenderTextureLayout::GENERAL, view.get());
    auto pipeline = MakePipeline(dev, set.layout.get(), vs.get(), fs.get(), 0);  // zero colour attachments
    REQUIRE(static_cast<VulkanGraphicsPipeline*>(pipeline.get())->vk != VK_NULL_HANDLE);

    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto upload = dev.createBuffer(RenderBufferDesc::UploadBuffer(kW * kH * 4));
    std::memset(upload->map(), 0, kW * kH * 4);
    upload->unmap();
    auto l = q->createCommandList();
    l->begin();
    l->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(img.get(), RenderTextureLayout::COPY_DEST));
    l->copyTextureRegion(RenderTextureCopyLocation::Subresource(img.get()),
                         RenderTextureCopyLocation::PlacedFootprint(upload.get(), RenderFormat::R8G8B8A8_UNORM, kW, kH, 1, kW));
    l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(img.get(), RenderTextureLayout::GENERAL));
    l->setFramebuffer(fb.get());
    DrawState(l.get(), pipeline.get(), set.layout.get(), set.set.get());
    for (int i = 0; i < 3; i++) {
        if (i != 0 && v.barrierBetween) {
            l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(img.get(), RenderTextureLayout::GENERAL));  // ends the pass
            l->setFramebuffer(fb.get());
            DrawState(l.get(), pipeline.get(), set.layout.get(), set.set.get());
        }
        if (i != 0 && v.restartNoBarrier) {
            l->setFramebuffer(fb.get());  // ends the pass, starts another at the next draw, no barrier
            DrawState(l.get(), pipeline.get(), set.layout.get(), set.set.get());
        }
        l->drawInstanced(3, 1, 0, 0);
    }
    l->end();
    uint32_t expected = 3;
    if (v.twoLists) {
        // the list above holds the upload and three draws; draws 4 and 5 are in lists of their own with no barrier between
        // them and the first (the layer sees the write-after-write across two command buffers, 6.9)
        SubmitAndWait(dev, q.get(), l.get());
        auto la = q->createCommandList();
        auto lb = q->createCommandList();
        for (auto* x : {la.get(), lb.get()}) {
            x->begin();
            x->setFramebuffer(fb.get());
            DrawState(x, pipeline.get(), set.layout.get(), set.set.get());
            x->drawInstanced(3, 1, 0, 0);
            x->end();
        }
        q->executeCommandLists(la.get(), nullptr);
        SubmitAndWait(dev, q.get(), lb.get());
        expected = 5;
    } else {
        SubmitAndWait(dev, q.get(), l.get());
    }
    const auto out = ReadTexture(dev, q.get(), img.get());
    int bad = 0;
    int atExpected = 0;
    for (uint32_t i = 0; i < kW * kH; i++) {
        uint32_t value;
        std::memcpy(&value, &out[i * 4], 4);
        atExpected += value == expected ? 1 : 0;
        if (v.unordered ? (value < 1 || value > expected) : value != expected) {
            bad++;
        }
    }
    uint32_t texel0;
    std::memcpy(&texel0, &out[0], 4);
    if (v.unordered) {
        std::printf("rhi_plume_patterns_test: %s: %d of %u texels outside 1 to %u; %d of them hold %u (texel0 = %u)\n", name, bad, kW * kH,
                    expected, atExpected, expected, texel0);
    } else {
        std::printf("rhi_plume_patterns_test: %s: %d of %u texels != %u (texel0 = %u)\n", name, bad, kW * kH, expected, texel0);
    }
    std::fflush(stdout);
    return bad;
}

void RunShadowCase(const char* name, const ShadowVariant& v) {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, name)) {
        return;
    }
    LogDevice(rig, name);
    const int bad = RunShadow(rig, v, name);
    if (bad >= 0) {
        CHECK(bad == 0);
    }
}

// ---- B8: a colour attachment that stays in GENERAL (patch 04) ------------------------------------------------------------

void RunB8(Rig& rig) {
    RenderDevice& dev = *rig.dev;
    auto img = MakeTarget(dev, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::STORAGE);
    const RenderTexture* attachments[1] = {img.get()};
    RenderFramebufferDesc fd(attachments, 1);
    fd.colorAttachmentsGeneralLayout = true;
    auto fb = dev.createFramebuffer(fd);
    REQUIRE(static_cast<VulkanFramebuffer*>(fb.get())->vk != VK_NULL_HANDLE);
    auto triVs = MakeShader(dev, SPV(spv_tri_vert));
    auto triFs = MakeShader(dev, SPV(spv_tri_frag));
    auto L1 = EmptyLayout(dev);
    auto P1 = MakePipeline(dev, L1.get(), triVs.get(), triFs.get(), 1);
    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto l = q->createCommandList();
    l->begin();
    l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(img.get(), RenderTextureLayout::GENERAL));
    l->setFramebuffer(fb.get());
    DrawState(l.get(), P1.get(), L1.get(), nullptr);
    l->clearColor(0, RenderColor(0.0f, 0.0f, 1.0f, 1.0f));
    l->drawInstanced(3, 1, 0, 0);
    l->end();
    SubmitAndWait(dev, q.get(), l.get());
    CHECK(WrongTrianglePixels(ReadTexture(dev, q.get(), img.get())) == 0);
}

// ---- C1, C2, C4: a producer on one queue, a consumer on another --------------------------------------------------------

// C1: the producer ends with a GRAPHICS barrier; C2: with a NONE barrier, so only the semaphore carries the dependency and
// only the per-wait stage (patch 03) makes the fragment-shader sample safe. Both wait at GRAPHICS.
void RunBinaryHandOff(Rig& rig, RenderBarrierStages producerEnd) {
    RenderDevice& dev = *rig.dev;
    Scene s(dev);
    REQUIRE(s.Valid());
    auto q1 = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto q2 = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto sem = dev.createCommandSemaphore();
    auto f = dev.createCommandFence();
    auto l1 = q1->createCommandList();
    auto l2 = q2->createCommandList();
    l1->begin();
    s.RecordProduce(l1.get(), producerEnd);
    l1->end();
    l2->begin();
    s.RecordConsume(l2.get());
    l2->end();
    RenderCommandSemaphore* signal[1] = {sem.get()};
    RenderCommandSemaphore* wait[1] = {sem.get()};
    const RenderCommandList* a[1] = {l1.get()};
    const RenderCommandList* b[1] = {l2.get()};
    const RenderBarrierStages stages[1] = {RenderBarrierStage::GRAPHICS};
    q1->executeCommandLists(a, 1, nullptr, 0, signal, 1, nullptr);
    q2->executeCommandListsEx(b, 1, wait, 1, stages, nullptr, nullptr, 0, nullptr, f.get());
    q2->waitForCommandFence(f.get());
    CheckXandY(dev, q2.get(), s);
}

// C4: a timeline semaphore, the consumer waits for value 1 at GRAPHICS (C5, the default stage, is N2-25's control).
void RunTimelineHandOff(Rig& rig) {
    RenderDevice& dev = *rig.dev;
    Scene s(dev);
    REQUIRE(s.Valid());
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
    const RenderBarrierStages stages[1] = {RenderBarrierStage::GRAPHICS};
    const RenderCommandList* a[1] = {l1.get()};
    const RenderCommandList* b[1] = {l2.get()};
    q1->executeCommandListsEx(a, 1, nullptr, 0, nullptr, nullptr, ts, 1, &one, nullptr);
    q2->executeCommandListsEx(b, 1, ts, 1, stages, &one, nullptr, 0, nullptr, f.get());
    q2->waitForCommandFence(f.get());
    uint64_t value = 0;
    REQUIRE(vkGetSemaphoreCounterValue(static_cast<VulkanDevice*>(&dev)->vk, static_cast<VulkanCommandSemaphore*>(timeline.get())->vk, &value) ==
            VK_SUCCESS);
    CHECK(value == 1);
    CheckXandY(dev, q2.get(), s);
}

// ---- D1: thread 1 draws and submits, thread 2 waits for its fence, then samples ----------------------------------------------

void RunD1(Rig& rig) {
    RenderDevice& dev = *rig.dev;
    Scene s(dev);
    REQUIRE(s.Valid());
    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto f1 = dev.createCommandFence();
    auto f2 = dev.createCommandFence();
    std::mutex m;
    std::condition_variable cv;
    bool submitted = false;
    auto l1 = q->createCommandList();
    auto l2 = q->createCommandList();
    std::thread t1([&] {
        l1->begin();
        s.RecordProduce(l1.get());
        l1->end();
        q->executeCommandLists(l1.get(), f1.get());
        {
            std::lock_guard<std::mutex> g(m);
            submitted = true;
        }
        cv.notify_all();
    });
    std::thread t2([&] {
        {
            std::unique_lock<std::mutex> g(m);
            cv.wait(g, [&] { return submitted; });
        }
        q->waitForCommandFence(f1.get());
        l2->begin();
        s.RecordConsume(l2.get());
        l2->end();
        q->executeCommandLists(l2.get(), f2.get());
        q->waitForCommandFence(f2.get());
    });
    t1.join();
    t2.join();
    CheckXandY(dev, q.get(), s);
}

// ---- D2 and D3: two threads record barriers for one texture ------------------------------------------------------------------
//
// Two threads each record a list of 20 barriers on texture T, 2,000 rounds, in lists they then submit in the order 1, 2.
// plume keeps T's layout in the resource and reads and writes it while recording (6.9, D2), so without a rule the layouts
// recorded in the barriers are wrong. D3 is the owner rule of the harness in its tested form: whoever records barriers for T
// holds one mutex (per texture) for the whole list, and the lists are recorded in the order they are submitted (list 1,
// then list 2).
//
// What the case looks at, besides the validation layer (docs/native/rhi.md):
//  * `overwritten`: after each barrier the thread reads T's layout field; it differs from the one it just asked for when the
//    other thread's record-time store landed in between;
//  * `brokenChains`: the (old, new) layouts the barriers carry, taken from the vkCmdPipelineBarrier calls, must form one chain
//    in submission order (each barrier's old layout is the one before it left: list 1's 20 barriers, then list 2's 20, then the
//    next round's). A missing mutex shows in `overwritten` and in the chain; a wrong recording order shows in the chain only.
// Both must be 0 with the rule. Without it (control D2) they are not, and the layer reports oldLayout-01197 thousands of
// times and ThreadSanitizer the data race at plume_vulkan.cpp's layout store.
//
// Three rules are run. D2: none. D3: the harness's form, the mutex and thread 2 recording only after thread 1 has recorded;
// there either half alone already serialises the two threads, so removing the mutex from D3 shows nothing. D3b: the same rule
// with each half load-bearing: either thread may record first, the lists get a ticket under the mutex, and they are submitted
// in ticket order (thread 2 goes first in one round of four, thread 1 in one, in two they start together). The lists have an
// odd number of barriers, so list 1 ends in COLOR_WRITE and list 2 in GENERAL and the chain depends on the order (with an even
// number both end in SHADER_READ and it would not). Without the mutex the recordings overlap (D2's numbers); without the ticket
// order, lists recorded 2, 1 and submitted 1, 2 break the chain.

struct RaceResult {
    int overwrittenThread1 = 0;
    int overwrittenThread2 = 0;
    int brokenChains = 0;
    int emptyRounds = 0;
};

enum class Rule { None, Harness, Ticket };

RaceResult RunTwoThreadBarriers(Rig& rig, Rule rule, int rounds, int barriersPerList) {
    RenderDevice& dev = *rig.dev;
    auto T = MakeTarget(dev);
    auto* vt = static_cast<VulkanTexture*>(T.get());
    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto l1 = q->createCommandList();
    auto l2 = q->createCommandList();
    const VkCommandBuffer cb1 = static_cast<VulkanCommandList*>(l1.get())->vk;
    const VkCommandBuffer cb2 = static_cast<VulkanCommandList*>(l2.get())->vk;
    TraceGuard trace(false, true, vt->vk);

    std::atomic<int> go{0}, ready{0}, rec1{0}, done{0}, nextTicket{0};
    std::atomic<int> ticketOf[2] = {0, 0};
    std::atomic<int> overwritten1{0}, overwritten2{0};
    std::mutex ownerMutex;
    auto worker = [&](int id, RenderCommandList* l, RenderTextureLayout layout, std::atomic<int>& overwritten) {
        for (int it = 0; it < rounds; it++) {
            while (go.load(std::memory_order_acquire) != it + 1) {
                std::this_thread::yield();
            }
            ready.fetch_add(1);
            while (ready.load() < 2 * (it + 1)) {
                std::this_thread::yield();  // both threads start recording together
            }
            if (rule == Rule::Harness && id == 2) {
                while (rec1.load() <= it) {
                    std::this_thread::yield();  // list 2 is recorded after list 1: the order they are submitted in
                }
            }
            // rounds 1 mod 4: thread 1 starts late, so thread 2 takes the mutex first; 3 mod 4: thread 2 starts late; 0 and 2 mod 4:
            // both start together (without the mutex they overlap)
            if (rule == Rule::Ticket && (it & 3) != 0 && (it & 3) != 2 && id == (((it & 3) == 1) ? 1 : 2)) {
                for (int spin = 0; spin < 300; spin++) {
                    std::this_thread::yield();
                }
            }
            {
                std::unique_lock<std::mutex> guard(ownerMutex, std::defer_lock);
                if (rule != Rule::None) {
                    guard.lock();
                }
                if (rule == Rule::Ticket) {
                    ticketOf[id - 1].store(nextTicket.fetch_add(1));
                }
                l->begin();
                for (int k = 0; k < barriersPerList; k++) {
                    const RenderTextureLayout want = (k & 1) ? RenderTextureLayout::SHADER_READ : layout;
                    l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(T.get(), want));
                    if (vt->textureLayout != want) {
                        overwritten.fetch_add(1);  // another thread's record-time store landed in between
                    }
                }
                l->end();
            }
            if (id == 1) {
                rec1.store(it + 1);
            }
            done.fetch_add(1);
        }
    };
    std::thread t1(worker, 1, l1.get(), RenderTextureLayout::COLOR_WRITE, std::ref(overwritten1));
    std::thread t2(worker, 2, l2.get(), RenderTextureLayout::GENERAL, std::ref(overwritten2));

    RaceResult result;
    VkImageLayout previousEnd = VK_IMAGE_LAYOUT_UNDEFINED;
    bool first = true;
    for (int it = 0; it < rounds; it++) {
        nextTicket.store(0);
        go.store(it + 1, std::memory_order_release);
        while (done.load() < 2 * (it + 1)) {
            std::this_thread::yield();
        }
        // both lists are recorded: the layout chain of the submission order (list 1, then list 2; with tickets, the order
        // in which they were recorded)
        const bool secondFirst = rule == Rule::Ticket && ticketOf[1].load() < ticketOf[0].load();
        const RenderCommandList* lists[2] = {secondFirst ? l2.get() : l1.get(), secondFirst ? l1.get() : l2.get()};
        std::vector<std::pair<VkImageLayout, VkImageLayout>> sequence;
        {
            std::lock_guard<std::mutex> lock(gTrace.mutex);
            for (VkCommandBuffer cb : (secondFirst ? std::array<VkCommandBuffer, 2>{cb2, cb1} : std::array<VkCommandBuffer, 2>{cb1, cb2})) {
                const auto& v = gTrace.barriers[cb];
                sequence.insert(sequence.end(), v.begin(), v.end());
                gTrace.barriers[cb].clear();
            }
        }
        if (sequence.empty()) {
            result.emptyRounds++;
        } else {
            bool broken = false;
            VkImageLayout expectedOld = first ? sequence.front().first : previousEnd;
            for (const auto& [oldLayout, newLayout] : sequence) {
                if (oldLayout != expectedOld) {
                    broken = true;
                }
                expectedOld = newLayout;
            }
            if (broken) {
                result.brokenChains++;
            }
            previousEnd = sequence.back().second;
            first = false;
        }
        auto f = dev.createCommandFence();
        q->executeCommandLists(lists, 2, nullptr, 0, nullptr, 0, f.get());
        q->waitForCommandFence(f.get());
    }
    t1.join();
    t2.join();
    result.overwrittenThread1 = overwritten1.load();
    result.overwrittenThread2 = overwritten2.load();
    std::printf("rhi_plume_patterns_test: %s: %d rounds x 2 threads x %d barriers on one texture; record-time layout overwritten by the other thread: "
                "%d (thread 1) + %d (thread 2) times; submission-order layout chains broken: %d of %d rounds; owner rule %s\n",
                rule == Rule::None ? "D2" : rule == Rule::Harness ? "D3" : "D3b", rounds, barriersPerList, result.overwrittenThread1,
                result.overwrittenThread2, result.brokenChains, rounds,
                rule == Rule::None ? "off" : rule == Rule::Harness ? "ON (mutex, thread 2 after thread 1)" : "ON (mutex, ticket order)");
    std::fflush(stdout);
    return result;
}

// ---- D4: lists recorded in one order and submitted in the other -----------------------------------------------------------------

void RunD4(Rig& rig) {
    RenderDevice& dev = *rig.dev;
    auto T = MakeTarget(dev);
    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto l1 = q->createCommandList();
    auto l2 = q->createCommandList();
    l1->begin();
    l1->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(T.get(), RenderTextureLayout::COLOR_WRITE));
    l1->end();
    l2->begin();
    l2->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(T.get(), RenderTextureLayout::SHADER_READ));
    l2->end();
    // list 2's old layout is the one list 1 left (COLOR_WRITE), and list 2 is submitted first
    auto f = dev.createCommandFence();
    q->executeCommandLists(l2.get(), nullptr);
    q->executeCommandLists(l1.get(), f.get());
    q->waitForCommandFence(f.get());
}

// ---- E: the pipeline cost (patch 06) ---------------------------------------------------------------------------------------------

void RunE(Rig& rig) {
    RenderDevice& dev = *rig.dev;
    TraceGuard trace(true, false);
    Scene s(dev);
    REQUIRE(s.Valid());
    // 20 more identical pipelines, twice: 2 + 40 = 42 in all, with two framebuffers
    std::vector<std::unique_ptr<RenderPipeline>> pipelines;
    for (int i = 0; i < 40; i++) {
        pipelines.push_back(MakePipeline(dev, s.S2.layout.get(), s.fsVs.get(), s.smpFs.get(), 1));
    }
    int renderPasses, created, withoutCache;
    {
        std::lock_guard<std::mutex> lock(gTrace.mutex);
        renderPasses = gTrace.renderPasses;
        created = gTrace.pipelinesCreated;
        withoutCache = gTrace.pipelinesWithoutCache;
    }
    CHECK(created == 42);
    CHECK(withoutCache == 0);  // every vkCreateGraphicsPipelines call names the device's VkPipelineCache
    CHECK(renderPasses == 3);  // 6.9: 3 render passes for the 42 pipelines (unpatched: 44)
    std::vector<VkRenderPass> passes;
    for (const auto& p : pipelines) {
        const VkRenderPass rp = static_cast<VulkanGraphicsPipeline*>(p.get())->renderPass;
        if (std::find(passes.begin(), passes.end(), rp) == passes.end()) {
            passes.push_back(rp);
        }
    }
    CHECK(passes.size() == 1);  // the 40 identical pipelines share one render pass

    // the pipeline cache a device saves is accepted by a second device
    std::vector<uint8_t> blob;
    REQUIRE(dev.savePipelineCache(blob));
    CHECK(blob.size() >= 16);  // lavapipe: the 32-byte header
    auto second = rig.iface->createDevice();
    REQUIRE(second != nullptr);
    CHECK(second->loadPipelineCache(blob.data(), blob.size()));
}

// ---- F: the blend constant (patch 07) ------------------------------------------------------------------------------------------------

void RunF(Rig& rig) {
    RenderDevice& dev = *rig.dev;
    auto X = MakeTarget(dev);
    auto fbX = MakeFramebuffer(dev, X.get());
    auto vs = MakeShader(dev, SPV(spv_fs_vert));
    auto fs = MakeShader(dev, SPV(spv_blend_frag));
    auto L = EmptyLayout(dev);
    RenderBlendDesc b;
    b.blendEnabled = true;
    b.srcBlend = RenderBlend::BLEND_FACTOR;
    b.dstBlend = RenderBlend::ZERO;
    b.blendOp = RenderBlendOperation::ADD;
    b.srcBlendAlpha = RenderBlend::BLEND_FACTOR;
    b.dstBlendAlpha = RenderBlend::ZERO;
    b.blendOpAlpha = RenderBlendOperation::ADD;
    RenderGraphicsPipelineDesc gd;
    gd.pipelineLayout = L.get();
    gd.vertexShader = vs.get();
    gd.pixelShader = fs.get();
    gd.renderTargetCount = 1;
    gd.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
    gd.renderTargetBlend[0] = b;
    gd.dynamicBlendConstantsEnabled = true;
    auto P = dev.createGraphicsPipeline(gd);
    REQUIRE(static_cast<VulkanGraphicsPipeline*>(P.get())->vk != VK_NULL_HANDLE);
    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto l = q->createCommandList();
    l->begin();
    l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(X.get(), RenderTextureLayout::COLOR_WRITE));
    l->setFramebuffer(fbX.get());
    DrawState(l.get(), P.get(), L.get(), nullptr);
    const float constants[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    l->setBlendConstants(constants);
    l->clearColor(0, RenderColor(0.5f, 0.5f, 0.5f, 0.5f));
    l->drawInstanced(3, 1, 0, 0);
    l->end();
    SubmitAndWait(dev, q.get(), l.get());
    // source (1,1,1,1) times the constant: 0.5 * 255 = 128 (the pipeline's own constant would give 0)
    const auto out = ReadTexture(dev, q.get(), X.get());
    for (uint32_t i = 0; i < kW * kH; i++) {
        for (int c = 0; c < 4; c++) {
            REQUIRE(Near1(out[i * 4 + c], 128));
        }
    }
}

// ---- patch 10: dynamic stencil state -----------------------------------------------------------------------------------------------

constexpr uint32_t kQuadrant = kW / 2;

// A stencil target cleared to 5, four 8x8 quadrants of a colour target cleared black, each with its own state set on the
// command list:
//   Q0 reference 5, compare mask 0xFF, EQUAL            -> passes (white)
//   Q1 reference 6, compare mask 0xFF, EQUAL            -> fails (black)
//   Q2 reference 0x35, compare mask 0x0F, EQUAL         -> passes: only the compare mask set on the list makes 0x35 equal 5
//   Q3 first a draw with no colour that REPLACEs the stencil with reference 0xAB under write mask 0x0F (0x05 becomes 0x0B),
//      then reference 0x0B, compare mask 0xFF, EQUAL    -> passes only if the write mask was applied (0xAB would fail)
// A list whose three setters did nothing (the base class's) would leave the pipeline's own reference 0, read mask and
// write mask 0xFFFFFFFF: Q0 would fail (0 != 5).
void RunStencil(Rig& rig, RenderFormat stencilFormat, const char* name) {
    RenderDevice& dev = *rig.dev;
    if (!CapabilityOk(rig, dev.getFormatSupport(stencilFormat).attachment, "depth-stencil attachment in this format", name)) {
        return;
    }
    auto color = MakeTarget(dev);
    auto depth = dev.createTexture(RenderTextureDesc::DepthTarget(kW, kH, stencilFormat));
    const RenderTexture* attachments[1] = {color.get()};
    RenderFramebufferDesc fd(attachments, 1, depth.get());
    auto fb = dev.createFramebuffer(fd);
    REQUIRE(static_cast<VulkanFramebuffer*>(fb.get())->vk != VK_NULL_HANDLE);
    auto vs = MakeShader(dev, SPV(spv_fs_vert));
    auto fs = MakeShader(dev, SPV(spv_blend_frag));  // writes white
    auto layout = EmptyLayout(dev);

    auto makePipeline = [&](RenderComparisonFunction compare, RenderStencilOp passOp, uint8_t colorMask) {
        RenderGraphicsPipelineDesc d;
        d.pipelineLayout = layout.get();
        d.vertexShader = vs.get();
        d.pixelShader = fs.get();
        d.renderTargetCount = 1;
        d.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
        d.renderTargetBlend[0] = RenderBlendDesc::Copy();
        d.renderTargetBlend[0].renderTargetWriteMask = colorMask;
        d.depthTargetFormat = stencilFormat;
        d.stencilEnabled = true;
        d.dynamicStencilEnabled = true;
        d.stencilFrontFace.compareFunction = compare;
        d.stencilBackFace.compareFunction = compare;
        d.stencilFrontFace.passOp = passOp;
        d.stencilBackFace.passOp = passOp;
        d.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        d.cullMode = RenderCullMode::NONE;
        return dev.createGraphicsPipeline(d);
    };
    auto test = makePipeline(RenderComparisonFunction::EQUAL, RenderStencilOp::KEEP, uint8_t(RenderColorWriteEnable::ALL));
    auto replace = makePipeline(RenderComparisonFunction::ALWAYS, RenderStencilOp::REPLACE, 0);
    REQUIRE(static_cast<VulkanGraphicsPipeline*>(test.get())->vk != VK_NULL_HANDLE);
    REQUIRE(static_cast<VulkanGraphicsPipeline*>(replace.get())->vk != VK_NULL_HANDLE);

    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);
    auto l = q->createCommandList();
    l->begin();
    l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(color.get(), RenderTextureLayout::COLOR_WRITE));
    l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(depth.get(), RenderTextureLayout::DEPTH_WRITE));
    l->setFramebuffer(fb.get());
    l->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    l->clearDepthStencil(true, true, 1.0f, 5);
    auto draw = [&](const RenderPipeline* pipeline, uint32_t quadrant, uint32_t reference, uint32_t compareMask, uint32_t writeMask) {
        const int32_t x = int32_t((quadrant & 1) * kQuadrant);
        const int32_t y = int32_t((quadrant >> 1) * kQuadrant);
        DrawState(l.get(), pipeline, layout.get(), nullptr);
        l->setScissors(RenderRect(x, y, x + int32_t(kQuadrant), y + int32_t(kQuadrant)));
        l->setStencilReference(reference);
        l->setStencilCompareMask(compareMask);
        l->setStencilWriteMask(writeMask);
        l->drawInstanced(3, 1, 0, 0);
    };
    draw(test.get(), 0, 5, 0xFF, 0xFF);
    draw(test.get(), 1, 6, 0xFF, 0xFF);
    draw(test.get(), 2, 0x35, 0x0F, 0xFF);
    draw(replace.get(), 3, 0xAB, 0xFF, 0x0F);
    draw(test.get(), 3, 0x0B, 0xFF, 0xFF);
    l->end();
    SubmitAndWait(dev, q.get(), l.get());

    const auto px = ReadTexture(dev, q.get(), color.get());
    const bool expectWhite[4] = {true, false, true, true};
    for (uint32_t quadrant = 0; quadrant < 4; quadrant++) {
        int wrong = 0;
        for (uint32_t y = 0; y < kQuadrant; y++) {
            for (uint32_t x = 0; x < kQuadrant; x++) {
                const uint32_t ox = (quadrant & 1) * kQuadrant + x;
                const uint32_t oy = (quadrant >> 1) * kQuadrant + y;
                const uint8_t* p = &px[(oy * kW + ox) * 4];
                const uint8_t value = expectWhite[quadrant] ? 255 : 0;
                if (p[0] != value || p[1] != value || p[2] != value || p[3] != 255) {
                    wrong++;
                }
            }
        }
        INFO(std::string(name) << " quadrant " << quadrant);
        CHECK(wrong == 0);
    }
}

// ---- patch 11: D24S8 and D32S8 copies, the 16-bit colour formats ---------------------------------------------------------------------

// A depth-stencil image is cleared and copied to a buffer: one copy command, two regions (patch 11): the depth plane at
// the footprint's offset (4 bytes per texel, D24 in the low 24 bits, D32 the float), then the stencil plane right after it.
void RunDepthStencilCopy(Rig& rig, RenderFormat format, const char* name) {
    RenderDevice& dev = *rig.dev;
    if (!CapabilityOk(rig, dev.getFormatSupport(format).attachment, "depth-stencil attachment in this format", name)) {
        return;
    }
    auto color = MakeTarget(dev);
    auto depth = dev.createTexture(RenderTextureDesc::DepthTarget(kW, kH, format));
    const RenderTexture* attachments[1] = {color.get()};
    RenderFramebufferDesc fd(attachments, 1, depth.get());
    auto fb = dev.createFramebuffer(fd);
    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);
    const size_t planeBytes = size_t(kW) * kH;
    struct Clear {
        float depth;
        uint32_t stencil;
        uint32_t depthWord;  // the depth plane's word (24 bits for D24)
    };
    const bool d24 = format == RenderFormat::D24_UNORM_S8_UINT;
    const Clear clears[2] = {{1.0f, 0xAB, d24 ? 0xFFFFFFu : 0x3F800000u}, {0.0f, 0x3C, 0u}};
    for (const Clear& clear : clears) {
        auto rb = dev.createBuffer(RenderBufferDesc::ReadbackBuffer(planeBytes * 4 + planeBytes));
        auto l = q->createCommandList();
        l->begin();
        l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(color.get(), RenderTextureLayout::COLOR_WRITE));
        l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(depth.get(), RenderTextureLayout::DEPTH_WRITE));
        l->setFramebuffer(fb.get());
        l->clearDepthStencil(true, true, clear.depth, clear.stencil);
        l->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(depth.get(), RenderTextureLayout::COPY_SOURCE));
        l->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(rb.get(), format, kW, kH, 1, kW), RenderTextureCopyLocation::Subresource(depth.get()));
        l->end();
        SubmitAndWait(dev, q.get(), l.get());
        const uint8_t* bytes = static_cast<const uint8_t*>(rb->map());
        int wrongDepth = 0, wrongStencil = 0;
        for (size_t i = 0; i < planeBytes; i++) {
            uint32_t word;
            std::memcpy(&word, bytes + i * 4, 4);
            if (d24) {
                word &= 0xFFFFFFu;  // the top byte of the word is not defined
            }
            wrongDepth += (word != clear.depthWord) ? 1 : 0;
            wrongStencil += (bytes[planeBytes * 4 + i] != clear.stencil) ? 1 : 0;
        }
        rb->unmap();
        INFO(std::string(name) << " depth " << clear.depth << " stencil " << clear.stencil);
        CHECK(wrongDepth == 0);
        CHECK(wrongStencil == 0);
    }
}

struct PackedFormat {
    RenderFormat format;
    const char* name;
    // bit shift and width of each component in the 16-bit word (Vulkan's *_PACK16 layouts)
    int r[2], g[2], b[2], a[2];
};

uint16_t Pack(const PackedFormat& f, bool r, bool g, bool b, bool a) {
    auto field = [](bool on, const int c[2]) { return on ? uint32_t(((1u << c[1]) - 1u) << c[0]) : 0u; };
    return uint16_t(field(r, f.r) | field(g, f.g) | field(b, f.b) | field(a, f.a));
}

const PackedFormat kPacked[] = {
    {RenderFormat::R5G6B5_UNORM, "R5G6B5", {11, 5}, {5, 6}, {0, 5}, {0, 0}},
    {RenderFormat::B5G6R5_UNORM, "B5G6R5", {0, 5}, {5, 6}, {11, 5}, {0, 0}},
    {RenderFormat::R5G5B5A1_UNORM, "R5G5B5A1", {11, 5}, {6, 5}, {1, 5}, {0, 1}},
    {RenderFormat::B5G5R5A1_UNORM, "B5G5R5A1", {1, 5}, {6, 5}, {11, 5}, {0, 1}},
    {RenderFormat::A1R5G5B5_UNORM, "A1R5G5B5", {10, 5}, {5, 5}, {0, 5}, {15, 1}},
    {RenderFormat::R4G4B4A4_UNORM, "R4G4B4A4", {12, 4}, {8, 4}, {4, 4}, {0, 4}},
    {RenderFormat::B4G4R4A4_UNORM, "B4G4R4A4", {4, 4}, {8, 4}, {12, 4}, {0, 4}},
};

// Each 16-bit colour format the query says is an attachment is created as one, cleared to red with alpha 1 and to blue with
// alpha 0, and copied out: the word has the bits the format's component order puts them in.
void RunPackedFormats(Rig& rig) {
    RenderDevice& dev = *rig.dev;
    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);
    int tried = 0;
    for (const PackedFormat& f : kPacked) {
        const RenderFormatSupport support = dev.getFormatSupport(f.format);
        std::printf("rhi_plume_patterns_test: format %s: attachment=%d sampled=%d blitSource=%d blitDestination=%d\n", f.name, support.attachment,
                    support.sampled, support.blitSource, support.blitDestination);
        if (!support.attachment) {
            continue;
        }
        tried++;
        auto target = MakeTarget(dev, f.format);
        REQUIRE(VkImageOf(target.get()) != VK_NULL_HANDLE);
        auto fb = MakeFramebuffer(dev, target.get());
        REQUIRE(static_cast<VulkanFramebuffer*>(fb.get())->vk != VK_NULL_HANDLE);
        struct Clear {
            float r, g, b, a;
        };
        for (const Clear c : {Clear{1, 0, 0, 1}, Clear{0, 0, 1, 0}, Clear{0, 1, 0, 1}}) {
            auto l = q->createCommandList();
            l->begin();
            l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE));
            l->setFramebuffer(fb.get());
            l->clearColor(0, RenderColor(c.r, c.g, c.b, c.a));
            l->end();
            SubmitAndWait(dev, q.get(), l.get());
            const auto bytes = ReadRaw(dev, q.get(), target.get(), f.format, size_t(kW) * kH * 2);
            const uint16_t expected = Pack(f, c.r != 0, c.g != 0, c.b != 0, c.a != 0);
            int wrong = 0;
            for (size_t i = 0; i < size_t(kW) * kH; i++) {
                uint16_t word;
                std::memcpy(&word, &bytes[i * 2], 2);
                wrong += (word != expected) ? 1 : 0;
            }
            INFO(std::string(f.name) << " clear (" << c.r << "," << c.g << "," << c.b << "," << c.a << ") expected word 0x" << std::hex << expected);
            CHECK(wrong == 0);
        }
    }
    CHECK_MESSAGE(tried > 0, "no 16-bit colour format is an attachment on this device");
}

// ---- patch 12: a custom border colour ---------------------------------------------------------------------------------------------------

// Samples a green 16x16 target with border addressing over uv * 3 - 1 (border.frag): the pixels whose coordinate is out of
// range return the sampler's border colour, the middle ones return green. Two samplers: the custom colour
// (0.25, 0.5, 0.75, 0.5), and OPAQUE_BLACK as the reference for what an ordinary border gives.
void RunCustomBorder(Rig& rig) {
    RenderDevice& dev = *rig.dev;
    auto source = MakeTarget(dev);
    auto fbSource = MakeFramebuffer(dev, source.get());
    auto vs = MakeShader(dev, SPV(spv_fs_vert));
    auto fs = MakeShader(dev, SPV(spv_border_frag));
    Set1 set;
    set.Build(dev, {RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1), RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1)});
    auto pipeline = MakePipeline(dev, set.layout.get(), vs.get(), fs.get(), 1);
    REQUIRE(static_cast<VulkanGraphicsPipeline*>(pipeline.get())->vk != VK_NULL_HANDLE);

    auto makeSampler = [&](RenderBorderColor border, const float color[4]) {
        RenderSamplerDesc sd;
        sd.minFilter = sd.magFilter = RenderFilter::NEAREST;
        sd.mipmapMode = RenderMipmapMode::NEAREST;
        sd.addressU = sd.addressV = sd.addressW = RenderTextureAddressMode::BORDER;
        sd.borderColor = border;
        if (color != nullptr) {
            std::copy(color, color + 4, sd.customBorderColor);
        }
        return dev.createSampler(sd);
    };
    const float custom[4] = {0.25f, 0.5f, 0.75f, 0.5f};
    auto customSampler = makeSampler(RenderBorderColor::CUSTOM, custom);
    auto blackSampler = makeSampler(RenderBorderColor::OPAQUE_BLACK, nullptr);
    REQUIRE(static_cast<VulkanSampler*>(customSampler.get())->vk != VK_NULL_HANDLE);
    REQUIRE(static_cast<VulkanSampler*>(blackSampler.get())->vk != VK_NULL_HANDLE);

    auto q = dev.createCommandQueue(RenderCommandListType::DIRECT);
    // the source: cleared green
    {
        auto l = q->createCommandList();
        l->begin();
        l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(source.get(), RenderTextureLayout::COLOR_WRITE));
        l->setFramebuffer(fbSource.get());
        l->clearColor(0, RenderColor(0.0f, 1.0f, 0.0f, 1.0f));
        l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(source.get(), RenderTextureLayout::SHADER_READ));
        l->end();
        SubmitAndWait(dev, q.get(), l.get());
    }
    set.set->setTexture(0, source.get(), RenderTextureLayout::SHADER_READ);

    auto sampleWith = [&](RenderSampler* sampler) {
        auto target = MakeTarget(dev);
        auto fb = MakeFramebuffer(dev, target.get());
        set.set->setSampler(1, sampler);
        auto l = q->createCommandList();
        l->begin();
        l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE));
        l->setFramebuffer(fb.get());
        DrawState(l.get(), pipeline.get(), set.layout.get(), set.set.get());
        l->drawInstanced(3, 1, 0, 0);
        l->end();
        SubmitAndWait(dev, q.get(), l.get());  // the set is rewritten only when no submission uses it
        return ReadTexture(dev, q.get(), target.get());
    };
    // pixels 5..10 of a row map into the texture (uv * 3 - 1 in 0..1), the others are out of range
    auto inside = [](uint32_t c) { return c >= 5 && c <= 10; };
    auto check = [&](const std::vector<uint8_t>& image, const int border[4], const char* what) {
        int wrongBorder = 0, wrongInside = 0;
        for (uint32_t y = 0; y < kH; y++) {
            for (uint32_t x = 0; x < kW; x++) {
                const uint8_t* p = &image[(y * kW + x) * 4];
                if (inside(x) && inside(y)) {
                    wrongInside += (Near1(p[0], 0) && Near1(p[1], 255) && Near1(p[2], 0) && Near1(p[3], 255)) ? 0 : 1;
                } else {
                    wrongBorder += (Near1(p[0], border[0]) && Near1(p[1], border[1]) && Near1(p[2], border[2]) && Near1(p[3], border[3])) ? 0 : 1;
                }
            }
        }
        INFO(std::string(what));
        CHECK(wrongInside == 0);
        CHECK(wrongBorder == 0);
    };
    const int customExpected[4] = {64, 128, 191, 128};  // 0.25, 0.5, 0.75, 0.5 as UNORM8
    const int blackExpected[4] = {0, 0, 0, 255};
    check(sampleWith(customSampler.get()), customExpected, "custom border colour (0.25, 0.5, 0.75, 0.5)");
    check(sampleWith(blackSampler.get()), blackExpected, "OPAQUE_BLACK border colour");
}

// Where the device did not enable VK_EXT_custom_border_color, a sampler with a custom border colour is refused (a message
// on stderr and a null handle, plume's way), the other samplers are not affected, and the capability says so.
void RunCustomBorderRefused(Rig& rig) {
    CHECK_FALSE(rig.vd->capabilities.customBorderColor);
    CHECK_FALSE(Has(rig.vd->enabledExtensionNames, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME));
    RenderSamplerDesc custom;
    custom.addressU = custom.addressV = custom.addressW = RenderTextureAddressMode::BORDER;
    custom.borderColor = RenderBorderColor::CUSTOM;
    custom.customBorderColor[3] = 1.0f;
    std::fflush(stderr);
    auto refused = rig.dev->createSampler(custom);
    REQUIRE(refused != nullptr);
    CHECK(static_cast<VulkanSampler*>(refused.get())->vk == VK_NULL_HANDLE);
    RenderSamplerDesc plain = custom;
    plain.borderColor = RenderBorderColor::OPAQUE_WHITE;
    auto accepted = rig.dev->createSampler(plain);
    REQUIRE(accepted != nullptr);
    CHECK(static_cast<VulkanSampler*>(accepted.get())->vk != VK_NULL_HANDLE);
}

}  // namespace

// ---- cases ----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("vendor/border_colour_patch_is_listed") {
    // the patch that adds the custom border colour is a numbered file after patch 11; its number is read from the list
    // the build records, never written here
    const auto& info = mh4n::gpu::backend::plume::VendorInfo();
    int number = 0;
    for (size_t i = 0; i < info.patches.size(); i++) {
        const std::string name = info.patches[i];
        if (name.find("-custom-border-colour.diff") != std::string::npos) {
            number = std::atoi(name.c_str());
            CHECK(i + 1 == size_t(number));  // the queue is numbered from 1 without gaps
        }
    }
    CHECK(number >= 12);
}

TEST_CASE("A2_render_then_sample_in_two_lists") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "A2")) {
        return;
    }
    LogDevice(rig, "A2");
    RunA2(rig);
}

TEST_CASE("B1_shadow_store_no_interlock") {
    ShadowVariant v;
    v.unordered = true;  // see ShadowVariant: asserts what the shader guarantees (1 to 3 per texel), logs how many hold 3
    RunShadowCase("B1", v);
}
TEST_CASE("B2_shadow_store_interlock") {
    ShadowVariant v;
    v.interlock = true;
    RunShadowCase("B2", v);
}
TEST_CASE("B3_shadow_cas_no_interlock") {
    ShadowVariant v;
    v.cas = true;
    RunShadowCase("B3", v);
}
TEST_CASE("B4_shadow_cas_interlock") {
    ShadowVariant v;
    v.cas = true;
    v.interlock = true;
    RunShadowCase("B4", v);
}
TEST_CASE("B5_shadow_store_barrier_between_draws") {
    ShadowVariant v;
    v.barrierBetween = true;
    RunShadowCase("B5", v);
}

TEST_CASE("B8_colour_attachment_in_general_layout") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "B8")) {
        return;
    }
    LogDevice(rig, "B8");
    RunB8(rig);
}

TEST_CASE("C1_binary_semaphore_graphics_barrier") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "C1")) {
        return;
    }
    LogDevice(rig, "C1");
    RunBinaryHandOff(rig, RenderBarrierStage::GRAPHICS);
}

TEST_CASE("C2_binary_semaphore_none_barrier_per_wait_stage") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "C2")) {
        return;
    }
    LogDevice(rig, "C2");
    RunBinaryHandOff(rig, RenderBarrierStage::NONE);
}

TEST_CASE("C4_timeline_semaphore_with_stage") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "C4")) {
        return;
    }
    LogDevice(rig, "C4");
    if (!CapabilityOk(rig, rig.vd->capabilities.timelineSemaphore, "timelineSemaphore", "C4")) {
        return;
    }
    RunTimelineHandOff(rig);
}

TEST_CASE("D1_draw_on_one_thread_sample_on_another") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "D1")) {
        return;
    }
    LogDevice(rig, "D1");
    RunD1(rig);
}

TEST_CASE("D3_owner_rule_two_threads_one_texture") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "D3")) {
        return;
    }
    LogDevice(rig, "D3");
    const RaceResult r = RunTwoThreadBarriers(rig, Rule::Harness, 2000, 20);
    CHECK(r.emptyRounds == 0);
    CHECK(r.overwrittenThread1 == 0);
    CHECK(r.overwrittenThread2 == 0);
    CHECK(r.brokenChains == 0);
}

TEST_CASE("D3b_owner_rule_mutex_and_ticket_order") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "D3b")) {
        return;
    }
    LogDevice(rig, "D3b");
    const RaceResult r = RunTwoThreadBarriers(rig, Rule::Ticket, 2000, 19);
    CHECK(r.emptyRounds == 0);
    CHECK(r.overwrittenThread1 == 0);
    CHECK(r.overwrittenThread2 == 0);
    CHECK(r.brokenChains == 0);
}

TEST_CASE("E_pipeline_cache_and_render_pass_cache") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "E")) {
        return;
    }
    LogDevice(rig, "E");
    RunE(rig);
}

TEST_CASE("F_blend_constant_half") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "F")) {
        return;
    }
    LogDevice(rig, "F");
    RunF(rig);
}

TEST_CASE("patch09/optional_extension_lists") {
    // the default list takes no ray tracing and, without a surface, no presentation extension; an empty list takes none of the
    // optional ones; a named one is taken when offered (patch 09)
    Rig byDefault;
    if (!DeviceOk(byDefault.Open(), byDefault.error, "patch09/optional_extension_lists")) {
        return;
    }
    LogDevice(byDefault, "patch09/optional_extension_lists");
    for (const std::string& name : byDefault.vd->enabledExtensionNames) {
        INFO(name);
        CHECK(name.find("ray_tracing") == std::string::npos);
        CHECK(name.find("acceleration_structure") == std::string::npos);
        CHECK(name != VK_KHR_PRESENT_ID_EXTENSION_NAME);
        CHECK(name != VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        CHECK(name != VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    }

    VulkanDeviceExtras none;
    none.optionalExtensions = std::vector<std::string>{};
    Rig empty;
    REQUIRE_MESSAGE(empty.Open({}, none), empty.error);
    for (const std::string& name : empty.vd->enabledExtensionNames) {
        INFO(name);
        CHECK(name == VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);  // the one extension that is always added when offered
    }
    CHECK_FALSE(empty.vd->capabilities.fragmentShaderInterlock);
    CHECK_FALSE(empty.vd->capabilities.indexTypeUint8);
    CHECK_FALSE(empty.vd->capabilities.customBorderColor);
    CHECK_FALSE(empty.vd->capabilities.descriptorIndexing);

    VulkanDeviceExtras named;
    named.optionalExtensions = std::vector<std::string>{VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME};
    Rig one;
    REQUIRE_MESSAGE(one.Open({}, named), one.error);
    const bool offered = byDefault.vd->capabilities.customBorderColor;  // the default list takes it when it is offered
    CHECK(Has(one.vd->enabledExtensionNames, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME) == offered);
    CHECK_FALSE(one.vd->capabilities.fragmentShaderInterlock);
}

TEST_CASE("patch10/dynamic_stencil_state_d24s8") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "patch10/d24s8")) {
        return;
    }
    LogDevice(rig, "patch10/d24s8");
    RunStencil(rig, RenderFormat::D24_UNORM_S8_UINT, "D24S8");
}

TEST_CASE("patch10/dynamic_stencil_state_d32s8") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "patch10/d32s8")) {
        return;
    }
    LogDevice(rig, "patch10/d32s8");
    RunStencil(rig, RenderFormat::D32_FLOAT_S8_UINT, "D32S8");
}

TEST_CASE("patch11/depth_stencil_copy_as_two_regions") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "patch11/depth_stencil_copy")) {
        return;
    }
    LogDevice(rig, "patch11/depth_stencil_copy");
    // D24S8 is the case of the spec; D32S8 is the format the PC takes (4, 9.3) and uses the same two regions
    RunDepthStencilCopy(rig, RenderFormat::D24_UNORM_S8_UINT, "D24S8");
    RunDepthStencilCopy(rig, RenderFormat::D32_FLOAT_S8_UINT, "D32S8");
}

TEST_CASE("patch11/16_bit_colour_formats_where_the_query_says_attachment") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "patch11/16_bit_formats")) {
        return;
    }
    LogDevice(rig, "patch11/16_bit_formats");
    RunPackedFormats(rig);
}

TEST_CASE("patch12/custom_border_colour_out_of_range") {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "patch12/custom_border_colour")) {
        return;
    }
    LogDevice(rig, "patch12/custom_border_colour");
    if (!CapabilityOk(rig, rig.vd->capabilities.customBorderColor, "customBorderColor", "patch12/custom_border_colour")) {
        return;
    }
    RunCustomBorder(rig);
}

TEST_CASE("patch12/custom_border_colour_refused_without_the_extension") {
    VulkanDeviceExtras none;
    none.optionalExtensions = std::vector<std::string>{};  // VK_EXT_custom_border_color is not enabled
    Rig rig;
    if (!DeviceOk(rig.Open({}, none), rig.error, "patch12/refused")) {
        return;
    }
    LogDevice(rig, "patch12/refused");
    RunCustomBorderRefused(rig);
}

// ---- controls: skipped unless named (`--test-case=control/B7 --no-skip`) ---------------------------------------------------------------------

// Draws 4 and 5 are in command lists of their own with no barrier after the first three: the pixels are right on lavapipe
// and the layer must report SYNC-HAZARD-WRITE-AFTER-WRITE.
TEST_CASE("control/B7" * doctest::skip()) {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "control/B7")) {
        return;
    }
    LogDevice(rig, "control/B7");
    ShadowVariant v;
    v.twoLists = true;
    CHECK(RunShadow(rig, v, "B7") == 0);
}

// The example of what the layer misses (6.9): a new pass for each draw, storage-image writes, no barrier, in one command
// buffer. The pixels are right on lavapipe and the layer says nothing; nothing asserts on it.
TEST_CASE("control/B6" * doctest::skip()) {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "control/B6")) {
        return;
    }
    LogDevice(rig, "control/B6");
    ShadowVariant v;
    v.restartNoBarrier = true;
    (void)RunShadow(rig, v, "B6");
}

// Recorded 1, 2; submitted 2, 1: the layer must report UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout.
TEST_CASE("control/D4" * doctest::skip()) {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "control/D4")) {
        return;
    }
    LogDevice(rig, "control/D4");
    RunD4(rig);  // nothing asserts here: the layer's message is the result
}

// D3 without the owner rule: the layer must report VUID-VkImageMemoryBarrier-oldLayout-01197 and InvalidImageLayout (counts
// vary per run) and ThreadSanitizer a data race on the texture's layout field. Not run by the ThreadSanitizer job.
TEST_CASE("control/D2" * doctest::skip()) {
    Rig rig;
    if (!DeviceOk(rig.Open(), rig.error, "control/D2")) {
        return;
    }
    LogDevice(rig, "control/D2");
    const RaceResult r = RunTwoThreadBarriers(rig, Rule::None, 2000, 20);
    CHECK(r.emptyRounds == 0);
}
