// N2-01 plume validation experiment: patterns A..F run through plume's public API (plain C++17).
// usage: plume_val <pattern>   (A1 A2 B1..B5 C1 C2 C3 D1 D2 D3 E F)
// Exit status: 0 = pixels/behaviour as expected, 1 = pixels wrong, 2 = setup failed (object creation), 3 = bad usage.
// Layer messages go to stdout (the layer's default); our own lines start with "[T] ".
#include "plume_vulkan.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gen/spv_blend_frag.h"
#include "gen/spv_fs_vert.h"
#include "gen/spv_sample_frag.h"
#include "gen/spv_shadow_cas_il.h"
#include "gen/spv_shadow_cas_nil.h"
#include "gen/spv_shadow_store_il.h"
#include "gen/spv_shadow_store_nil.h"
#include "gen/spv_tri_frag.h"
#include "gen/spv_tri_vert.h"

namespace plume { extern std::unique_ptr<RenderInterface> CreateVulkanInterface(); }
using namespace plume;

static const uint32_t W = 16, H = 16;
static bool gContinue = false; // PLUME_VAL_CONTINUE=1: do not stop at the first null handle

static void say(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    printf("[T] %s\n", buf); fflush(stdout);
}

struct Env {
    std::unique_ptr<RenderInterface> iface;
    std::unique_ptr<RenderDevice> dev;
    VulkanDevice *vd = nullptr;
} E;

#define SPV(a) (const void *)(a), sizeof(a)

static std::unique_ptr<RenderShader> mkShader(const void *d, size_t n) { return E.dev->createShader(d, n, "main", RenderShaderFormat::SPIRV); }

// --- call trace (PLUME_VAL_TRACE=1): plume calls Vulkan through volk's global function pointers, so the test program can
// interpose on them without touching plume. Lines start with "[V] ".
static PFN_vkQueueSubmit r_QueueSubmit;
static PFN_vkCmdPipelineBarrier r_CmdPipelineBarrier;
static PFN_vkCreateRenderPass r_CreateRenderPass;
static PFN_vkCreateGraphicsPipelines r_CreateGraphicsPipelines;
static PFN_vkCreateImage r_CreateImage;
static PFN_vkCmdBeginRenderPass r_CmdBeginRenderPass;
static std::atomic<int> nRenderPassCreated{ 0 }, nPipelinesCreated{ 0 }, nBeginRenderPass{ 0 };

static void vtrace(const char *fmt, ...) {
    char buf[1536];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    printf("[V] %s\n", buf); fflush(stdout);
}
static VkResult VKAPI_CALL t_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    for (uint32_t i = 0; i < n; i++) {
        std::string m;
        for (uint32_t j = 0; j < s[i].waitSemaphoreCount && j < 4; j++) { char b[48]; snprintf(b, sizeof b, " [%u]=0x%x", j, s[i].pWaitDstStageMask[j]); m += b; }
        vtrace("vkQueueSubmit: commandBuffers=%u waitSemaphoreCount=%u signalSemaphoreCount=%u pWaitDstStageMask=%s%s (0x400 = COLOR_ATTACHMENT_OUTPUT; the pointer is plume's single local %s)",
            s[i].commandBufferCount, s[i].waitSemaphoreCount, s[i].signalSemaphoreCount, s[i].waitSemaphoreCount ? "array" : "null", m.c_str(), s[i].waitSemaphoreCount > 1 ? "variable, so elements past [0] are not plume's" : "variable");
    }
    return r_QueueSubmit(q, n, s, f);
}
static void VKAPI_CALL t_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags df, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    vtrace("vkCmdPipelineBarrier: srcStageMask=0x%x dstStageMask=0x%x bufferBarriers=%u imageBarriers=%u", src, dst, nb, ni);
    for (uint32_t i = 0; i < ni; i++)
        vtrace("  image barrier: old=%d new=%d srcAccess=0x%x dstAccess=0x%x (0x8000 MEMORY_READ | 0x10000 MEMORY_WRITE) aspect=0x%x levels=%u layers=%u srcQF=%u dstQF=%u", int(im[i].oldLayout), int(im[i].newLayout), im[i].srcAccessMask, im[i].dstAccessMask, im[i].subresourceRange.aspectMask, im[i].subresourceRange.levelCount, im[i].subresourceRange.layerCount, im[i].srcQueueFamilyIndex, im[i].dstQueueFamilyIndex);
    r_CmdPipelineBarrier(cb, src, dst, df, nm, m, nb, b, ni, im);
}
static VkResult VKAPI_CALL t_CreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *rp) {
    nRenderPassCreated++;
    vtrace("vkCreateRenderPass #%d: attachments=%u subpasses=%u dependencies=%u att0.initial=%d att0.final=%d", nRenderPassCreated.load(), ci->attachmentCount, ci->subpassCount, ci->dependencyCount, ci->attachmentCount ? int(ci->pAttachments[0].initialLayout) : -1, ci->attachmentCount ? int(ci->pAttachments[0].finalLayout) : -1);
    return r_CreateRenderPass(d, ci, a, rp);
}
static VkResult VKAPI_CALL t_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    nPipelinesCreated += int(n);
    std::string dyn;
    for (uint32_t i = 0; ci[0].pDynamicState && i < ci[0].pDynamicState->dynamicStateCount; i++) { char b[32]; snprintf(b, sizeof b, " %d", int(ci[0].pDynamicState->pDynamicStates[i])); dyn += b; }
    const float *bc = ci[0].pColorBlendState->blendConstants;
    vtrace("vkCreateGraphicsPipelines: count=%u pipelineCache=%p viewportCount=%u blendConstants=(%g,%g,%g,%g) dynamicStates:%s (0=VIEWPORT 1=SCISSOR 3=BLEND_CONSTANTS 12=STENCIL_REFERENCE)",
        n, (void *)c, ci[0].pViewportState->viewportCount, bc[0], bc[1], bc[2], bc[3], dyn.c_str());
    return r_CreateGraphicsPipelines(d, c, n, ci, a, p);
}
static VkResult VKAPI_CALL t_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *i) {
    vtrace("vkCreateImage: format=%d flags=0x%x usage=0x%x (flags bit 0x8 = MUTABLE_FORMAT) sharing=%d", int(ci->format), ci->flags, ci->usage, int(ci->sharingMode));
    return r_CreateImage(d, ci, a, i);
}
static void VKAPI_CALL t_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    nBeginRenderPass++;
    r_CmdBeginRenderPass(cb, bi, c);
}
static void installTrace() {
    r_QueueSubmit = vkQueueSubmit; vkQueueSubmit = t_QueueSubmit;
    r_CmdPipelineBarrier = vkCmdPipelineBarrier; vkCmdPipelineBarrier = t_CmdPipelineBarrier;
    r_CreateRenderPass = vkCreateRenderPass; vkCreateRenderPass = t_CreateRenderPass;
    r_CreateGraphicsPipelines = vkCreateGraphicsPipelines; vkCreateGraphicsPipelines = t_CreateGraphicsPipelines;
    r_CreateImage = vkCreateImage; vkCreateImage = t_CreateImage;
    r_CmdBeginRenderPass = vkCmdBeginRenderPass; vkCmdBeginRenderPass = t_CmdBeginRenderPass;
}

// --- small helpers -------------------------------------------------------------------------------------------------
struct Set1 { // one descriptor set layout + its pipeline layout (+ optionally a set)
    std::vector<RenderDescriptorRange> ranges;
    RenderDescriptorSetDesc sd;
    std::unique_ptr<RenderPipelineLayout> layout;
    std::unique_ptr<RenderDescriptorSet> set;
    void build(std::vector<RenderDescriptorRange> r) {
        ranges = std::move(r);
        sd = RenderDescriptorSetDesc(ranges.data(), uint32_t(ranges.size()));
        RenderPipelineLayoutDesc ld;
        ld.descriptorSetDescs = &sd; ld.descriptorSetDescsCount = 1;
        layout = E.dev->createPipelineLayout(ld);
        set = E.dev->createDescriptorSet(sd);
    }
};

static std::unique_ptr<RenderPipelineLayout> emptyLayout() {
    RenderPipelineLayoutDesc ld;
    return E.dev->createPipelineLayout(ld);
}

static std::unique_ptr<RenderPipeline> mkPipe(const RenderPipelineLayout *l, const RenderShader *vs, const RenderShader *fs, uint32_t nrt, RenderFormat fmt = RenderFormat::R8G8B8A8_UNORM, RenderBlendDesc blend = RenderBlendDesc::Copy()) {
    RenderGraphicsPipelineDesc d;
    d.pipelineLayout = l; d.vertexShader = vs; d.pixelShader = fs;
    d.renderTargetCount = nrt;
    if (nrt) { d.renderTargetFormat[0] = fmt; d.renderTargetBlend[0] = blend; }
    d.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    d.cullMode = RenderCullMode::NONE;
    return E.dev->createGraphicsPipeline(d);
}

static std::unique_ptr<RenderTexture> mkTarget(RenderFormat f = RenderFormat::R8G8B8A8_UNORM, RenderTextureFlags extra = 0) {
    return E.dev->createTexture(RenderTextureDesc::ColorTarget(W, H, f, RenderMultisampling(), nullptr, extra));
}

static std::unique_ptr<RenderFramebuffer> mkFb(const RenderTexture *t) {
    const RenderTexture *a[1] = { t };
    RenderFramebufferDesc d(a, 1);
    return E.dev->createFramebuffer(d);
}

static VkImage vkimg(const RenderTexture *t) { return static_cast<const VulkanTexture *>(t)->vk; }

static void drawState(RenderCommandList *l, const RenderPipeline *p, const RenderPipelineLayout *pl, RenderDescriptorSet *set) {
    l->setPipeline(p);
    l->setGraphicsPipelineLayout(pl);
    if (set) l->setGraphicsDescriptorSet(set, 0);
    l->setViewports(RenderViewport(0, 0, float(W), float(H)));
    l->setScissors(RenderRect(0, 0, int32_t(W), int32_t(H)));
}

static void recCopyOut(RenderCommandList *l, RenderTexture *t, RenderBuffer *rb) {
    l->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(t, RenderTextureLayout::COPY_SOURCE));
    l->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(rb, RenderFormat::R8G8B8A8_UNORM, W, H, 1, W), RenderTextureCopyLocation::Subresource(t));
}

static void submitWait(RenderCommandQueue *q, RenderCommandList *l) {
    auto f = E.dev->createCommandFence();
    q->executeCommandLists(l, f.get());
    q->waitForCommandFence(f.get());
}

static std::vector<uint8_t> mapOut(RenderBuffer *rb) {
    std::vector<uint8_t> v(W * H * 4);
    memcpy(v.data(), rb->map(), v.size());
    rb->unmap();
    return v;
}

static std::vector<uint8_t> readTex(RenderCommandQueue *q, RenderTexture *t) {
    auto rb = E.dev->createBuffer(RenderBufferDesc::ReadbackBuffer(W * H * 4));
    auto l = q->createCommandList();
    l->begin(); recCopyOut(l.get(), t, rb.get()); l->end();
    submitWait(q, l.get());
    return mapOut(rb.get());
}

static bool near1(int a, int b) { return a - b <= 1 && b - a <= 1; }

// X expectations for the "triangle then clear" image: upper-left half is (64,128,191,255), the rest the clear (0,0,255,255).
static bool checkTri(const std::vector<uint8_t> &x, const char *name) {
    int bad = 0;
    for (uint32_t py = 0; py < H; py++) for (uint32_t px = 0; px < W; px++) {
        const uint8_t *p = &x[(py * W + px) * 4];
        bool inside = (px + py + 1 < W);           // pixel centre (px+.5, py+.5) with x+y < W
        bool onEdge = (px + py + 1 == W);
        if (onEdge) continue;
        int e[4] = { inside ? 64 : 0, inside ? 128 : 0, inside ? 191 : 255, 255 };
        for (int c = 0; c < 4; c++) if (!near1(p[c], e[c])) { bad++; break; }
    }
    say("%s: %d of %u pixels differ from the expected triangle image (corner (2,2)=%u %u %u %u, corner (13,13)=%u %u %u %u)", name, bad, W * H,
        x[(2 * W + 2) * 4], x[(2 * W + 2) * 4 + 1], x[(2 * W + 2) * 4 + 2], x[(2 * W + 2) * 4 + 3],
        x[(13 * W + 13) * 4], x[(13 * W + 13) * 4 + 1], x[(13 * W + 13) * 4 + 2], x[(13 * W + 13) * 4 + 3]);
    return bad == 0;
}

static bool checkSame(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, const char *name) {
    int bad = 0;
    for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) bad++;
    say("%s: %d of %zu bytes differ between the sampled image Y and the source image X", name, bad, a.size());
    return bad == 0;
}

static bool nullCheck(const char *what, bool isNull) {
    if (isNull) { say("STOP: %s is VK_NULL_HANDLE (creation failed)", what); return !gContinue; }
    return false;
}

// --- shared scene for A, C, D --------------------------------------------------------------------------------------
struct Scene {
    std::unique_ptr<RenderTexture> X, Y;
    std::unique_ptr<RenderFramebuffer> fbX, fbY;
    std::unique_ptr<RenderShader> triVs, triFs, fsVs, smpFs;
    std::unique_ptr<RenderPipelineLayout> L1;
    Set1 S2;
    std::unique_ptr<RenderSampler> smp;
    std::unique_ptr<RenderPipeline> P1, P2;
    bool ok = false;

    bool build() {
        X = mkTarget(); Y = mkTarget();
        fbX = mkFb(X.get()); fbY = mkFb(Y.get());
        triVs = mkShader(SPV(spv_tri_vert)); triFs = mkShader(SPV(spv_tri_frag));
        fsVs = mkShader(SPV(spv_fs_vert)); smpFs = mkShader(SPV(spv_sample_frag));
        L1 = emptyLayout();
        S2.build({ RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1), RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1) });
        RenderSamplerDesc sd; sd.minFilter = sd.magFilter = RenderFilter::NEAREST; sd.mipmapMode = RenderMipmapMode::NEAREST;
        sd.addressU = sd.addressV = sd.addressW = RenderTextureAddressMode::CLAMP;
        smp = E.dev->createSampler(sd);
        S2.set->setTexture(0, X.get(), RenderTextureLayout::SHADER_READ);
        S2.set->setSampler(1, smp.get());
        P1 = mkPipe(L1.get(), triVs.get(), triFs.get(), 1);
        P2 = mkPipe(S2.layout.get(), fsVs.get(), smpFs.get(), 1);
        ok = true;
        return true;
    }
    // draw the triangle into X (clear blue first), then X -> SHADER_READ with the given stages
    void recProduce(RenderCommandList *l, RenderBarrierStages toRead = RenderBarrierStage::GRAPHICS, bool transition = true) {
        l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(X.get(), RenderTextureLayout::COLOR_WRITE));
        l->setFramebuffer(fbX.get());
        drawState(l, P1.get(), L1.get(), nullptr);
        l->clearColor(0, RenderColor(0.0f, 0.0f, 1.0f, 1.0f));
        l->drawInstanced(3, 1, 0, 0);
        if (transition) l->barriers(toRead, RenderTextureBarrier(X.get(), RenderTextureLayout::SHADER_READ));
    }
    // full-screen triangle into Y that samples X
    void recConsume(RenderCommandList *l, bool yBarrier = true) {
        if (yBarrier) l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(Y.get(), RenderTextureLayout::COLOR_WRITE));
        l->setFramebuffer(fbY.get());
        drawState(l, P2.get(), S2.layout.get(), S2.set.get());
        l->drawInstanced(3, 1, 0, 0);
    }
};

static int finishAC(Scene &s, RenderCommandQueue *q, const char *name) {
    auto x = readTex(q, s.X.get()); auto y = readTex(q, s.Y.get());
    bool a = checkTri(x, "X"), b = checkSame(x, y, "Y==X");
    say("%s pixels %s", name, (a && b) ? "RIGHT" : "WRONG");
    return (a && b) ? 0 : 1;
}

// --- A -------------------------------------------------------------------------------------------------------------
static int patternA(int variant) {
    Scene s; s.build();
    if (nullCheck("framebuffer X", static_cast<VulkanFramebuffer *>(s.fbX.get())->vk == VK_NULL_HANDLE)) return 2;
    if (nullCheck("pipeline P1", static_cast<VulkanGraphicsPipeline *>(s.P1.get())->vk == VK_NULL_HANDLE)) return 2;
    if (nullCheck("pipeline P2", static_cast<VulkanGraphicsPipeline *>(s.P2.get())->vk == VK_NULL_HANDLE)) return 2;
    auto q = E.dev->createCommandQueue(RenderCommandListType::DIRECT);
    if (variant == 1) {
        auto l = q->createCommandList();
        l->begin();
        s.recProduce(l.get());
        s.recConsume(l.get());
        l->end();
        say("A1: submitting one command list (render X, X->SHADER_READ, render Y sampling X)");
        submitWait(q.get(), l.get());
    } else {
        auto l1 = q->createCommandList(); auto l2 = q->createCommandList();
        l1->begin(); s.recProduce(l1.get()); l1->end();
        l2->begin(); s.recConsume(l2.get()); l2->end();
        say("A2: submitting list 1 (render X, X->SHADER_READ) then list 2 (render Y sampling X) as two vkQueueSubmit calls on one queue");
        q->executeCommandLists(l1.get(), nullptr);
        submitWait(q.get(), l2.get());
    }
    return finishAC(s, q.get(), variant == 1 ? "A1" : "A2");
}

// --- B -------------------------------------------------------------------------------------------------------------
static int patternB(int variant) {
    const bool interlock = (variant == 2 || variant == 4);
#ifdef PLUME_VAL_HAS_INTERLOCK
    if (interlock) say("device capability fragmentShaderInterlock = %d", int(E.dev->getCapabilities().fragmentShaderInterlock));
#endif
    const bool cas = (variant == 3 || variant == 4);
    const bool barrierBetween = (variant == 5);
    const bool twoLists = (variant == 7);           // control: each draw in its own command list, no barrier between them
    const bool restartNoBarrier = (variant == 6);   // control: new render pass for each draw but no barrier
    const char *names[] = { "", "B1 store/no-interlock", "B2 store/interlock", "B3 cas/no-interlock", "B4 cas/interlock", "B5 store/no-interlock/barrier-between-draws", "B6 store/no-interlock/pass-restart-WITHOUT-barrier (control)", "B7 store/no-interlock/draws-in-separate-lists-WITHOUT-barrier (control)" };
    say("%s", names[variant]);

#ifdef PLUME_VAL_HAS_MUTABLE_FORMAT
    RenderTextureFlags f = RenderTextureFlag::STORAGE | RenderTextureFlag::MUTABLE_FORMAT;
#else
    RenderTextureFlags f = RenderTextureFlag::STORAGE;
#endif
    say("stage: create RGBA8 colour target with STORAGE%s", f != RenderTextureFlag::STORAGE ? " + MUTABLE_FORMAT" : " (plume has no MUTABLE_FORMAT flag)");
    auto img = mkTarget(RenderFormat::R8G8B8A8_UNORM, f);
    if (nullCheck("image", vkimg(img.get()) == VK_NULL_HANDLE)) return 2;

    say("stage: create R32_UINT view of the RGBA8 image");
    auto view = img->createTextureView(RenderTextureViewDesc::Texture2D(RenderFormat::R32_UINT));
    if (nullCheck("R32_UINT view", static_cast<VulkanTextureView *>(view.get())->vk == VK_NULL_HANDLE)) return 2;

    say("stage: create zero-attachment framebuffer");
    RenderFramebufferDesc fbd;
#ifdef PLUME_VAL_HAS_FB_SIZE
    fbd.width = W; fbd.height = H;
#endif
    auto fb = E.dev->createFramebuffer(fbd);
    if (nullCheck("zero-attachment framebuffer", static_cast<VulkanFramebuffer *>(fb.get())->vk == VK_NULL_HANDLE)) return 2;

    say("stage: create shader (%s, %s interlock)", cas ? "compare-and-swap loop" : "load/store", interlock ? "with" : "without");
    std::unique_ptr<RenderShader> fs;
    if (cas) fs = interlock ? mkShader(SPV(spv_shadow_cas_il)) : mkShader(SPV(spv_shadow_cas_nil));
    else fs = interlock ? mkShader(SPV(spv_shadow_store_il)) : mkShader(SPV(spv_shadow_store_nil));
    auto vs = mkShader(SPV(spv_fs_vert));
    if (nullCheck("fragment shader module", static_cast<VulkanShader *>(fs.get())->vk == VK_NULL_HANDLE)) return 2;

    Set1 S; S.build({ RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_TEXTURE, 0, 1) });
    S.set->setTexture(0, img.get(), RenderTextureLayout::GENERAL, view.get());
    say("stage: create pipeline with zero colour attachments");
    auto P = mkPipe(S.layout.get(), vs.get(), fs.get(), 0);
    if (nullCheck("pipeline", static_cast<VulkanGraphicsPipeline *>(P.get())->vk == VK_NULL_HANDLE)) return 2;

    auto q = E.dev->createCommandQueue(RenderCommandListType::DIRECT);
    auto up = E.dev->createBuffer(RenderBufferDesc::UploadBuffer(W * H * 4));
    memset(up->map(), 0, W * H * 4); up->unmap();
    auto l = q->createCommandList();
    l->begin();
    l->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(img.get(), RenderTextureLayout::COPY_DEST));
    l->copyTextureRegion(RenderTextureCopyLocation::Subresource(img.get()), RenderTextureCopyLocation::PlacedFootprint(up.get(), RenderFormat::R8G8B8A8_UNORM, W, H, 1, W));
    l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(img.get(), RenderTextureLayout::GENERAL));
    l->setFramebuffer(fb.get());
    drawState(l.get(), P.get(), S.layout.get(), S.set.get());
    for (int i = 0; i < 3; i++) {
        if (i && barrierBetween) {
            l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(img.get(), RenderTextureLayout::GENERAL)); // ends the pass
            l->setFramebuffer(fb.get());
            drawState(l.get(), P.get(), S.layout.get(), S.set.get());
        }
        if (i && restartNoBarrier) {
            l->setFramebuffer(fb.get()); // ends the pass, starts another at the next draw, no barrier
            drawState(l.get(), P.get(), S.layout.get(), S.set.get());
        }
        l->drawInstanced(3, 1, 0, 0);
    }
    l->end();
    say("stage: submit (3 overlapping full-screen draws, each adds 1; expected every R32 texel = 3)");
    if (twoLists) {
        // the first list holds upload + draw 1 (the loop above recorded all three draws, so re-record: draws 2 and 3 in their own lists)
        auto f = E.dev->createCommandFence();
        q->executeCommandLists(l.get(), f.get()); q->waitForCommandFence(f.get());
        // the layer's view of the next two lists: each starts a pass and draws into the same storage image, no barrier
        auto la = q->createCommandList(); auto lb = q->createCommandList();
        for (auto *x : { la.get(), lb.get() }) { x->begin(); x->setFramebuffer(fb.get()); drawState(x, P.get(), S.layout.get(), S.set.get()); x->drawInstanced(3, 1, 0, 0); x->end(); }
        q->executeCommandLists(la.get(), nullptr);
        submitWait(q.get(), lb.get());
        auto out = readTex(q.get(), img.get());
        int bad = 0; uint32_t v0; memcpy(&v0, &out[0], 4);
        for (uint32_t i = 0; i < W * H; i++) { uint32_t v; memcpy(&v, &out[i * 4], 4); if (v != 5) bad++; }
        say("%s: %d of %u texels != 5 (texel0 = %u) -> pixels %s", names[variant], bad, W * H, v0, bad ? "WRONG" : "RIGHT");
        return bad ? 1 : 0;
    }
    submitWait(q.get(), l.get());
    auto out = readTex(q.get(), img.get());
    int bad = 0;
    for (uint32_t i = 0; i < W * H; i++) { uint32_t v; memcpy(&v, &out[i * 4], 4); if (v != 3) bad++; }
    uint32_t v0; memcpy(&v0, &out[0], 4);
    say("%s: %d of %u texels != 3 (texel0 = %u) -> pixels %s", names[variant], bad, W * H, v0, bad ? "WRONG" : "RIGHT");
    return bad ? 1 : 0;
}

// --- B8: colour attachment that is in GENERAL layout (Azahar keeps everything in GENERAL) -----------------------------
static int patternB8() {
    say("B8: draw into an RGBA8 colour attachment whose layout is GENERAL (a storage-capable target), then sample nothing; read back");
    RenderTextureFlags f = RenderTextureFlag::STORAGE;
    auto img = mkTarget(RenderFormat::R8G8B8A8_UNORM, f);
    auto fb0 = mkFb(img.get());
#ifdef PLUME_VAL_HAS_GENERAL_ATTACHMENT
    say("(patched) framebuffer created with attachmentsInGeneralLayout = true");
    const RenderTexture *a[1] = { img.get() };
    RenderFramebufferDesc d(a, 1); d.colorAttachmentsGeneralLayout = true;
    auto fb = E.dev->createFramebuffer(d);
#else
    auto &fb = fb0;
#endif
    auto triVs = mkShader(SPV(spv_tri_vert)); auto triFs = mkShader(SPV(spv_tri_frag));
    auto L1 = emptyLayout();
    auto P1 = mkPipe(L1.get(), triVs.get(), triFs.get(), 1);
    auto q = E.dev->createCommandQueue(RenderCommandListType::DIRECT);
    auto l = q->createCommandList();
    l->begin();
    l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(img.get(), RenderTextureLayout::GENERAL));
    l->setFramebuffer(fb.get());
    drawState(l.get(), P1.get(), L1.get(), nullptr);
    l->clearColor(0, RenderColor(0.0f, 0.0f, 1.0f, 1.0f));
    l->drawInstanced(3, 1, 0, 0);
    l->end();
    say("B8: submit");
    submitWait(q.get(), l.get());
    auto x = readTex(q.get(), img.get());
    bool ok = checkTri(x, "B8 X");
    say("B8 pixels %s", ok ? "RIGHT" : "WRONG");
    return ok ? 0 : 1;
}

// --- C -------------------------------------------------------------------------------------------------------------
static int patternC(int variant) {
    Scene s; s.build();
    if (nullCheck("pipeline P2", static_cast<VulkanGraphicsPipeline *>(s.P2.get())->vk == VK_NULL_HANDLE)) return 2;
    auto q1 = E.dev->createCommandQueue(RenderCommandListType::DIRECT);
    auto q2 = E.dev->createCommandQueue(RenderCommandListType::DIRECT);
    auto s1 = E.dev->createCommandSemaphore(); auto s2 = E.dev->createCommandSemaphore();
    auto f = E.dev->createCommandFence();
    const bool handoffNone = (variant == 2 || variant >= 4);   // producer ends with a NONE-stage barrier: only the semaphore carries the dependency
    auto lz = q1->createCommandList();
    std::unique_ptr<RenderTexture> Z;
    std::unique_ptr<RenderFramebuffer> fbZ;
    RenderCommandSemaphore *sigs[1] = { s1.get() };
    RenderCommandSemaphore *sigs2[1] = { s2.get() };
    RenderCommandSemaphore *waits[2] = { s1.get(), s2.get() };
    if (variant == 3) { // a second, independent producer whose semaphore s2 the consumer also waits for
        Z = mkTarget();
        fbZ = mkFb(Z.get());
        lz->begin();
        lz->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(Z.get(), RenderTextureLayout::COLOR_WRITE));
        lz->setFramebuffer(fbZ.get());
        lz->clearColor(0, RenderColor(1, 0, 0, 1));
        lz->end();
        say("C3: submission 0 clears Z on q1, signals s2");
        const RenderCommandList *z[1] = { lz.get() };
        q1->executeCommandLists(z, 1, nullptr, 0, sigs2, 1, nullptr);
    }
    auto l1 = q1->createCommandList(); auto l2 = q2->createCommandList();
    l1->begin(); s.recProduce(l1.get(), handoffNone ? RenderBarrierStage::NONE : RenderBarrierStage::GRAPHICS); l1->end();
    l2->begin(); s.recConsume(l2.get()); l2->end();
    say("C%d: submission 1 on q1 renders X (ends with %s barrier to SHADER_READ)%s", variant, handoffNone ? "a NONE-stage" : "a GRAPHICS-stage", variant >= 4 ? "" : ", signals binary s1");
    const RenderCommandList *a[1] = { l1.get() };
    const RenderCommandList *b[1] = { l2.get() };
#ifdef PLUME_VAL_HAS_TIMELINE
    if (variant >= 4) {
        // C4: timeline semaphore, consumer waits at GRAPHICS.  C5: timeline semaphore, consumer wait stage left at the backend default.
        auto tl = E.dev->createTimelineSemaphore(0);
        if (!tl) { say("createTimelineSemaphore returned null"); return 2; }
        RenderCommandSemaphore *ts[1] = { tl.get() };
        uint64_t one = 1;
        RenderBarrierStages ws[1] = { RenderBarrierStage::GRAPHICS };
        say("C%d: submission 1 signals timeline value 1; submission 2 waits for value 1 at %s", variant, variant == 4 ? "GRAPHICS" : "the default stage");
        q1->executeCommandListsEx(a, 1, nullptr, 0, nullptr, nullptr, ts, 1, &one, nullptr);
        q2->executeCommandListsEx(b, 1, ts, 1, variant == 4 ? ws : nullptr, &one, nullptr, 0, nullptr, f.get());
        q2->waitForCommandFence(f.get());
        uint64_t v = 0; vkGetSemaphoreCounterValue(E.vd->vk, static_cast<VulkanCommandSemaphore *>(tl.get())->vk, &v);
        say("C%d: timeline counter value after the run = %llu", variant, (unsigned long long)v);
        return finishAC(s, q2.get(), variant == 4 ? "C4" : "C5");
    }
#endif
    q1->executeCommandLists(a, 1, nullptr, 0, sigs, 1, nullptr);
    say("C%d: submission 2 on q2 waits on %s and samples X", variant, variant == 3 ? "s1 and s2 (two binary semaphores)" : "s1");
#ifdef PLUME_VAL_HAS_WAIT_STAGES
    {   // patched plume: every wait blocks at the stages that consume the image (graphics, which includes the fragment shader)
        RenderBarrierStages ws[2] = { RenderBarrierStage::GRAPHICS, RenderBarrierStage::GRAPHICS };
        say("C%d: (patched) wait stages given per semaphore: GRAPHICS", variant);
        q2->executeCommandListsEx(b, 1, waits, variant == 3 ? 2 : 1, ws, nullptr, nullptr, 0, nullptr, f.get());
    }
#else
    q2->executeCommandLists(b, 1, waits, variant == 3 ? 2 : 1, nullptr, 0, f.get());
#endif
    q2->waitForCommandFence(f.get());
    return finishAC(s, q2.get(), variant == 1 ? "C1" : variant == 2 ? "C2" : "C3");
}

// --- D -------------------------------------------------------------------------------------------------------------
static int patternD1() {
    Scene s; s.build();
    if (nullCheck("pipeline P2", static_cast<VulkanGraphicsPipeline *>(s.P2.get())->vk == VK_NULL_HANDLE)) return 2;
    auto q = E.dev->createCommandQueue(RenderCommandListType::DIRECT);
    auto f1 = E.dev->createCommandFence(); auto f2 = E.dev->createCommandFence();
    std::mutex m; std::condition_variable cv; bool submitted = false;
    std::unique_ptr<RenderCommandList> l1 = q->createCommandList(), l2 = q->createCommandList();
    std::thread t1([&] {
        l1->begin(); s.recProduce(l1.get()); l1->end();
        q->executeCommandLists(l1.get(), f1.get());
        { std::lock_guard<std::mutex> g(m); submitted = true; }
        cv.notify_all();
        say("D1: thread 1 recorded and submitted the draw into X");
    });
    std::thread t2([&] {
        { std::unique_lock<std::mutex> g(m); cv.wait(g, [&] { return submitted; }); }
        q->waitForCommandFence(f1.get());
        l2->begin(); s.recConsume(l2.get()); l2->end();
        q->executeCommandLists(l2.get(), f2.get());
        q->waitForCommandFence(f2.get());
        say("D1: thread 2 waited for thread 1's fence, recorded and submitted the sampling draw");
    });
    t1.join(); t2.join();
    return finishAC(s, q.get(), "D1");
}

static int patternD23(bool ownerRule) {
    auto T = mkTarget();
    auto *vt = static_cast<VulkanTexture *>(T.get());
    auto q = E.dev->createCommandQueue(RenderCommandListType::DIRECT);
    const int N = 2000;
    const int K = 20; // barriers recorded per list per iteration, to widen the window
    auto l1 = q->createCommandList(), l2 = q->createCommandList();
    std::atomic<int> go{ 0 }, ready{ 0 }, rec1{ 0 }, doneCount{ 0 };
    std::atomic<int> foreign1{ 0 }, foreign2{ 0 };
    std::mutex ownerMutex;
    // The owner rule of the test program: whoever records barriers for T takes ownerMutex, and the lists are recorded in the
    // order in which they will be submitted (list 1, then list 2).
    auto worker = [&](int id, RenderCommandList *l, RenderTextureLayout lay, std::atomic<int> &foreign) {
        for (int it = 0; it < N; it++) {
            while (go.load(std::memory_order_acquire) != it + 1) std::this_thread::yield();
            ready.fetch_add(1);
            while (ready.load() < 2 * (it + 1)) { }          // start recording together
            if (ownerRule && id == 2) while (rec1.load() <= it) std::this_thread::yield();
            {
                std::unique_lock<std::mutex> g(ownerMutex, std::defer_lock);
                if (ownerRule) g.lock();
                l->begin();
                for (int k = 0; k < K; k++) {
                    RenderTextureLayout want = (k & 1) ? RenderTextureLayout::SHADER_READ : lay;
                    l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(T.get(), want));
                    if (vt->textureLayout != want) foreign.fetch_add(1); // another thread's record-time store landed in between
                }
                l->end();
            }
            if (id == 1) rec1.store(it + 1);
            doneCount.fetch_add(1);
        }
    };
    std::thread t1(worker, 1, l1.get(), RenderTextureLayout::COLOR_WRITE, std::ref(foreign1));
    std::thread t2(worker, 2, l2.get(), RenderTextureLayout::GENERAL, std::ref(foreign2));
    for (int it = 0; it < N; it++) {
        go.store(it + 1, std::memory_order_release);
        while (doneCount.load() < 2 * (it + 1)) std::this_thread::yield();
        const RenderCommandList *ls[2] = { l1.get(), l2.get() };
        auto f = E.dev->createCommandFence();
        q->executeCommandLists(ls, 2, nullptr, 0, nullptr, 0, f.get());
        q->waitForCommandFence(f.get());
    }
    t1.join(); t2.join();
    say("%s: %d iterations x 2 threads x %d barriers on one texture; record-time layout overwritten by the other thread: %d (thread 1) + %d (thread 2) times; owner rule %s",
        ownerRule ? "D3" : "D2", N, K, foreign1.load(), foreign2.load(), ownerRule ? "ON" : "off");
    return 0;
}

// --- D4: lists recorded in one order, submitted in the other (layout state is read and written at record time) ----------
static int patternD4() {
    auto T = mkTarget();
    auto q = E.dev->createCommandQueue(RenderCommandListType::DIRECT);
    auto l1 = q->createCommandList(), l2 = q->createCommandList();
    l1->begin(); l1->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(T.get(), RenderTextureLayout::COLOR_WRITE)); l1->end();
    l2->begin(); l2->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(T.get(), RenderTextureLayout::SHADER_READ)); l2->end();
    say("D4: recorded list 1 (T -> COLOR_WRITE) then list 2 (T -> SHADER_READ, old layout taken from the texture's field = COLOR_WRITE); submitting list 2 first, then list 1");
    auto f = E.dev->createCommandFence();
    q->executeCommandLists(l2.get(), nullptr);
    q->executeCommandLists(l1.get(), f.get());
    q->waitForCommandFence(f.get());
    return 0;
}

// --- E -------------------------------------------------------------------------------------------------------------
static int patternE() {
    Scene s; s.build();
    const int N = 20;
    std::vector<std::unique_ptr<RenderPipeline>> ps;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) ps.push_back(mkPipe(s.S2.layout.get(), s.fsVs.get(), s.smpFs.get(), 1));
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::vector<VkRenderPass> rps;
    for (auto &p : ps) rps.push_back(static_cast<VulkanGraphicsPipeline *>(p.get())->renderPass);
    int distinct = 0;
    for (size_t i = 0; i < rps.size(); i++) { bool dup = false; for (size_t j = 0; j < i; j++) if (rps[j] == rps[i]) dup = true; if (!dup) distinct++; }
    say("E: %d identical graphics pipelines created in %.2f ms (%.3f ms each) (the pipelineCache argument is shown by the call trace; plume_vulkan.cpp:1652 passes VK_NULL_HANDLE unpatched)", N, ms, ms / N);
    say("E: distinct VkRenderPass handles owned by those %d pipelines: %d (framebuffer for X owns another: %s)", N, distinct,
        (static_cast<VulkanFramebuffer *>(s.fbX.get())->renderPass == rps[0]) ? "same" : "different");
#ifdef PLUME_VAL_HAS_PIPELINE_CACHE
    {
        std::vector<uint8_t> blob;
        bool ok = E.dev->savePipelineCache(blob);
        uint32_t hdr[4] = {};
        if (blob.size() >= 16) memcpy(hdr, blob.data(), 16);
        say("E: (patched) savePipelineCache -> %d, %zu bytes, header: size=%u version=%u vendor=0x%x device=0x%x", int(ok), blob.size(), hdr[0], hdr[1], hdr[2], hdr[3]);
        auto dev2 = E.iface->createDevice();
        bool ok2 = dev2 && dev2->loadPipelineCache(blob.data(), blob.size());
        say("E: (patched) a second device accepted the blob: %d", int(ok2));
    }
#endif
    // Second round to see whether lavapipe caches by itself.
    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) ps.push_back(mkPipe(s.S2.layout.get(), s.fsVs.get(), s.smpFs.get(), 1));
    auto t3 = std::chrono::steady_clock::now();
    say("E: a second batch of %d took %.2f ms", N, std::chrono::duration<double, std::milli>(t3 - t2).count());
    return 0;
}

// --- F: blend constant ---------------------------------------------------------------------------------------------
static int patternF() {
    auto X = mkTarget(); auto fbX = mkFb(X.get());
    auto vs = mkShader(SPV(spv_fs_vert)); auto fs = mkShader(SPV(spv_blend_frag));
    auto L = emptyLayout();
    RenderBlendDesc b; b.blendEnabled = true; b.srcBlend = RenderBlend::BLEND_FACTOR; b.dstBlend = RenderBlend::ZERO; b.blendOp = RenderBlendOperation::ADD;
    b.srcBlendAlpha = RenderBlend::BLEND_FACTOR; b.dstBlendAlpha = RenderBlend::ZERO; b.blendOpAlpha = RenderBlendOperation::ADD;
#ifdef PLUME_VAL_HAS_BLEND_CONSTANTS
    RenderGraphicsPipelineDesc gd;
    gd.pipelineLayout = L.get(); gd.vertexShader = vs.get(); gd.pixelShader = fs.get();
    gd.renderTargetCount = 1; gd.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM; gd.renderTargetBlend[0] = b;
    gd.dynamicBlendConstantsEnabled = true;
    auto P = E.dev->createGraphicsPipeline(gd);
#else
    auto P = mkPipe(L.get(), vs.get(), fs.get(), 1, RenderFormat::R8G8B8A8_UNORM, b);
#endif
    auto q = E.dev->createCommandQueue(RenderCommandListType::DIRECT);
    auto l = q->createCommandList();
    l->begin();
    l->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(X.get(), RenderTextureLayout::COLOR_WRITE));
    l->setFramebuffer(fbX.get());
    drawState(l.get(), P.get(), L.get(), nullptr);
#ifdef PLUME_VAL_HAS_BLEND_CONSTANTS
    { const float bc[4] = { 0.5f, 0.5f, 0.5f, 0.5f }; l->setBlendConstants(bc); say("F: (patched) setBlendConstants(0.5,0.5,0.5,0.5)"); }
#endif
    l->clearColor(0, RenderColor(0.5f, 0.5f, 0.5f, 0.5f));
    l->drawInstanced(3, 1, 0, 0);
    l->end();
    submitWait(q.get(), l.get());
    auto o = readTex(q.get(), X.get());
    say("F: blend factor BLEND_FACTOR, source (1,1,1,1), clear (0.5,0.5,0.5,0.5): pixel0 = %u %u %u %u (a constant of (c,c,c,c) would give c*255; plume has no setter, so the constant is the pipeline's zero)", o[0], o[1], o[2], o[3]);
#ifdef PLUME_VAL_HAS_BLEND_CONSTANTS
    return (near1(o[0], 128) && near1(o[1], 128) && near1(o[2], 128) && near1(o[3], 128)) ? 0 : 1;
#else
    return (o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0) ? 0 : 1;
#endif
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: plume_val <pattern>\n"); return 3; }
    gContinue = getenv("PLUME_VAL_CONTINUE") != nullptr;
    E.iface = CreateVulkanInterface();
    if (!E.iface) { say("CreateVulkanInterface returned null"); return 2; }
    E.dev = E.iface->createDevice();
    if (!E.dev) { say("createDevice returned null"); return 2; }
    E.vd = static_cast<VulkanDevice *>(E.dev.get());
    if (getenv("PLUME_VAL_TRACE")) installTrace();
    say("pattern %s on %s, instance apiVersion=1.2 requested by plume", argv[1], E.dev->getDescription().name.c_str());
    std::string p = argv[1];
    int rc = 3;
    if (p == "A1") rc = patternA(1);
    else if (p == "A2") rc = patternA(2);
    else if (p == "B1") rc = patternB(1);
    else if (p == "B2") rc = patternB(2);
    else if (p == "B3") rc = patternB(3);
    else if (p == "B4") rc = patternB(4);
    else if (p == "B5") rc = patternB(5);
    else if (p == "B6") rc = patternB(6);
    else if (p == "B7") rc = patternB(7);
    else if (p == "B8") rc = patternB8();
    else if (p == "C1") rc = patternC(1);
    else if (p == "C2") rc = patternC(2);
    else if (p == "C3") rc = patternC(3);
    else if (p == "C4") rc = patternC(4);
    else if (p == "C5") rc = patternC(5);
    else if (p == "D1") rc = patternD1();
    else if (p == "D2") rc = patternD23(false);
    else if (p == "D3") rc = patternD23(true);
    else if (p == "D4") rc = patternD4();
    else if (p == "E") rc = patternE();
    else if (p == "F") rc = patternF();
    if (getenv("PLUME_VAL_TRACE")) vtrace("totals: vkCreateRenderPass=%d vkCreateGraphicsPipelines=%d vkCmdBeginRenderPass=%d", nRenderPassCreated.load(), nPipelinesCreated.load(), nBeginRenderPass.load());
    say("exit code %d", rc);
    E.dev.reset(); E.iface.reset();
    return rc;
}
