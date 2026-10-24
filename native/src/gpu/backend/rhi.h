// The renderer-facing interface of the GPU backend (N2-26, decision D4 option 2b; rhi-spike.md 9.4).
//
// Written once and FROZEN when N2-26 is merged: it changes only through the orchestrator. The other
// modules of native/src/gpu/backend/ implement the operations this file declares for them; the owner of
// each group is named in the comment above it. A function declared here and defined by a module that is
// not merged yet must not be called.
//
// Operation level (9.4 item 1): this file speaks the renderer's operations (buffers, rings, passes,
// pipelines, bindings, submissions, readbacks), never an image layout, a render-pass object, a
// descriptor set or a pipeline layout, and it names no Vulkan or third-party type. The Vulkan side the
// boundary leaves out (the raw handles, the recording command buffer, the queue lock, wrapped swapchain
// images) is vulkan_access.h. Only the standard library and mh4n/ headers are included here.
//
// Every class below has an impl() accessor for the modules of native/src/gpu/backend/ (their private state is
// core/impl.h); nothing else uses it.
//
// Threads. One device is shared by several threads (D56). Every image has one owning thread, images
// move between threads only by a timeline value with its own wait stage, and recorders are submitted in
// the order they were begun on each thread (the owner rule, 9.4 item 5, OwnerRule below). Everything
// else is thread-safe as the comment on each operation says; nothing is thread-safe that does not say
// so. Resources (buffers, pipelines, binding sets, images) are not destroyed by the backend while the
// GPU may still use them: the caller keeps one alive until the GPU has passed the value of the last
// submission that used it (Backend::Retire hands that job to the backend).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::gpu::rhi {

// ================================================================================================
// Results
// ================================================================================================

// An error message. `Result<T>` is a value or an error; an error is never thrown.
struct Error {
    std::string message;
};

template <typename T>
class Result {
public:
    Result(T value) : v_(std::move(value)) {}
    Result(Error error) : v_(std::move(error)) {}

    bool ok() const { return v_.index() == 0; }
    explicit operator bool() const { return ok(); }
    // Only when ok(); using it otherwise ends the program (std::bad_variant_access).
    T& value() { return std::get<0>(v_); }
    const T& value() const { return std::get<0>(v_); }
    T& operator*() { return value(); }
    const T& operator*() const { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    // Only when !ok(); "" when ok().
    const std::string& error() const {
        static const std::string kNone;
        return ok() ? kNone : std::get<1>(v_).message;
    }

private:
    std::variant<T, Error> v_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : ok_(false), error_(std::move(error.message)) {}

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }
    const std::string& error() const { return error_; }

private:
    bool ok_ = true;
    std::string error_;
};

// ================================================================================================
// Core (N2-26): time, capabilities, the instance and the backend
// ================================================================================================

// The value of the backend's timeline: each submission signals the next one and returns it. Values rise
// in submission order across all threads. 0 means "nothing": it has always passed.
using TimelineValue = u64;

// The pipeline stage a wait or a barrier applies at (a wait carries its own stage, 9.4 item 7). A barrier
// uses the exact stage. A wait is expressed in the coarser groups the vendored queue knows (graphics,
// compute, transfer): VertexShader, FragmentShader and AllGraphics are the same group there, so naming the
// stage that reads costs nothing and stays correct if a finer mapping is added.
enum class Stage : u8 {
    VertexShader,
    FragmentShader,
    ComputeShader,
    Transfer,
    AllGraphics,
};

// "Block at `stage` until the backend's timeline has reached `value`." A wait at too early a stage is
// a data hazard the validation layer reports (rhi-spike.md 6.9, C2 and C5): name the stage that reads.
struct Wait {
    TimelineValue value = 0;
    Stage stage = Stage::AllGraphics;
};

// ---- Capabilities (9.4 item 2) -------------------------------------------------------------------

// The bits of Capabilities the mask can turn off. The name of a bit (CapName) is the field's name in
// Capabilities and the word MH4N_RHI_MASK uses.
enum class Cap : u8 {
    D24S8,              // "d24s8": a 24-bit depth with 8-bit stencil format can be an attachment and be sampled
    Interlock,          // "interlock": fragment shader pixel interlock
    IndexUint8,         // "index_uint8": 8-bit indices
    CustomBorderColor,  // "custom_border_color": samplers with a border colour of any value
    TriangleFans,       // "triangle_fans": the triangle-fan topology
    LogicOp,            // "logic_op": hardware logic ops
    DynamicRendering,   // "dynamic_rendering"
    PipelineLibrary,    // "pipeline_library"
    Eds3Blend,          // "eds3_blend": blend enable, equation, write mask and logic-op enable as dynamic state
    Bc7,                // "bc7": BC7 textures
    Timeline,           // "timeline": timeline semaphores (without them every submission uses a fence)
    PresentWait,        // "present_wait": a presentation id and a wait for it
    DirectUpload,       // "direct_upload": the host can write device memory directly (else uploads are staged)
};
inline constexpr u32 kCapCount = 13;

// The name of a bit: "d24s8", "interlock", "index_uint8", "custom_border_color", "triangle_fans",
// "logic_op", "dynamic_rendering", "pipeline_library", "eds3_blend", "bc7", "timeline", "present_wait",
// "direct_upload".
std::string_view CapName(Cap cap);

// A set of capability bits to turn OFF. A mask only ever turns bits off, never on.
struct CapabilityMask {
    u32 bits = 0;

    constexpr bool Has(Cap cap) const { return (bits >> static_cast<u32>(cap)) & 1u; }
    constexpr CapabilityMask With(Cap cap) const { return CapabilityMask{bits | (1u << static_cast<u32>(cap))}; }
    constexpr CapabilityMask Union(CapabilityMask other) const { return CapabilityMask{bits | other.bits}; }
    static constexpr CapabilityMask All() { return CapabilityMask{(1u << kCapCount) - 1u}; }
    constexpr bool operator==(const CapabilityMask&) const = default;

    // "d24s8,bc7" or "all" (case-insensitive, blanks around a name ignored; "" is the empty mask). An
    // unknown name is an error that lists the valid ones.
    static Result<CapabilityMask> Parse(std::string_view list);
    // "d24s8,bc7"; "all" when every bit is set; "" when none is.
    std::string ToString() const;
};

// What the chosen device can do, after the mask (the mask is applied after the query, so a masked bit
// is also not enabled on the device and its fallback really runs). Filled from the device's own
// queries, never from constants.
struct Capabilities {
    // ---- the bits the mask can turn off ----
    bool d24s8 = false;
    bool interlock = false;
    bool index_uint8 = false;
    bool custom_border_color = false;
    bool triangle_fans = false;
    bool logic_op = false;
    bool dynamic_rendering = false;   // reported; no path uses it: every device takes render passes
    bool pipeline_library = false;    // reported; no path uses it: the pipeline cache alone
    bool eds3_blend = false;          // reported; no path uses it: blend state is baked into pipelines
    bool bc7 = false;
    bool timeline = false;
    bool present_wait = false;
    bool direct_upload = false;   // some host-visible, device-local memory lives in a heap as large as the biggest device-local one

    // ---- values ----
    bool geometry_shader = false;
    u32 stride_alignment = 1;               // vertex strides must be multiples of it (the portability subset's value; 1 otherwise)
    // MoltenVK's or KosmicKrisp's driver id, or Apple's vendor id. On such devices fans are rewritten as
    // lists and 8-bit indices widened whatever the two bits say (9.4 item 11).
    bool apple = false;
    bool primitive_restart_always_on = false;  // true wherever `apple` is (9.4 item 10; MoltenVK always restarts)
    bool portability_subset = false;        // the device lists the portability subset extension
    u32 driver_id = 0;
    std::string driver_name;
    std::string device_name;
    u32 api_version = 0;
    float timestamp_period = 0.0f;          // nanoseconds per timestamp tick
    u32 max_texel_buffer_elements = 0;
    u32 vendor_id = 0;
    u32 device_index = 0;                   // the device's index in the instance's enumeration order
    CapabilityMask masked;                  // the bits that were turned off (BackendOptions::mask plus MH4N_RHI_MASK)

    constexpr bool Has(Cap cap) const {
        switch (cap) {
        case Cap::D24S8: return d24s8;
        case Cap::Interlock: return interlock;
        case Cap::IndexUint8: return index_uint8;
        case Cap::CustomBorderColor: return custom_border_color;
        case Cap::TriangleFans: return triangle_fans;
        case Cap::LogicOp: return logic_op;
        case Cap::DynamicRendering: return dynamic_rendering;
        case Cap::PipelineLibrary: return pipeline_library;
        case Cap::Eds3Blend: return eds3_blend;
        case Cap::Bc7: return bc7;
        case Cap::Timeline: return timeline;
        case Cap::PresentWait: return present_wait;
        case Cap::DirectUpload: return direct_upload;
        }
        return false;
    }
};

// ---- The owner rule (9.4 item 5) -----------------------------------------------------------------

// The rule a breach broke. Clause numbers are those of rhi-spike.md 9.4 item 5.
enum class OwnerRule : u8 {
    OneOpenRecorder,  // a thread began a second recorder before submitting or dropping its first
    SubmissionOrder,  // clause 3: a recorder was submitted after a later one of the same thread
    RequiredWait,     // clause 2: a submission lacks the wait that covers an image handed over to the thread
    ForeignThread,    // clause 1: a thread recorded on an image it does not own
    HandOver,         // clause 2: an image was handed over that cannot be (an attachment of an open pass, a depth-stencil target)
    WrongThread,      // a wrapper's call that must run on one thread ran on another (the renderer or present thread)
};

struct OwnerRuleBreach {
    OwnerRule rule = OwnerRule::OneOpenRecorder;
    std::string message;
};
using OwnerRuleHandler = std::function<void(const OwnerRuleBreach&)>;

// Replaces the process-wide handler and returns the previous one. An empty handler restores the default,
// which logs the breach at Critical and aborts. The handler runs on the thread that breached the rule,
// and the call then returns normally (a test handler counts breaches). The checks that call it exist
// only when the backend is built with MH4N_RHI_CHECKS (the CMake option of the core module: ON by
// default in every build type, OFF only for a shipping build).
OwnerRuleHandler SetOwnerRuleHandler(OwnerRuleHandler handler);
// Calls the handler. For the checks of the other modules and of the code that wraps the backend.
void ReportOwnerRuleBreach(OwnerRule rule, std::string message);

// ---- Instance (R1) -------------------------------------------------------------------------------

enum class DebugSeverity : u8 { Warning, Error };
using DebugMessageHandler = std::function<void(DebugSeverity, std::string_view)>;

// What CreateInstance enables. An instance-level surface extension is enabled only when these options ask
// for it, and then only if the loader offers it (the headless flag below, and the caller's extension
// names, for example SDL3's); with default options the instance enables no surface extension, no
// swapchain support (that is a device extension, and only with a surface) and no debug-utils extension.
struct InstanceOptions {
    // Instance extensions the caller needs by name (for example the ones SDL3 lists for a window). Each
    // is required: one the loader does not offer fails CreateInstance with a message naming it.
    std::vector<std::string> extensions;

    // Enable the headless-surface extension and the generic surface extension it needs, each only if the
    // loader offers it (a test surface that needs no window). Off by default.
    bool headless_surface = false;

    // Enable the validation layer if it is installed.
    bool validation_layer = false;

    // The debug messenger on request: when set, the debug-utils extension is enabled (when offered) and
    // every warning and error message of the driver or the layers is delivered to this handler, on
    // whatever thread the message arises. One live debug-utils setting per process: a second instance
    // made with and a first made without it lose each other's object names.
    DebugMessageHandler debug_messages;
};

class Instance {
public:
    struct Impl;
    explicit Instance(std::unique_ptr<Impl> impl);
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    // The instance extensions that were enabled, by name.
    const std::vector<std::string>& EnabledExtensions() const;

    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// Loads the Vulkan loader at run time and makes an instance. With no loader, or when the instance cannot
// be made, the result is an error whose message says so; nothing else in the process fails (R6).
Result<std::shared_ptr<Instance>> CreateInstance(const InstanceOptions& options = {});

// A surface the caller made on the instance (see vulkan_access.h: SurfaceFrom). 0 is "none".
struct SurfaceHandle {
    u64 value = 0;
    explicit operator bool() const { return value != 0; }
};

struct BackendOptions {
    // A surface the device must be able to present to. With one, the device enables the swapchain
    // extension, the queue family is chosen among those that can present to it, and the presentation
    // extensions are taken where offered and not masked.
    SurfaceHandle surface;

    // Picks the first device whose "<device name> (<driver name>)" contains this text. Empty: the
    // MH4N_VULKAN_DEVICE environment variable, and when that is empty too, the backend's own choice.
    // Two devices with the same name and different drivers (two "Apple M2") are told apart by the
    // driver's name. No device matches: CreateBackend fails with the list of devices.
    std::string device;

    // Capability bits to turn off, together with MH4N_RHI_MASK (a comma list of bit names, or "all").
    CapabilityMask mask;
};

// ---- Formats (N2-27 owns QueryFormat and ChooseFormat; the enumeration is part of the interface) ----

// A format of a buffer's texels, a vertex attribute or an image. Four of them are PICA names that the
// chooser may map to another format (Rgb8, Rgb565, Rgb5A1, Rgba4: see ChooseFormat) and D24S8, which the
// chooser may map to D32S8; every other member is exactly what the device stores.
enum class Format : u8 {
    Undefined = 0,
    // PICA colour formats (logical: ChooseFormat gives the format used)
    Rgb8,
    Rgb565,
    Rgb5A1,
    Rgba4,
    // colour
    R8Unorm,
    Rg8Unorm,
    Rgba8Unorm,
    Rgba8Snorm,
    Bgra8Unorm,
    R5G6B5,  // the 16-bit formats in both component orders (the first letter is the lowest bits)
    B5G6R5,
    R5G5B5A1,
    B5G5R5A1,
    A1R5G5B5,
    R4G4B4A4,
    B4G4R4A4,
    R32Uint,  // the shadow write's storage view
    Rgba16Float,
    Bc7Unorm,
    // depth and stencil
    D16Unorm,
    D24S8,  // PICA: 24-bit depth, 8-bit stencil (when the device lacks it, D32S8 is used)
    D32Float,
    D32S8,
    // vertex attributes (integer where the device has no scaled formats, X3)
    R32Float,
    Rg32Float,
    Rgb32Float,
    Rgba32Float,
    R8Uint,
    R8Sint,
    Rg8Uint,
    Rg8Sint,
    Rgba8Uint,
    Rgba8Sint,
    R16Uint,
    R16Sint,
    Rg16Sint,
    Rgba16Sint,
};

// ---- Resources: buffers, rings, binding sets, pipelines ------------------------------------------

// Where a buffer's memory lives.
enum class Heap : u8 {
    Device,    // device-local; not mapped
    Upload,    // host-visible, persistently mapped, the host writes and the GPU reads
    Readback,  // host-visible, persistently mapped, the GPU writes and the host reads
};

// What a buffer can be used for. A buffer in the Upload heap can be a source of copies, one in the
// Readback heap a destination, one in the Device heap both.
enum class BufferUsage : u32 {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Texel = 1u << 4,     // a uniform texel buffer (formatted reads in a shader)
    Transfer = 1u << 5,  // takes part in CopyBuffer and the copies between buffers and images
};
constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr BufferUsage operator&(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<u32>(a) & static_cast<u32>(b));
}
constexpr bool HasUsage(BufferUsage set, BufferUsage bit) { return (set & bit) != BufferUsage::None; }

struct BufferDesc {
    u64 size = 0;
    Heap heap = Heap::Device;
    BufferUsage usage = BufferUsage::None;
    std::string name;  // for logs and the validation layer's messages
};

class Buffer {
public:
    struct Impl;
    explicit Buffer(std::unique_ptr<Impl> impl);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    u64 Size() const;
    Heap GetHeap() const;
    BufferUsage Usage() const;
    // The whole buffer's memory for the Upload and Readback heaps (stable for the buffer's life); empty
    // for the Device heap. Reading memory the GPU wrote needs Invalidate first.
    std::span<u8> Mapped() const;
    // Make host writes to [offset, offset + size) visible to the GPU / GPU writes visible to the host.
    // No-ops on coherent memory; call them anyway.
    void Flush(u64 offset, u64 size);
    void Invalidate(u64 offset, u64 size);

    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// What Ring::Map hands out.
struct RingAllocation {
    std::span<u8> data;  // `size` bytes, writable
    u64 offset = 0;      // of the first byte in Ring::GetBuffer()
    bool wrapped = false;  // this allocation restarted at the beginning of the buffer
};

// A persistently mapped ring buffer in the Upload heap (5.8). Map and Commit are used by one thread at a
// time. Space is reused once the GPU has passed the timeline value of the submission that used it.
class Ring {
public:
    struct Impl;
    explicit Ring(std::unique_ptr<Impl> impl);
    ~Ring();
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    // Reserves `size` bytes at a multiple of `alignment` (a power of two). When the end of the buffer is
    // reached it wraps to the start (RingAllocation::wrapped) and, if the space there is still in use by
    // a submission the GPU has not finished, waits for that submission. Space committed in a recorder
    // that has not been submitted yet cannot be waited for: the call then fails with a message, as it
    // does for a size above the buffer's.
    Result<RingAllocation> Map(u64 size, u64 alignment = 1);
    // Marks the first `size` bytes of the last Map as written and used by the calling thread's open
    // recorder: they are free again once that recorder's submission has passed. Fails when the thread
    // has no open recorder, or `size` is above what Map reserved. (If the recorder is dropped without a
    // submission the space is free at once.)
    Result<void> Commit(u64 size);

    const std::shared_ptr<Buffer>& GetBuffer() const;
    u64 Size() const;

    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// The kinds of resource a binding set can hold. The core implements UniformBuffer and StorageBuffer; the
// others belong to the targets module (images, samplers, texel buffers).
enum class BindingType : u8 {
    UniformBuffer,
    StorageBuffer,
    TexelBuffer,
    SampledImage,
    Sampler,
    StorageImage,
};

struct BindingSlot {
    u32 binding = 0;  // the shader's binding number
    BindingType type = BindingType::UniformBuffer;
    bool operator==(const BindingSlot&) const = default;
};

// The shape of one set of bindings: which binding numbers hold which kinds of resource. Every slot is
// visible to every shader stage and holds one resource (no arrays).
struct BindingSetDesc {
    std::vector<BindingSlot> slots;
    bool operator==(const BindingSetDesc&) const = default;
};

class Image;
class Sampler;
enum class ImageView : u8;

// The resources one set of a pipeline's shaders reads, written before the set is bound and not while a
// submission that uses it is in flight. Its shape (BindingSetDesc) must equal the pipeline's shape for
// that set index.
class BindingSet {
public:
    struct Impl;
    explicit BindingSet(std::unique_ptr<Impl> impl);
    ~BindingSet();
    BindingSet(const BindingSet&) = delete;
    BindingSet& operator=(const BindingSet&) = delete;

    const BindingSetDesc& Desc() const;

    // Core. `offset` must be a multiple of the device's uniform / storage buffer offset alignment; the
    // buffer must have been made with the matching usage; offset + size must lie inside the buffer.
    Result<void> SetUniformBuffer(u32 binding, const Buffer& buffer, u64 offset, u64 size);
    Result<void> SetStorageBuffer(u32 binding, const Buffer& buffer, u64 offset, u64 size);
    // Targets (N2-27).
    Result<void> SetTexelBuffer(u32 binding, const Buffer& buffer, Format format);
    Result<void> SetSampledImage(u32 binding, const Image& image, ImageView view);
    Result<void> SetSampler(u32 binding, const Sampler& sampler);
    Result<void> SetStorageImage(u32 binding, const Image& image, ImageView view);

    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

inline constexpr u32 kMaxVertexBindings = 13;    // 12 buffers of attributes and the fixed one (9.4 item 10)
inline constexpr u32 kMaxVertexAttributes = 16;

// SPIR-V (from glslang or the fragment generator).
struct ShaderCode {
    std::span<const u32> spirv;
    std::string entry = "main";
};

// An attribute format is one of the vertex-capable members of Format. The vertex layout takes the
// formats N2-05 produces: integer formats where there are no scaled ones (X3).
struct VertexBinding {
    u32 binding = 0;
    u32 stride = 0;  // a multiple of Capabilities::stride_alignment
    bool per_instance = false;
};
struct VertexAttribute {
    u32 location = 0;
    u32 binding = 0;
    Format format{};
    u32 offset = 0;
};
struct VertexLayout {
    std::vector<VertexBinding> bindings;
    std::vector<VertexAttribute> attributes;
};

enum class Topology : u8 { PointList, LineList, LineStrip, TriangleList, TriangleStrip, TriangleFan };

enum class BlendFactor : u8 {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    ConstantColor,
    OneMinusConstantColor,
    // The PICA has them; the vendored backend cannot express them (it maps only the constant-colour
    // pair), so a pipeline that uses one fails to be created with a message until a patch adds them.
    ConstantAlpha,
    OneMinusConstantAlpha,
    SrcAlphaSaturate,
};
enum class BlendOp : u8 { Add, Subtract, ReverseSubtract, Min, Max };
enum class LogicOp : u8 {
    Clear, Set, Copy, CopyInverted, NoOp, Invert, And, Nand, Or, Nor, Xor, Equiv, AndReverse, AndInverted, OrReverse, OrInverted,
};
enum class CompareOp : u8 { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class StencilOp : u8 { Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap };
enum class CullMode : u8 { None, Front, Back };
enum class FrontFace : u8 { Clockwise, CounterClockwise };

inline constexpr u8 kWriteRed = 1, kWriteGreen = 2, kWriteBlue = 4, kWriteAlpha = 8, kWriteAll = 15;

struct StencilFaceState {
    StencilOp fail = StencilOp::Keep;
    StencilOp pass = StencilOp::Keep;
    StencilOp depth_fail = StencilOp::Keep;
    CompareOp compare = CompareOp::Always;
};

// The state the pipeline bakes in. The blend constants, the stencil reference, compare mask and write
// mask, the viewport and the scissor are always dynamic (Recorder::SetBlendConstants, SetStencilReference,
// SetStencilMasks, SetViewport, SetScissor), so a different value never makes another pipeline.
struct FixedFunctionState {
    bool blend_enabled = false;
    BlendFactor src_color = BlendFactor::One;
    BlendFactor dst_color = BlendFactor::Zero;
    BlendOp color_op = BlendOp::Add;
    BlendFactor src_alpha = BlendFactor::One;
    BlendFactor dst_alpha = BlendFactor::Zero;
    BlendOp alpha_op = BlendOp::Add;
    u8 color_write_mask = kWriteAll;  // kWriteRed | kWriteGreen | ...

    bool logic_op_enabled = false;  // needs Capabilities::logic_op, and blending off
    LogicOp logic_op = LogicOp::NoOp;

    bool depth_test = false;
    bool depth_write = false;
    CompareOp depth_compare = CompareOp::Less;

    bool stencil_test = false;
    StencilFaceState stencil_front;
    StencilFaceState stencil_back;

    CullMode cull = CullMode::None;
    FrontFace front_face = FrontFace::CounterClockwise;

    bool depth_bias_enabled = false;
    float depth_bias_constant = 0.0f;
    float depth_bias_clamp = 0.0f;
    float depth_bias_slope = 0.0f;
};

struct GraphicsPipelineDesc {
    ShaderCode vertex;
    ShaderCode fragment;  // empty spirv: no fragment shader (a pass with no attachments)
    ShaderCode geometry;  // empty spirv: none; needs Capabilities::geometry_shader
    VertexLayout vertex_layout;
    Topology topology = Topology::TriangleList;  // TriangleFan needs Capabilities::triangle_fans
    // The attachment formats the pipeline is used with (Format{} = none). The formats are the ones the
    // targets module's images actually have (after ChooseFormat).
    Format color_format{};
    Format depth_format{};
    FixedFunctionState state;
    std::vector<BindingSetDesc> sets;  // the shape of each set index the shaders use
    std::string name;
};

struct ComputePipelineDesc {
    ShaderCode shader;
    std::vector<BindingSetDesc> sets;
    std::string name;
};

// A compiled pipeline. Creation may be called from several threads at once.
class Pipeline {
public:
    struct Impl;
    explicit Pipeline(std::unique_ptr<Impl> impl);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    bool IsCompute() const;
    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// ---- Recording -----------------------------------------------------------------------------------

struct Viewport {
    float x = 0, y = 0, width = 0, height = 0;
    float min_depth = 0.0f, max_depth = 1.0f;
};
struct Rect {
    s32 x = 0, y = 0;
    u32 width = 0, height = 0;
};

struct VertexBufferBinding {
    const Buffer* buffer = nullptr;
    u64 offset = 0;  // one buffer can be bound at many offsets
};
enum class IndexType : u8 { U8, U16, U32 };  // U8 needs Capabilities::index_uint8

// A pass with no attachments: the shadow write and the vertex-stage pipelines (X4). The framebuffer
// size, viewport and scissor are width x height until SetViewport / SetScissor change them.
struct PassDesc {
    u32 width = 0;
    u32 height = 0;
};

enum class FrameMark : u8 { Begin, End };

// ---- Images, samplers and passes with attachments (N2-27) ----------------------------------------

struct FormatSupport {
    bool attachment = false;  // a colour attachment (colour formats) or a depth-stencil attachment
    bool sampled = false;
    bool storage = false;
    bool blit_source = false;
    bool blit_destination = false;
};

// What an image is for, for ChooseFormat.
enum class FormatUse : u8 { ColorTarget, DepthTarget, Sampled };

// The conversion the caller owes when the format used is not the one asked for.
enum class Conversion : u8 {
    None,
    Rgb8ToRgba8,     // Rgb8 is stored as Rgba8: add an opaque alpha
    UnpackToRgba8,   // a 16-bit colour format the device cannot use: the caller stores Rgba8
    DecodeBc7,       // BC7 is off: decode on the CPU and store Rgba8
    DepthStencilTo32,  // D24S8 is stored as D32S8: convert the depth
};
struct ChosenFormat {
    Format format = Format::Undefined;
    Conversion conversion = Conversion::None;
};

struct ImageDesc {
    Format format = Format::Undefined;  // goes through ChooseFormat; the image's own format is Image::GetFormat()
    u32 width = 0;
    u32 height = 0;
    u32 mip_levels = 1;
    u32 layers = 1;   // 1, or 6 for a cube
    bool cube = false;
    // The image is a shadow map's colour target: it is also viewed as an R32 unsigned-integer storage
    // image (ImageView::ShadowStorage). No other image has storage usage (9.4 item 4).
    bool shadow_source = false;
    std::string name;
};

// A view of an image, made on first use.
enum class ImageView : u8 { Sampled, Depth, Stencil, ShadowStorage };

class Image {
public:
    struct Impl;
    explicit Image(std::unique_ptr<Impl> impl);
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Format GetFormat() const;  // the format the device stores
    u32 Width() const;
    u32 Height() const;
    u32 MipLevels() const;
    u32 Layers() const;

    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

enum class Filter : u8 { Nearest, Linear };
enum class AddressMode : u8 { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder, MirrorClampToEdge };
struct SamplerDesc {
    Filter min_filter = Filter::Linear;
    Filter mag_filter = Filter::Linear;
    Filter mip_filter = Filter::Linear;
    AddressMode address_u = AddressMode::ClampToEdge;
    AddressMode address_v = AddressMode::ClampToEdge;
    // The border colour of ClampToBorder. Unset: transparent black. Any value needs
    // Capabilities::custom_border_color; without it CreateSampler fails with a message and the caller
    // emulates the colour in the shader. There is no compare sampler: shadow lookups compare in the shader.
    std::optional<std::array<float, 4>> custom_border_color;
    float min_lod = 0.0f;
    float max_lod = 1000.0f;
};

class Sampler {
public:
    struct Impl;
    explicit Sampler(std::unique_ptr<Impl> impl);
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

enum class LoadOp : u8 { Load, Clear };
// A pass with up to one colour and one depth-stencil attachment; the attachments are stored.
struct TargetPassDesc {
    Image* color = nullptr;
    Image* depth = nullptr;
    LoadOp color_load = LoadOp::Load;
    LoadOp depth_load = LoadOp::Load;  // depth and stencil
    std::array<float, 4> clear_color{};
    float clear_depth = 1.0f;
    u32 clear_stencil = 0;
    Rect area;  // the render area; viewport and scissor start as this
};

struct ClearValue {
    std::array<float, 4> color{};
    float depth = 1.0f;
    u32 stencil = 0;
};
struct ImageCopyRegion {
    u32 src_mip = 0, src_layer = 0, src_x = 0, src_y = 0;
    u32 dst_mip = 0, dst_layer = 0, dst_x = 0, dst_y = 0;
    u32 width = 0, height = 0;
};
struct ImageBlitRegion {
    u32 src_mip = 0, dst_mip = 0, layer = 0;
    Rect src;
    Rect dst;
};
enum class ImageAspect : u8 { Color, Depth, Stencil };
// A buffer-image copy covers one aspect of one level and layer: depth and stencil are two regions.
struct BufferImageRegion {
    u64 buffer_offset = 0;
    u32 mip_level = 0;
    u32 layer = 0;
    Rect image_rect;
    ImageAspect aspect = ImageAspect::Color;
};

// One recorder is one command list of the calling thread. A thread has at most one open recorder
// (Backend::BeginRecording) and submits its recorders in the order it began them. A recorder that is
// destroyed without being submitted is dropped. Resources a recorder uses must stay alive until the GPU
// has passed the value its submission returned.
class Recorder {
public:
    struct Impl;
    explicit Recorder(std::unique_ptr<Impl> impl);
    Recorder(Recorder&&) noexcept;
    Recorder& operator=(Recorder&&) noexcept;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // ---- core (N2-26) ----
    void SetPipeline(const Pipeline& pipeline);  // graphics or compute; the following draws use it
    // Binds `set` at index `index` of the bound pipeline (SetPipeline first).
    void Bind(const BindingSet& set, u32 index);
    // Binds vertex buffers from `first_binding`; at most kMaxVertexBindings bindings in all.
    void SetVertexBuffers(u32 first_binding, std::span<const VertexBufferBinding> buffers);
    void SetIndexBuffer(const Buffer& buffer, u64 offset, IndexType type);
    void SetViewport(const Viewport& viewport);
    void SetScissor(const Rect& scissor);
    void Draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0, u32 first_instance = 0);
    void DrawIndexed(u32 index_count, u32 instance_count = 1, u32 first_index = 0, s32 vertex_offset = 0, u32 first_instance = 0);
    void Dispatch(u32 x, u32 y, u32 z);
    // Outside a pass. Both buffers need BufferUsage::Transfer; the destination is not in the Upload heap
    // and the source not in the Readback heap.
    void CopyBuffer(const Buffer& dst, u64 dst_offset, const Buffer& src, u64 src_offset, u64 size);
    // A memory dependency: what `from` wrote in earlier commands (and earlier submissions) is visible to
    // what `to` reads or writes in later ones. Outside a pass. For buffers; images have their own rule.
    void Barrier(Stage from, Stage to);
    // A pass with no attachments (X4); draws and nothing else may follow until EndPass.
    void BeginPass(const PassDesc& pass);
    void EndPass();

    // ---- targets (N2-27) ----
    void CopyImage(const Image& src, Image& dst, std::span<const ImageCopyRegion> regions);
    void Blit(const Image& src, Image& dst, std::span<const ImageBlitRegion> regions, bool linear);
    void ClearImage(Image& image, const ClearValue& value, u32 mip_level = 0, u32 layer = 0);
    void CopyBufferToImage(const Buffer& src, Image& dst, std::span<const BufferImageRegion> regions);
    void CopyImageToBuffer(const Image& src, Buffer& dst, std::span<const BufferImageRegion> regions);
    void BeginTargetPass(const TargetPassDesc& pass);
    // Ends the open pass, copies every mip of the whole target into a twin (made once per target and
    // kept) and returns the twin for sampling; the next draw begins a new pass that loads (X6).
    Image& FeedbackCopy(Image& target);
    // For a wrapped swapchain image only: ends the open pass and moves the image to what presentation
    // needs (the last command of a frame's recorder, before the queue present of the caller).
    void PrepareForPresent(Image& image);

    // ---- state (N2-13) ----
    void SetBlendConstants(const std::array<float, 4>& rgba);
    void SetStencilReference(u32 reference);
    void SetStencilMasks(u32 compare_mask, u32 write_mask);

    // ---- readback and timing (N2-28) ----
    // Writes one of the two timestamps of the frame; Backend::FrameGpuTimeNs() is their difference.
    void MarkFrame(FrameMark mark);

    // The thread that began this recorder.
    std::thread::id Thread() const;

    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// ---- The backend ---------------------------------------------------------------------------------

// PICA framebuffer formats, for ReadbackToGuest (N2-28).
enum class PicaFormat : u8 { Rgba8, Rgb8, Rgb5A1, Rgb565, Rgba4, D16, D24, D24S8 };

class Backend {
public:
    struct Impl;
    explicit Backend(std::shared_ptr<Impl> impl);
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // ---- core (N2-26): the device ----
    const Capabilities& Caps() const;
    // True only with a surface, the present_wait bit on, and the presentation id and wait features
    // enabled.
    bool PresentWaitAvailable() const;
    // PresentWaitAvailable(), and a driver whose wait is released at display time: false for MoltenVK
    // (its wait is released when the command buffer completes, rhi-spike.md 7.3) and for KosmicKrisp
    // (unverified); every other driver is taken to measure display (inference).
    bool PresentWaitMeasuresDisplay() const;
    // The display-timing extension is enabled (with a surface, where the device lists it).
    bool DisplayTimingAvailable() const;
    // The device extensions that were enabled, by name.
    const std::vector<std::string>& EnabledExtensions() const;
    bool HasSurface() const;

    // ---- core: resources ----
    Result<std::shared_ptr<Buffer>> CreateBuffer(const BufferDesc& desc);
    // A ring buffer of `size` bytes in the Upload heap, usable as `usage` (Transfer is not needed).
    Result<std::unique_ptr<Ring>> CreateRing(u64 size, BufferUsage usage, std::string name = {});
    Result<std::shared_ptr<BindingSet>> CreateBindingSet(const BindingSetDesc& desc);
    Result<std::shared_ptr<Pipeline>> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc);
    Result<std::shared_ptr<Pipeline>> CreateComputePipeline(const ComputePipelineDesc& desc);

    // ---- core: recording and submission (R2) ----
    // The calling thread's next recorder. A second open recorder on one thread is a breach of the owner
    // rule (OwnerRule::OneOpenRecorder).
    Recorder BeginRecording();
    // Submits the recorder through the submit lock and returns the timeline value its completion
    // signals. Any wait the recorder requires (an image handed over to its thread) makes this a breach
    // (RequiredWait): use the overload with waits.
    TimelineValue Submit(Recorder&& recorder);
    // Blocks the calling thread until the timeline has reached `value`. WaitIdle() waits for everything
    // submitted so far.
    void WaitIdle(TimelineValue value);
    void WaitIdle();
    TimelineValue CompletedValue() const;      // the highest value the GPU has passed
    TimelineValue LastSubmittedValue() const;  // the highest value returned so far
    // Reads `out.size()` bytes of `buffer` at `offset` into `out`, after waiting for `after` (and, for a
    // buffer in the Device heap, through an internal copy). Does not need, and does not disturb, the
    // calling thread's open recorder.
    Result<void> ReadBuffer(const Buffer& buffer, u64 offset, std::span<u8> out, TimelineValue after = 0);

    // ---- sync (N2-28) ----
    // As Submit(Recorder&&), with waits: each wait keeps its own stage; the recorder's required waits
    // must each be covered by a wait of the same stage and a value at least as high. Without the
    // `timeline` bit a wait whose value has not completed blocks the submitter first.
    TimelineValue Submit(Recorder&& recorder, std::span<const Wait> waits);
    // Keeps `object` alive until the GPU has passed `last_use`, then releases it.
    void Retire(std::shared_ptr<void> object, TimelineValue last_use);
    // The pipeline cache blob: accepted only when its header matches the device (vendor, device, UUID).
    bool LoadPipelineCache(std::span<const u8> blob);
    std::vector<u8> SavePipelineCache();

    // ---- targets (N2-27) ----
    // Attachment, sampled, storage and blit support of a format, asked of the device (optimal tiling).
    FormatSupport QueryFormat(Format format) const;
    // The format to store `format` in and the conversion the caller owes (9.4 item 3): D24S8 when the d24s8
    // bit is on, else D32S8; Rgb8 always as Rgba8; Rgb565, Rgb5A1 and Rgba4 in whichever component order
    // the query says is an attachment, else Rgba8 with a conversion; Bc7 only when the bc7 bit is on, else
    // decode on the CPU; the other formats as they are.
    ChosenFormat ChooseFormat(Format format, FormatUse use) const;
    Result<std::shared_ptr<Image>> CreateImage(const ImageDesc& desc);
    Result<std::shared_ptr<Sampler>> CreateSampler(const SamplerDesc& desc);

    // ---- readback (N2-28) ----
    // Writes `rect` of a render target into guest memory in the PICA's Morton-tiled layout and `format`.
    Result<void> ReadbackToGuest(const Image& image, Rect rect, PicaFormat format, std::span<u8> dest);
    // A region of any backend image in the image's own format, row by row without padding.
    Result<std::vector<u8>> ReadImage(const Image& image, u32 x, u32 y, u32 width, u32 height);
    // The GPU time between the two MarkFrame marks of the last frame whose submission has passed.
    u64 FrameGpuTimeNs() const;

    Impl& impl() const { return *impl_; }

private:
    std::shared_ptr<Impl> impl_;
};

// Chooses the device (by capability, not by index), enables only what the capabilities use, applies the
// mask and makes the backend. Errors: no loader, no device, no device that qualifies, a device override
// that matches nothing (the message lists the devices), an unknown name in MH4N_RHI_MASK (the message
// lists the valid ones), a device the driver refuses to create.
Result<std::shared_ptr<Backend>> CreateBackend(std::shared_ptr<Instance> instance, const BackendOptions& options = {});

// ---- Targets, per-draw bindings and hand-over (N2-27) --------------------------------------------

// The images' owner changes to `thread` once the timeline has passed `wait`: when that thread first
// records on or samples the image, the wait is added to its recorder's required waits, which the submit
// paths check. An attachment of an open pass and a depth-stencil target cannot be handed over (breach).
void HandOver(Image& image, std::thread::id thread, Wait wait);

// Per-draw binding sets of one recording thread, recycled by timeline value (X5, X5b).
class BindingArena {
public:
    struct Impl;
    explicit BindingArena(std::shared_ptr<Backend> backend);
    ~BindingArena();
    BindingArena(const BindingArena&) = delete;
    BindingArena& operator=(const BindingArena&) = delete;

    // A set of the given shape that no submission in flight uses. Valid until the calling thread's open
    // recorder is submitted; it is recycled once that submission has passed. Only the calling thread
    // writes and binds it.
    Result<BindingSet*> Acquire(const BindingSetDesc& desc);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace mh4n::gpu::rhi
