// Written for this package from the design: nothing is copied from Azahar here.
#include "gpu/native/compose.h"

#include <algorithm>

namespace mh4n::gpu::native {
namespace {

rhi::Error Err(std::string message) {
    return rhi::Error{std::move(message)};
}

struct Set0Sig {
    UniformRange vs_pica, vs, fragment;
    const rhi::Buffer* lf = nullptr;
    const rhi::Buffer* general = nullptr;
    bool operator==(const Set0Sig& o) const {
        auto same = [](const UniformRange& a, const UniformRange& b) {
            return a.buffer == b.buffer && a.offset == b.offset && a.size == b.size;
        };
        return same(vs_pica, o.vs_pica) && same(vs, o.vs) && same(fragment, o.fragment) && lf == o.lf && general == o.general;
    }
};

struct Set1Sig {
    std::array<const rhi::Image*, 9> images{};
    std::array<rhi::ImageView, 9> views{};
    std::array<const rhi::Sampler*, 3> samplers{};
    bool operator==(const Set1Sig&) const = default;
};

struct Set2Sig {
    const rhi::Image* image = nullptr;
    bool operator==(const Set2Sig&) const = default;
};

}  // namespace

FixupGeneratorRegistry& FixupGeneratorRegistry::Global() {
    static FixupGeneratorRegistry registry;
    return registry;
}

rhi::GraphicsPipelineDesc BaseDescOf(const PipelineKey& key, const rhi::Backend& backend) {
    rhi::GraphicsPipelineDesc desc;
    desc.vertex_layout = key.vertex.input.ToLayout();
    desc.topology = key.topology;
    if (key.pass == PassKind::Normal) {
        if (key.color_format != rhi::Format::Undefined) {
            desc.color_format = backend.ChooseFormat(key.color_format, rhi::FormatUse::ColorTarget).format;
        }
        if (key.depth_format != rhi::Format::Undefined) {
            desc.depth_format = backend.ChooseFormat(key.depth_format, rhi::FormatUse::DepthTarget).format;
        }
    }
    desc.state = key.state;
    desc.sets = PipelineLayoutSets();
    desc.name = key.pass == PassKind::ShadowWrite ? "pica shadow write" : "pica draw";
    return desc;
}

struct DrawComposer::Impl {
    std::shared_ptr<rhi::Backend> backend;
    rhi::BindingArena arena;
    std::shared_ptr<rhi::Image> dummy_2d, dummy_cube, dummy_shadow;
    std::shared_ptr<rhi::Sampler> sampler;

    enum class Open : u8 { None, Target, NoAttach } open = Open::None;
    rhi::Image* open_color = nullptr;
    rhi::Image* open_depth = nullptr;
    rhi::Rect open_area{};
    u32 open_w = 0, open_h = 0;

    std::optional<Set0Sig> sig0;
    std::optional<Set1Sig> sig1;
    std::optional<Set2Sig> sig2;
    rhi::BindingSet* set0 = nullptr;
    rhi::BindingSet* set1 = nullptr;
    rhi::BindingSet* set2 = nullptr;

    explicit Impl(std::shared_ptr<rhi::Backend> b) : backend(std::move(b)), arena(backend) {}
    void Forget() {
        open = Open::None;
        open_color = open_depth = nullptr;
        sig0.reset();
        sig1.reset();
        sig2.reset();
        set0 = set1 = set2 = nullptr;
    }
};

DrawComposer::DrawComposer(std::shared_ptr<rhi::Backend> backend, PipelineProvider& provider)
    : impl_(std::make_unique<Impl>(std::move(backend))), provider_(provider), fixup_(&FixupGeneratorRegistry::Global()) {}

DrawComposer::~DrawComposer() = default;

rhi::Result<std::unique_ptr<DrawComposer>> DrawComposer::Create(std::shared_ptr<rhi::Backend> backend,
                                                                PipelineProvider& provider) {
    std::unique_ptr<DrawComposer> composer(new DrawComposer(std::move(backend), provider));
    Impl& impl = *composer->impl_;
    rhi::ImageDesc desc;
    desc.format = rhi::Format::Rgba8Unorm;
    desc.width = desc.height = 1;
    desc.name = "fragment dummy 2d";
    {
        auto image = impl.backend->CreateImage(desc);
        if (!image.ok()) return Err(image.error());
        impl.dummy_2d = *image;
    }
    desc.name = "fragment dummy cube";
    desc.cube = true;
    desc.layers = 6;
    {
        auto image = impl.backend->CreateImage(desc);
        if (!image.ok()) return Err(image.error());
        impl.dummy_cube = *image;
    }
    desc.name = "fragment dummy shadow";
    desc.cube = false;
    desc.layers = 1;
    desc.shadow_source = true;
    {
        auto image = impl.backend->CreateImage(desc);
        if (!image.ok()) return Err(image.error());
        impl.dummy_shadow = *image;
    }
    rhi::SamplerDesc sampler;
    sampler.min_filter = sampler.mag_filter = sampler.mip_filter = rhi::Filter::Nearest;
    auto made = impl.backend->CreateSampler(sampler);
    if (!made.ok()) return Err(made.error());
    impl.sampler = *made;
    return composer;
}

void DrawComposer::BeginRecording(rhi::Recorder& rec) {
    Impl& im = *impl_;
    im.Forget();
    rhi::ClearValue white;
    white.color = {1.0f, 1.0f, 1.0f, 1.0f};
    rec.ClearImage(*im.dummy_2d, white);
    for (u32 layer = 0; layer < 6; ++layer) {
        rec.ClearImage(*im.dummy_cube, white, 0, layer);
    }
    rec.ClearImage(*im.dummy_shadow, white);
}

void DrawComposer::BeginTargetPass(rhi::Recorder& rec, const rhi::TargetPassDesc& pass) {
    Impl& im = *impl_;
    if (im.open == Impl::Open::NoAttach) {
        rec.EndPass();
        im.open = Impl::Open::None;
    }
    const rhi::Rect area = pass.area;
    const bool continues = im.open == Impl::Open::Target && im.open_color == pass.color && im.open_depth == pass.depth &&
                           im.open_area.x == area.x && im.open_area.y == area.y && im.open_area.width == area.width &&
                           im.open_area.height == area.height;
    rec.BeginTargetPass(pass);
    if (!continues) {
        ++counters_.pass_begins;
        ++frame_.pass_begins;
    }
    im.open = Impl::Open::Target;
    im.open_color = pass.color;
    im.open_depth = pass.depth;
    im.open_area = area;
}

void DrawComposer::EndPass(rhi::Recorder& rec) {
    Impl& im = *impl_;
    if (im.open != Impl::Open::None) {
        rec.EndPass();
        im.open = Impl::Open::None;
    }
}

static FixupDecision FixupOf(const DrawDesc& d, const rhi::Capabilities& caps, const FixupGeneratorRegistry* registry) {
    return DecideFixup(d.needs.fixup_gs, d.vertex.kind, caps.geometry_shader, registry != nullptr && registry->Has());
}

PipelineKey DrawComposer::KeyOf(const DrawDesc& d) const {
    PipelineKey key;
    key.pass = d.needs.shadow_write_pass ? PassKind::ShadowWrite : PassKind::Normal;
    key.vertex = d.vertex;
    key.fixup = FixupOf(d, impl_->backend->Caps(), fixup_) == FixupDecision::Applied ? FixupStage::FixedGeometry : FixupStage::None;
    key.fragment.kind = FragmentKind::Specialised;
    if (d.fragment != nullptr) {
        key.fragment.key = *d.fragment;
    }
    key.state = d.state;
    key.topology = d.topology;
    if (key.pass == PassKind::Normal) {
        key.color_format = d.color_format;
        key.depth_format = d.depth_format;
    }
    return key;
}

rhi::Result<DrawResult> DrawComposer::Draw(rhi::Recorder& rec, const DrawDesc& d) {
    Impl& im = *impl_;
    if (d.fragment == nullptr) {
        return Err("DrawComposer::Draw: no fragment key");
    }
    const bool shadow_pass = d.needs.shadow_write_pass;
    if (shadow_pass && d.shadow_target == nullptr) {
        return Err("DrawComposer::Draw: a shadow-write draw needs the shadow source (DrawDesc::shadow_target)");
    }
    if (!shadow_pass && d.target.color == nullptr && d.target.depth == nullptr) {
        return Err("DrawComposer::Draw: a normal draw needs a target");
    }
    DrawResult out;
    ++counters_.draws;
    ++frame_.draws;

    // The fix-up stage and the pipeline.
    const FixupDecision fixup = FixupOf(d, im.backend->Caps(), fixup_);
    out.fixup = fixup == FixupDecision::Applied;
    if (fixup == FixupDecision::Stubbed) {
        out.fixup_stubbed = true;
        ++counters_.stubbed_fixup;
        ++frame_.stubbed_fixup;
    }
    const PipelineKey key = KeyOf(d);
    rhi::Result<PipelineHandle> handle = provider_.Lookup(key);
    if (!handle.ok()) {
        return Err(handle.error());
    }
    out.fallback = handle->fallback;
    if (out.fallback) {
        ++counters_.fallback_draws;
        ++frame_.fallback_draws;
    }

    // The pass, for the normal kind (the shadow kind begins after its set is written: a barrier must not land inside it).
    rhi::Image* twin = nullptr;
    if (!shadow_pass) {
        rhi::TargetPassDesc pass = d.target;
        pass.color_load = rhi::LoadOp::Load;
        pass.depth_load = rhi::LoadOp::Load;
        BeginTargetPass(rec, pass);
        if (d.needs.dst_copy) {
            if (d.target.color == nullptr) {
                return Err("DrawComposer::Draw: dst_copy needs a colour target");
            }
            twin = &rec.FeedbackCopy(*d.target.color);
            out.dst_copied = true;
            ++counters_.dst_copies;
            ++frame_.dst_copies;
        }
    } else if (im.open == Impl::Open::Target) {
        rec.EndPass();
        im.open = Impl::Open::None;
    }

    // Set 0: uniforms and tables.
    Set0Sig sig0;
    sig0.vs_pica = d.vs_pica_uniforms;
    sig0.vs = d.vs_uniforms;
    sig0.fragment = UniformRange{d.fragment_uniforms.buffer, d.fragment_uniforms.offset, d.fragment_uniforms.size};
    if (d.tables != nullptr) {
        sig0.lf = &d.tables->Buffer(fragment::TableRing::Lf);
        sig0.general = &d.tables->Buffer(fragment::TableRing::General);
    }
    if (im.sig0 && *im.sig0 == sig0 && im.set0 != nullptr) {
        out.set0_reused = true;
        ++counters_.sets_reused;
    } else {
        rhi::Result<rhi::BindingSet*> set = im.arena.Acquire(PipelineLayoutSets()[set::kUniforms]);
        if (!set.ok()) return Err(set.error());
        rhi::BindingSet& s = **set;
        auto put = [&](u32 binding, const UniformRange& range) -> rhi::Result<void> {
            if (range.buffer == nullptr) return rhi::Result<void>{};
            return s.SetUniformBuffer(binding, *range.buffer, range.offset, range.size);
        };
        for (const auto& step : {std::pair{binding::kVsPicaUniforms, sig0.vs_pica}, std::pair{binding::kVsUniforms, sig0.vs},
                                 std::pair{binding::kFragmentBlock, sig0.fragment}}) {
            rhi::Result<void> r = put(step.first, step.second);
            if (!r.ok()) return Err(r.error());
        }
        if (d.tables != nullptr) {
            rhi::Result<void> r = s.SetTexelBuffer(binding::kLutLf, *sig0.lf, fragment::kLutLfFormat);
            if (r.ok()) r = s.SetTexelBuffer(binding::kLutRg, *sig0.general, fragment::kLutRgFormat);
            if (r.ok()) r = s.SetTexelBuffer(binding::kLutRgba, *sig0.general, fragment::kLutRgbaFormat);
            if (!r.ok()) return Err(r.error());
        }
        im.set0 = &s;
        im.sig0 = sig0;
        ++counters_.sets_acquired;
    }

    // Set 1: the textures. Unit 0 goes to the class its type names; the other classes hold the dummies.
    using fragment::regenum::TextureType;
    const TextureType type = d.fragment->texture.texture0_type();
    const bool cube = type == TextureType::TextureCube || type == TextureType::ShadowCube;
    const bool shadow = type == TextureType::Shadow2D;
    Set1Sig sig1;
    auto pick = [&](const TextureBinding& unit, const rhi::Image* dummy) { return unit.image != nullptr ? unit.image : dummy; };
    sig1.images[binding::kTex0] = pick(d.units[0], im.dummy_2d.get());
    sig1.images[binding::kTex0Cube] = im.dummy_cube.get();
    sig1.images[binding::kTex0Shadow] = im.dummy_shadow.get();
    sig1.views[binding::kTex0] = rhi::ImageView::Sampled;
    sig1.views[binding::kTex0Cube] = rhi::ImageView::Sampled;
    sig1.views[binding::kTex0Shadow] = rhi::ImageView::ShadowStorage;
    if (d.units[0].image != nullptr) {
        const u32 slot = cube ? binding::kTex0Cube : (shadow ? binding::kTex0Shadow : binding::kTex0);
        sig1.images[slot] = d.units[0].image;
        sig1.views[slot] = shadow ? rhi::ImageView::ShadowStorage : d.units[0].view;
        if (slot != binding::kTex0) sig1.images[binding::kTex0] = im.dummy_2d.get();
    }
    sig1.images[binding::kTex1] = pick(d.units[1], im.dummy_2d.get());
    sig1.images[binding::kTex2] = pick(d.units[2], im.dummy_2d.get());
    sig1.views[binding::kTex1] = d.units[1].image != nullptr ? d.units[1].view : rhi::ImageView::Sampled;
    sig1.views[binding::kTex2] = d.units[2].image != nullptr ? d.units[2].view : rhi::ImageView::Sampled;
    sig1.images[binding::kTexColor] = twin != nullptr ? twin : im.dummy_2d.get();
    sig1.views[binding::kTexColor] = rhi::ImageView::Sampled;
    for (u32 i = 0; i < 3; ++i) {
        sig1.samplers[i] = d.units[i].sampler != nullptr ? d.units[i].sampler : im.sampler.get();
    }
    if (im.sig1 && *im.sig1 == sig1 && im.set1 != nullptr) {
        out.set1_reused = true;
        ++counters_.sets_reused;
    } else {
        rhi::Result<rhi::BindingSet*> set = im.arena.Acquire(PipelineLayoutSets()[set::kTextures]);
        if (!set.ok()) return Err(set.error());
        rhi::BindingSet& s = **set;
        for (u32 slot : {binding::kTex0, binding::kTex0Cube, binding::kTex0Shadow, binding::kTex1, binding::kTex2,
                         binding::kTexColor}) {
            rhi::Result<void> r = s.SetSampledImage(slot, *sig1.images[slot], sig1.views[slot]);
            if (!r.ok()) return Err(r.error());
        }
        const u32 sampler_slot[3] = {binding::kSampler0, binding::kSampler1, binding::kSampler2};
        for (u32 i = 0; i < 3; ++i) {
            rhi::Result<void> r = s.SetSampler(sampler_slot[i], *sig1.samplers[i]);
            if (!r.ok()) return Err(r.error());
        }
        im.set1 = &s;
        im.sig1 = sig1;
        ++counters_.sets_acquired;
    }

    // Set 2: the shadow write's storage image.
    Set2Sig sig2;
    sig2.image = shadow_pass ? d.shadow_target : im.dummy_shadow.get();
    if (im.sig2 && *im.sig2 == sig2 && im.set2 != nullptr) {
        out.set2_reused = true;
        ++counters_.sets_reused;
    } else {
        rhi::Result<rhi::BindingSet*> set = im.arena.Acquire(PipelineLayoutSets()[set::kShadow]);
        if (!set.ok()) return Err(set.error());
        rhi::Result<void> r = (*set)->SetStorageImage(binding::kShadowOut, *sig2.image, rhi::ImageView::ShadowStorage);
        if (!r.ok()) return Err(r.error());
        im.set2 = *set;
        im.sig2 = sig2;
        ++counters_.sets_acquired;
    }

    // The shadow-write pass: no attachments, the size of the shadow source.
    if (shadow_pass) {
        const u32 w = d.shadow_target->Width(), h = d.shadow_target->Height();
        if (im.open == Impl::Open::NoAttach && (im.open_w != w || im.open_h != h)) {
            rec.EndPass();
            im.open = Impl::Open::None;
        }
        if (im.open != Impl::Open::NoAttach) {
            rec.BeginPass(rhi::PassDesc{w, h});
            im.open = Impl::Open::NoAttach;
            im.open_w = w;
            im.open_h = h;
            im.open_color = im.open_depth = nullptr;
            ++counters_.pass_begins;
            ++frame_.pass_begins;
        }
    }

    // The draw.
    rec.SetPipeline(*handle->pipeline);
    rec.Bind(*im.set0, set::kUniforms);
    rec.Bind(*im.set1, set::kTextures);
    rec.Bind(*im.set2, set::kShadow);
    if (d.viewport) rec.SetViewport(*d.viewport);
    if (d.scissor) rec.SetScissor(*d.scissor);
    if (!d.vertex_buffers.empty()) {
        rec.SetVertexBuffers(0, d.vertex_buffers);
    }
    if (d.index_buffer != nullptr) {
        rec.SetIndexBuffer(*d.index_buffer, d.index_offset, d.index_type);
        rec.DrawIndexed(d.count, 1, d.first, d.base_vertex, 0);
    } else {
        rec.Draw(d.count, 1, d.first, 0);
    }
    return out;
}

std::vector<FrameCounter> DrawComposer::TakeFrameCounters() {
    std::vector<FrameCounter> out;
    out.push_back({"stubbed:fixup", frame_.stubbed_fixup});
    out.push_back({"dst_copies", frame_.dst_copies});
    out.push_back({"fallback_draws", frame_.fallback_draws});
    frame_ = ComposerCounters{};
    return out;
}

} // namespace mh4n::gpu::native
