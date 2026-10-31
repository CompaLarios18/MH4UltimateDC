// The compositor (N2b-05). Not derived from Azahar: the shaders and the drawing are ours (Azahar's renderers
// draw the screens with their own pipelines; the layouts are N2b-04's).
#include "present/compositor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gpu/backend/targets/upload.h"
#include "mh4n/log.h"
#include "present/shader_compile.h"

namespace mh4n::present {

namespace rhi = ::mh4n::gpu::rhi;

namespace {

constexpr const char* kLog = "Present";

// One quad of a screen: four vertices of a triangle strip. `pos` is in the target's NDC; `uv` in the source
// image's normalized coordinates; `bounds` the part of the image the sample is clamped to (x0, y0, x1, y1:
// the centres of the region's edge texels, so that linear filtering never reads outside the region);
// `alpha` the layer's opacity.
struct Vertex {
    float pos[2];
    float uv[2];
    float bounds[4];
    float alpha;
};
static_assert(sizeof(Vertex) == 36);

const char* const kVertexShader = R"(#version 450
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_bounds;
layout(location = 3) in float in_alpha;
layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out vec4 v_bounds;
layout(location = 2) flat out float v_alpha;
void main() {
    gl_Position = vec4(in_pos, 0.0, 1.0);
    v_uv = in_uv;
    v_bounds = in_bounds;
    v_alpha = in_alpha;
}
)";

// The screen is opaque whatever its alpha channel says; the layer's opacity is the blend's source alpha.
const char* const kFragmentShader = R"(#version 450
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler smp;
layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in vec4 v_bounds;
layout(location = 2) flat in float v_alpha;
layout(location = 0) out vec4 out_color;
void main() {
    vec2 uv = clamp(v_uv, v_bounds.xy, v_bounds.zw);
    out_color = vec4(texture(sampler2D(tex, smp), uv).rgb, v_alpha);
}
)";

const rhi::BindingSetDesc kSetDesc{{{0, rhi::BindingType::SampledImage}, {1, rhi::BindingType::Sampler}}};

constexpr u64 kVertexRingSize = 256 * 1024;
constexpr std::size_t kMaxIdleTextures = 8;

} // namespace

struct ComposeResult::CpuTexture {
    std::shared_ptr<rhi::Image> image;
    u32 width = 0;
    u32 height = 0;
    rhi::TimelineValue busy_until = 0; // the value of the last submission that sampled it
    u64 taken_in = 0;                  // the Compose that is using it (State::serial)
};

struct Compositor::State {
    std::shared_ptr<rhi::Backend> backend;
    std::vector<u32> vertex_spirv;
    std::vector<u32> fragment_spirv;
    std::map<rhi::Format, std::shared_ptr<rhi::Pipeline>> pipelines;
    std::shared_ptr<rhi::Sampler> nearest;
    std::shared_ptr<rhi::Sampler> linear;
    std::unique_ptr<rhi::Ring> vertices;
    std::unique_ptr<rhi::BindingArena> arena;
    std::vector<std::unique_ptr<ComposeResult::CpuTexture>> textures;
    u64 serial = 0;
    u32 textures_created = 0;
    rhi::TimelineValue last_value = 0;
    bool warned_invalid_top = false;
    bool warned_invalid_bottom = false;

    // The pipeline for a target of `format`, made on first use; null when the format cannot be drawn to.
    rhi::Pipeline* PipelineFor(rhi::Format format) {
        auto found = pipelines.find(format);
        if (found != pipelines.end()) {
            return found->second.get();
        }
        std::shared_ptr<rhi::Pipeline> made;
        if (backend->QueryFormat(format).attachment) {
            rhi::GraphicsPipelineDesc desc;
            desc.vertex.spirv = vertex_spirv;
            desc.fragment.spirv = fragment_spirv;
            desc.vertex_layout.bindings = {{0, sizeof(Vertex), false}};
            desc.vertex_layout.attributes = {
                {0, 0, rhi::Format::Rg32Float, offsetof(Vertex, pos)},
                {1, 0, rhi::Format::Rg32Float, offsetof(Vertex, uv)},
                {2, 0, rhi::Format::Rgba32Float, offsetof(Vertex, bounds)},
                {3, 0, rhi::Format::R32Float, offsetof(Vertex, alpha)},
            };
            desc.topology = rhi::Topology::TriangleStrip;
            desc.color_format = format;
            // src * a + dst * (1 - a) on the colour; the target's alpha stays what the clear made it.
            desc.state.blend_enabled = true;
            desc.state.src_color = rhi::BlendFactor::SrcAlpha;
            desc.state.dst_color = rhi::BlendFactor::OneMinusSrcAlpha;
            desc.state.color_op = rhi::BlendOp::Add;
            desc.state.src_alpha = rhi::BlendFactor::Zero;
            desc.state.dst_alpha = rhi::BlendFactor::One;
            desc.state.alpha_op = rhi::BlendOp::Add;
            desc.sets = {kSetDesc};
            desc.name = "compositor";
            rhi::Result<std::shared_ptr<rhi::Pipeline>> pipeline = backend->CreateGraphicsPipeline(desc);
            if (pipeline.ok()) {
                made = *pipeline;
            } else {
                MH4N_LOG(Error, kLog, "compositor pipeline for a target format: {}", pipeline.error());
            }
        }
        pipelines.emplace(format, made);
        return made.get();
    }

    // A texture of w x h for a CPU-pixel screen: an idle one of the pool, else a new one.
    ComposeResult::CpuTexture* AcquireTexture(u32 w, u32 h) {
        const rhi::TimelineValue completed = backend->CompletedValue();
        for (auto& texture : textures) {
            if (texture->width == w && texture->height == h && texture->taken_in != serial &&
                texture->busy_until <= completed) {
                texture->taken_in = serial;
                return texture.get();
            }
        }
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = w;
        desc.height = h;
        desc.name = "compositor screen upload";
        rhi::Result<std::shared_ptr<rhi::Image>> image = backend->CreateImage(desc);
        if (!image.ok()) {
            MH4N_LOG(Error, kLog, "cannot make a {}x{} texture for CPU pixels: {}", w, h, image.error());
            return nullptr;
        }
        auto texture = std::make_unique<ComposeResult::CpuTexture>();
        texture->image = *image;
        texture->width = w;
        texture->height = h;
        texture->taken_in = serial;
        ++textures_created;
        textures.push_back(std::move(texture));
        return textures.back().get();
    }

    // Drops idle textures beyond the pool's size (oldest first); none that the running Compose uses.
    void TrimTextures() {
        const rhi::TimelineValue completed = backend->CompletedValue();
        std::size_t idle = 0;
        for (const auto& texture : textures) {
            idle += texture->taken_in != serial && texture->busy_until <= completed;
        }
        for (auto it = textures.begin(); it != textures.end() && idle > kMaxIdleTextures;) {
            if ((*it)->taken_in != serial && (*it)->busy_until <= completed) {
                it = textures.erase(it);
                --idle;
            } else {
                ++it;
            }
        }
    }
};

namespace {

// The part of a screen a draw shows, `crop` in fractions of the displayed screen (screen_layout::RectF), and
// the source image it samples.
struct SourceImage {
    const rhi::Image* image = nullptr;
    // The stored region, in image pixels.
    double x = 0, y = 0, width = 0, height = 0;
    bool rotated = false;
    double scaled_width = 0;  // the displayed size in image pixels (scale * width, scale * height)
    double scaled_height = 0;
};

// A displayed position (fractions of the screen) as a normalized coordinate of the source image. Upright:
// image position = displayed position. Rotated90 (present.h): the pixel row y of the screen is the stored
// column scaled_height - 1 - y, and the pixel column x the stored row x, so the continuous mapping is
// (X, Y) -> (scaled_height - Y, X).
void SourceCoord(const SourceImage& src, double fx, double fy, float uv[2]) {
    const double X = fx * src.scaled_width;
    const double Y = fy * src.scaled_height;
    const double ix = src.rotated ? src.scaled_height - Y : X;
    const double iy = src.rotated ? X : Y;
    uv[0] = static_cast<float>((src.x + ix) / src.image->Width());
    uv[1] = static_cast<float>((src.y + iy) / src.image->Height());
}

// The rectangle `a` clipped to the target; width or height 0 when nothing is left.
rhi::Rect ClipToTarget(const screen_layout::Rect& r, u32 target_w, u32 target_h) {
    const u32 left = std::min(r.left, target_w);
    const u32 top = std::min(r.top, target_h);
    const u32 right = std::clamp(r.right, left, target_w);
    const u32 bottom = std::clamp(r.bottom, top, target_h);
    return rhi::Rect{static_cast<s32>(left), static_cast<s32>(top), right - left, bottom - top};
}

} // namespace

Compositor::Compositor(std::unique_ptr<State> state) : state_(std::move(state)) {}

Compositor::~Compositor() {
    if (state_ != nullptr && state_->backend != nullptr && state_->last_value != 0) {
        state_->backend->WaitIdle(state_->last_value);
    }
}

u32 Compositor::PipelineCount() const {
    u32 count = 0;
    for (const auto& entry : state_->pipelines) {
        count += entry.second != nullptr;
    }
    return count;
}

u32 Compositor::TexturesCreated() const {
    return state_->textures_created;
}

rhi::Result<std::unique_ptr<Compositor>> Compositor::Create(std::shared_ptr<rhi::Backend> backend) {
    if (backend == nullptr) {
        return rhi::Error{"Compositor::Create: no backend"};
    }
    auto state = std::make_unique<State>();
    state->backend = backend;
    std::string error;
    if (!CompileGlsl(GlslStage::Vertex, kVertexShader, state->vertex_spirv, error)) {
        return rhi::Error{"the compositor's vertex shader does not compile: " + error};
    }
    if (!CompileGlsl(GlslStage::Fragment, kFragmentShader, state->fragment_spirv, error)) {
        return rhi::Error{"the compositor's fragment shader does not compile: " + error};
    }
    for (const bool linear : {false, true}) {
        rhi::SamplerDesc desc;
        desc.min_filter = desc.mag_filter = linear ? rhi::Filter::Linear : rhi::Filter::Nearest;
        desc.mip_filter = rhi::Filter::Nearest;
        desc.max_lod = 0.0f;
        rhi::Result<std::shared_ptr<rhi::Sampler>> sampler = backend->CreateSampler(desc);
        if (!sampler.ok()) {
            return rhi::Error{"the compositor's sampler: " + sampler.error()};
        }
        (linear ? state->linear : state->nearest) = *sampler;
    }
    rhi::Result<std::unique_ptr<rhi::Ring>> ring =
        backend->CreateRing(kVertexRingSize, rhi::BufferUsage::Vertex, "compositor vertices");
    if (!ring.ok()) {
        return rhi::Error{"the compositor's vertex ring: " + ring.error()};
    }
    state->vertices = std::move(*ring);
    state->arena = std::make_unique<rhi::BindingArena>(backend);
    // The pipelines for the usual targets now: the offscreen image and the swapchain's usual format.
    for (const rhi::Format format : {rhi::Format::Rgba8Unorm, rhi::Format::Bgra8Unorm}) {
        if (backend->QueryFormat(format).attachment && state->PipelineFor(format) == nullptr) {
            return rhi::Error{"the compositor's pipeline could not be made (see the log)"};
        }
    }
    return std::unique_ptr<Compositor>(new Compositor(std::move(state)));
}

ComposeResult Compositor::Compose(rhi::Recorder& recorder, rhi::Image& target, const DisplayFrame& frame,
                                  const screen_layout::Layout& layout, const ComposeOptions& options) {
    State& s = *state_;
    ComposeResult result;
    rhi::Pipeline* pipeline = s.PipelineFor(target.GetFormat());
    if (pipeline == nullptr) {
        return result;
    }
    ++s.serial;
    s.TrimTextures();
    result.frame_images_ = std::make_shared<std::vector<std::shared_ptr<const void>>>();

    // ---- the screens that can be drawn, and the images they sample ----
    struct Source {
        const ScreenImage* screen = nullptr;
        SourceImage src;
        bool ok = false;
    };
    const auto prepare = [&](const std::optional<ScreenImage>& screen, bool& warned, const char* name) {
        Source out;
        if (!screen.has_value()) {
            return out;
        }
        if (!screen->Valid()) {
            if (!warned) {
                warned = true;
                MH4N_LOG(Warning, kLog, "the {} screen of a frame is not valid (sizes disagree): drawn black", name);
            }
            return out;
        }
        out.screen = &*screen;
        if (screen->IsBackendImage()) {
            const rhi::Image* image = static_cast<const rhi::Image*>(screen->image.get());
            const ImageRegion& region = screen->region;
            if (static_cast<u64>(region.x) + region.width > image->Width() ||
                static_cast<u64>(region.y) + region.height > image->Height()) {
                if (!warned) {
                    warned = true;
                    MH4N_LOG(Warning, kLog, "the {} screen's region lies outside its image: drawn black", name);
                }
                return out;
            }
            out.src.image = image;
            out.src.x = region.x;
            out.src.y = region.y;
            out.src.width = region.width;
            out.src.height = region.height;
            out.src.rotated = screen->orientation == Orientation::Rotated90;
            result.frame_images_->push_back(screen->image);
            if (screen->timeline_value != 0) {
                bool merged = false;
                for (rhi::Wait& wait : result.waits) {
                    if (wait.stage == rhi::Stage::FragmentShader) {
                        wait.value = std::max(wait.value, screen->timeline_value);
                        merged = true;
                    }
                }
                if (!merged) {
                    result.waits.push_back(rhi::Wait{screen->timeline_value, rhi::Stage::FragmentShader});
                }
            }
        } else {
            ComposeResult::CpuTexture* texture = s.AcquireTexture(screen->width, screen->height);
            if (texture == nullptr) {
                return out;
            }
            rhi::BufferImageRegion copy;
            copy.image_rect = rhi::Rect{0, 0, screen->width, screen->height};
            rhi::Result<void> uploaded =
                rhi::UploadToImage(*s.backend, recorder, *texture->image, copy,
                                   std::span<const u8>(screen->pixels->data(), screen->pixels->size()));
            if (!uploaded.ok()) {
                MH4N_LOG(Error, kLog, "uploading the {} screen's pixels: {}", name, uploaded.error());
                return out;
            }
            result.cpu_textures_.push_back(texture);
            out.src.image = texture->image.get();
            out.src.x = 0;
            out.src.y = 0;
            out.src.width = screen->width;
            out.src.height = screen->height;
        }
        out.src.scaled_width = screen->ScaledWidth();
        out.src.scaled_height = screen->ScaledHeight();
        out.ok = true;
        return out;
    };
    // The uploads come first: a copy ends the target's pass.
    const Source top = prepare(frame.top, s.warned_invalid_top, "top");
    const Source bottom = prepare(frame.bottom, s.warned_invalid_bottom, "bottom");

    // ---- the pass: clear to black, then the draws ----
    const u32 target_w = target.Width();
    const u32 target_h = target.Height();
    rhi::TargetPassDesc pass;
    pass.color = &target;
    pass.color_load = rhi::LoadOp::Clear;
    pass.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    recorder.BeginTargetPass(pass);
    result.drawn = true;

    const rhi::Sampler& sampler = options.linear_filter ? *s.linear : *s.nearest;
    const auto draw = [&](const Source& source, const screen_layout::Rect& rect, const screen_layout::RectF& crop,
                          float opacity) {
        if (!source.ok) {
            return;
        }
        const rhi::Rect scissor = ClipToTarget(rect, target_w, target_h);
        if (scissor.width == 0 || scissor.height == 0 || rect.right <= rect.left || rect.bottom <= rect.top) {
            return;
        }
        const SourceImage& src = source.src;
        Vertex v[4];
        const double w = target_w;
        const double h = target_h;
        // Strip order: top-left, top-right, bottom-left, bottom-right.
        for (int i = 0; i < 4; ++i) {
            const double sx = i & 1;
            const double sy = i >> 1;
            v[i].pos[0] = static_cast<float>(2.0 * (sx != 0 ? rect.right : rect.left) / w - 1.0);
            v[i].pos[1] = static_cast<float>(2.0 * (sy != 0 ? rect.bottom : rect.top) / h - 1.0);
            const double fx = crop.left + sx * (crop.right - crop.left);
            const double fy = crop.top + sy * (crop.bottom - crop.top);
            SourceCoord(src, fx, fy, v[i].uv);
            v[i].bounds[0] = static_cast<float>((src.x + 0.5) / src.image->Width());
            v[i].bounds[1] = static_cast<float>((src.y + 0.5) / src.image->Height());
            v[i].bounds[2] = static_cast<float>((src.x + src.width - 0.5) / src.image->Width());
            v[i].bounds[3] = static_cast<float>((src.y + src.height - 0.5) / src.image->Height());
            v[i].alpha = opacity;
        }
        rhi::Result<rhi::RingAllocation> allocation = s.vertices->Map(sizeof(v), 16);
        if (!allocation.ok()) {
            MH4N_LOG(Error, kLog, "the vertex ring: {}", allocation.error());
            return;
        }
        std::memcpy(allocation->data.data(), v, sizeof(v));
        if (!s.vertices->Commit(sizeof(v)).ok()) {
            MH4N_LOG(Error, kLog, "the vertex ring cannot commit");
            return;
        }
        rhi::Result<rhi::BindingSet*> set = s.arena->Acquire(kSetDesc);
        if (!set.ok()) {
            MH4N_LOG(Error, kLog, "binding set: {}", set.error());
            return;
        }
        if (!(*set)->SetSampledImage(0, *src.image, rhi::ImageView::Sampled).ok() ||
            !(*set)->SetSampler(1, sampler).ok()) {
            MH4N_LOG(Error, kLog, "binding a screen's image failed");
            return;
        }
        recorder.SetPipeline(*pipeline);
        recorder.Bind(**set, 0);
        const rhi::VertexBufferBinding binding{s.vertices->GetBuffer().get(), allocation->offset};
        recorder.SetVertexBuffers(0, std::span<const rhi::VertexBufferBinding>(&binding, 1));
        rhi::Viewport viewport;
        viewport.width = static_cast<float>(target_w);
        viewport.height = static_cast<float>(target_h);
        recorder.SetViewport(viewport);
        recorder.SetScissor(scissor);
        recorder.Draw(4);
    };

    if (layout.top_shown) {
        draw(top, layout.top, screen_layout::RectF{0.0f, 0.0f, 1.0f, 1.0f}, std::clamp(layout.top_opacity, 0.0f, 1.0f));
    }
    if (layout.bottom_shown) {
        // The further views are drawn at the bottom screen's opacity (platform.h: ScreenOverlay::View).
        const float opacity = std::clamp(layout.bottom_opacity, 0.0f, 1.0f);
        draw(bottom, layout.bottom, layout.bottom_crop, opacity);
        const u32 views = std::min<u32>(layout.view_count, static_cast<u32>(layout.views.size()));
        for (u32 i = 0; i < views; ++i) {
            draw(bottom, layout.views[i].screen, layout.views[i].crop, opacity);
        }
    }

    if (options.ui != nullptr) {
        options.ui->Record(&recorder, target_w, target_h);
    }
    return result;
}

rhi::TimelineValue Compositor::Submit(rhi::Recorder&& recorder, ComposeResult&& result) {
    State& s = *state_;
    const rhi::TimelineValue value = s.backend->Submit(std::move(recorder), result.waits);
    s.last_value = std::max(s.last_value, value);
    for (ComposeResult::CpuTexture* texture : result.cpu_textures_) {
        texture->busy_until = value;
    }
    if (result.frame_images_ != nullptr && !result.frame_images_->empty()) {
        s.backend->Retire(result.frame_images_, value);
    }
    result.cpu_textures_.clear();
    result.frame_images_.reset();
    return value;
}

} // namespace mh4n::present
