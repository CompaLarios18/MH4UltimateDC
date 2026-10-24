// The backend core (N2-26): the instance, the device chosen by capability, the capability structure and its
// mask, buffers and rings, binding sets, pipelines, a pass with no attachments, submission and the owner-rule
// checks. Run on whatever Vulkan device the machine has (lavapipe in the cloud and in Linux CI); a case that
// needs a device skips with a message without one, and fails under MH4N_REQUIRE_VULKAN=1.
//
// The cases that do not need a device (the mask's text, the device preference, the translation of the
// pipeline state, the ring's bookkeeping) run everywhere. The mask exists because lavapipe reports more
// than the PC does: a masked feature is not enabled on the device, so its fallback really runs.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "gpu/backend/core/impl.h"
#include "gpu/backend/core/policy.h"
#include "gpu/backend/core/ring_alloc.h"
#include "gpu/backend/core/to_plume.h"
#include "mh4n/log.h"
#include "support/vulkan_device.h"
#include "vkprobe/live.h"
#include "vkprobe/report.h"

#include "gpu_backend_core_shaders.inc"

// `using namespace mh4n` would make Result ambiguous (the 3DS result code of mh4n/types.h): name what is used.
using mh4n::LogRecord;
using mh4n::SetLogSink;
using mh4n::u16;
using mh4n::u32;
using mh4n::u64;
using mh4n::u8;
using mh4n::s32;
namespace test = mh4n::test;
namespace vkprobe = mh4n::vkprobe;
using namespace mh4n::gpu::rhi;

namespace {

// ---- helpers ---------------------------------------------------------------------------------------------------

void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

struct ScopedEnv {
    std::string name;
    std::string old;
    bool had = false;
    ScopedEnv(const char* n, const char* value) : name(n) {
        if (const char* current = std::getenv(n)) {
            had = true;
            old = current;
        }
        SetEnv(n, value);
    }
    ~ScopedEnv() { SetEnv(name.c_str(), had ? old.c_str() : nullptr); }
};

// Captures the log lines of the running case.
struct LogCapture {
    std::mutex mutex;
    std::vector<std::string> lines;
    LogCapture() {
        SetLogSink([this](const LogRecord& record) {
            std::lock_guard lock(mutex);
            lines.push_back(std::string(record.category) + ": " + std::string(record.message));
        });
    }
    ~LogCapture() { SetLogSink({}); }
    int Count(const std::string& needle) {
        std::lock_guard lock(mutex);
        return static_cast<int>(std::count_if(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; }));
    }
};

// Collects the owner-rule breaches of the running case.
struct BreachCapture {
    std::mutex mutex;
    std::vector<OwnerRule> rules;
    OwnerRuleHandler previous;
    BreachCapture() {
        previous = SetOwnerRuleHandler([this](const OwnerRuleBreach& breach) {
            std::lock_guard lock(mutex);
            rules.push_back(breach.rule);
        });
    }
    ~BreachCapture() { SetOwnerRuleHandler(previous); }
    int Count(OwnerRule rule) {
        std::lock_guard lock(mutex);
        return static_cast<int>(std::count(rules.begin(), rules.end(), rule));
    }
    size_t Total() {
        std::lock_guard lock(mutex);
        return rules.size();
    }
};

std::shared_ptr<Buffer> Make(Backend& backend, u64 size, Heap heap, BufferUsage usage, const char* name = "test") {
    BufferDesc desc;
    desc.size = size;
    desc.heap = heap;
    desc.usage = usage;
    desc.name = name;
    Result<std::shared_ptr<Buffer>> buffer = backend.CreateBuffer(desc);
    REQUIRE_MESSAGE(buffer.ok(), buffer.error());
    return *buffer;
}

std::vector<u32> ReadWords(Backend& backend, const Buffer& buffer, size_t count, TimelineValue after) {
    std::vector<u32> words(count);
    Result<void> read = backend.ReadBuffer(buffer, 0, std::span<u8>(reinterpret_cast<u8*>(words.data()), count * sizeof(u32)), after);
    REQUIRE_MESSAGE(read.ok(), read.error());
    return words;
}

void WriteWords(Buffer& buffer, std::initializer_list<u32> words) {
    REQUIRE(buffer.Mapped().size() >= words.size() * sizeof(u32));
    std::memcpy(buffer.Mapped().data(), words.begin(), words.size() * sizeof(u32));
    buffer.Flush(0, words.size() * sizeof(u32));
}

// The bindings every shader of the test uses: set 0 = a storage buffer at 0 and a uniform buffer at 1.
BindingSetDesc StorageAndUniform() {
    BindingSetDesc desc;
    desc.slots = {{0, BindingType::StorageBuffer}, {1, BindingType::UniformBuffer}};
    return desc;
}

// A storage buffer (device heap) and its binding set with scale and bias.
struct Scene {
    std::shared_ptr<Buffer> storage;
    std::shared_ptr<Buffer> uniform;
    std::shared_ptr<BindingSet> set;
};

Scene MakeScene(Backend& backend, u32 scale, u32 bias, u64 words = 64) {
    Scene scene;
    scene.storage = Make(backend, words * 4, Heap::Device, BufferUsage::Storage | BufferUsage::Transfer, "storage");
    scene.uniform = Make(backend, 256, Heap::Upload, BufferUsage::Uniform, "uniform");
    WriteWords(*scene.uniform, {scale, bias});
    Result<std::shared_ptr<BindingSet>> set = backend.CreateBindingSet(StorageAndUniform());
    REQUIRE_MESSAGE(set.ok(), set.error());
    scene.set = *set;
    REQUIRE(scene.set->SetStorageBuffer(0, *scene.storage, 0, words * 4).ok());
    REQUIRE(scene.set->SetUniformBuffer(1, *scene.uniform, 0, 8).ok());
    return scene;
}

std::shared_ptr<Pipeline> GraphicsWrite(Backend& backend, std::span<const u32> vertex, VertexLayout layout = {}) {
    GraphicsPipelineDesc desc;
    desc.vertex.spirv = vertex;
    desc.vertex_layout = std::move(layout);
    desc.sets = {StorageAndUniform()};
    desc.name = "graphics write";
    Result<std::shared_ptr<Pipeline>> pipeline = backend.CreateGraphicsPipeline(desc);
    REQUIRE_MESSAGE(pipeline.ok(), pipeline.error());
    return *pipeline;
}

std::shared_ptr<Pipeline> ComputeWrite(Backend& backend) {
    ComputePipelineDesc desc;
    desc.shader.spirv = cs_write_comp;
    desc.sets = {StorageAndUniform()};
    desc.name = "compute write";
    Result<std::shared_ptr<Pipeline>> pipeline = backend.CreateComputePipeline(desc);
    REQUIRE_MESSAGE(pipeline.ok(), pipeline.error());
    return *pipeline;
}

bool Has(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool AnyContains(const std::vector<std::string>& names, const std::string& part) {
    return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return n.find(part) != std::string::npos; });
}

// An instance made with the default options: the helper's own instance enables the debug messenger when the
// validation layer is on, and these cases are about what the defaults enable.
std::shared_ptr<Instance> PlainInstance() {
    Result<std::shared_ptr<Instance>> instance = CreateInstance();
    if (!instance.ok()) {
        test::SkipOrFail(instance.error());
        return nullptr;
    }
    return *instance;
}

// The bit names in the order of Cap.
std::vector<Cap> AllCaps() {
    std::vector<Cap> caps;
    for (u32 i = 0; i < kCapCount; ++i) {
        caps.push_back(static_cast<Cap>(i));
    }
    return caps;
}

// The capabilities after a mask, from a fresh backend. nullopt after a skip.
std::optional<Capabilities> CapsWith(CapabilityMask mask, std::vector<std::string>* extensions = nullptr) {
    BackendOptions options;
    options.mask = mask;
    std::shared_ptr<Backend> backend = test::RequireBackend(options);
    if (!backend) {
        return std::nullopt;
    }
    if (extensions != nullptr) {
        *extensions = backend->EnabledExtensions();
    }
    return backend->Caps();
}

}  // namespace

// ---- the mask's text (no device) --------------------------------------------------------------------------------

TEST_CASE("mask/parse") {
    CHECK(CapabilityMask::Parse("").value().bits == 0);
    CHECK(CapabilityMask::Parse("  ").value().bits == 0);
    for (Cap cap : AllCaps()) {
        const Result<CapabilityMask> one = CapabilityMask::Parse(std::string(CapName(cap)));
        REQUIRE(one.ok());
        CHECK(one->bits == (1u << static_cast<u32>(cap)));
    }
    const Result<CapabilityMask> list = CapabilityMask::Parse(" BC7 , interlock,d24s8 ");
    REQUIRE(list.ok());
    CHECK(list->Has(Cap::Bc7));
    CHECK(list->Has(Cap::Interlock));
    CHECK(list->Has(Cap::D24S8));
    CHECK_FALSE(list->Has(Cap::Timeline));
    CHECK(CapabilityMask::Parse("all").value() == CapabilityMask::All());
    CHECK(CapabilityMask::All().bits == (1u << kCapCount) - 1u);
    CHECK(CapabilityMask::Parse("bc7,all").value() == CapabilityMask::All());
    CHECK(CapabilityMask::All().ToString() == "all");
    CHECK(CapabilityMask::Parse("d24s8,bc7").value().ToString() == "d24s8,bc7");
    CHECK(CapabilityMask{}.ToString().empty());
}

TEST_CASE("mask/unknown_name_lists_the_valid_ones") {
    const Result<CapabilityMask> bad = CapabilityMask::Parse("bc7,flux_capacitor");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().find("flux_capacitor") != std::string::npos);
    for (Cap cap : AllCaps()) {
        CHECK_MESSAGE(bad.error().find(std::string(CapName(cap))) != std::string::npos, "the list names " << CapName(cap));
    }
    CHECK(bad.error().find("all") != std::string::npos);
    // From the environment's text too, with its name in the message.
    const Result<CapabilityMask> from_env = core::CombineMasks("nonsense", {});
    REQUIRE_FALSE(from_env.ok());
    CHECK(from_env.error().find("MH4N_RHI_MASK") != std::string::npos);
    // The two masks are joined, never subtracted.
    const Result<CapabilityMask> both = core::CombineMasks("bc7", CapabilityMask{}.With(Cap::Timeline));
    REQUIRE(both.ok());
    CHECK(both->Has(Cap::Bc7));
    CHECK(both->Has(Cap::Timeline));
}

// ---- the device choice (no device) ------------------------------------------------------------------------------

namespace {
core::DeviceCandidate Candidate(u32 index, const char* name, const char* driver, core::DeviceType type, u32 bits = 5) {
    core::DeviceCandidate c;
    c.index = index;
    c.name = name;
    c.driver_name = driver;
    c.type = type;
    c.has_graphics_queue = true;
    c.fragment_stores_and_atomics = true;
    c.capability_bits = bits;
    return c;
}
}  // namespace

TEST_CASE("choice/preference_order") {
    using core::DeviceType;
    // discrete before integrated before virtual before CPU, whatever the index
    {
        std::vector<core::DeviceCandidate> devices = {Candidate(0, "cpu", "llvm", DeviceType::Cpu, 13), Candidate(1, "virt", "v", DeviceType::Virtual, 13),
                                                      Candidate(2, "igpu", "i", DeviceType::Integrated, 1), Candidate(3, "dgpu", "d", DeviceType::Discrete, 0)};
        core::DeviceChoice choice = core::ChooseDevice(devices, "", false);
        REQUIRE(choice.ok);
        CHECK(choice.index == 3);
        CHECK(choice.log.size() == 4);  // every candidate is logged with its reason
        devices.erase(devices.begin() + 3);
        CHECK(core::ChooseDevice(devices, "", false).index == 2);
        devices.erase(devices.begin() + 2);
        CHECK(core::ChooseDevice(devices, "", false).index == 1);
        devices.erase(devices.begin() + 1);
        CHECK(core::ChooseDevice(devices, "", false).index == 0);
    }
    // inside one type: the most capability bits, then the lowest index
    {
        std::vector<core::DeviceCandidate> devices = {Candidate(0, "a", "d", DeviceType::Discrete, 4), Candidate(1, "b", "d", DeviceType::Discrete, 9),
                                                      Candidate(2, "c", "d", DeviceType::Discrete, 9)};
        CHECK(core::ChooseDevice(devices, "", false).index == 1);
        devices[0].capability_bits = 9;
        CHECK(core::ChooseDevice(devices, "", false).index == 0);
    }
    // a device without a graphics queue or without fragmentStoresAndAtomics is never chosen, however good
    {
        std::vector<core::DeviceCandidate> devices = {Candidate(0, "dgpu", "d", DeviceType::Discrete, 13), Candidate(1, "cpu", "c", DeviceType::Cpu, 0)};
        devices[0].fragment_stores_and_atomics = false;
        CHECK(core::ChooseDevice(devices, "", false).index == 1);
        devices[0].fragment_stores_and_atomics = true;
        devices[0].has_graphics_queue = false;
        CHECK(core::ChooseDevice(devices, "", false).index == 1);
        devices[1].has_graphics_queue = false;
        const core::DeviceChoice none = core::ChooseDevice(devices, "", false);
        CHECK_FALSE(none.ok);
        CHECK(none.error.find("no graphics queue") != std::string::npos);
    }
    // with a surface, a device that cannot present is not chosen; without one it is
    {
        std::vector<core::DeviceCandidate> devices = {Candidate(0, "dgpu", "d", DeviceType::Discrete), Candidate(1, "igpu", "i", DeviceType::Integrated)};
        devices[0].can_present = false;
        CHECK(core::ChooseDevice(devices, "", true).index == 1);
        CHECK(core::ChooseDevice(devices, "", false).index == 0);
    }
}

TEST_CASE("choice/override_tells_two_devices_with_one_name_apart") {
    using core::DeviceType;
    const std::vector<core::DeviceCandidate> devices = {Candidate(0, "Apple M2", "MoltenVK", DeviceType::Integrated), Candidate(1, "Apple M2", "KosmicKrisp", DeviceType::Integrated),
                                                        Candidate(2, "llvmpipe", "llvmpipe", DeviceType::Cpu)};
    CHECK(core::ChooseDevice(devices, "Apple M2 (KosmicKrisp)", false).index == 1);
    CHECK(core::ChooseDevice(devices, "kosmickrisp", false).index == 1);  // case is ignored
    CHECK(core::ChooseDevice(devices, "MoltenVK", false).index == 0);
    CHECK(core::ChooseDevice(devices, "Apple M2", false).index == 0);  // the first that matches
    CHECK(core::ChooseDevice(devices, "llvm", false).index == 2);
    // no match: an error that lists every device
    const core::DeviceChoice none = core::ChooseDevice(devices, "RX 6700", false);
    CHECK_FALSE(none.ok);
    CHECK(none.error.find("RX 6700") != std::string::npos);
    CHECK(none.error.find("Apple M2 (KosmicKrisp)") != std::string::npos);
    CHECK(none.error.find("Apple M2 (MoltenVK)") != std::string::npos);
    CHECK(none.error.find("llvmpipe (llvmpipe)") != std::string::npos);
    // a match that cannot be used is refused, not replaced by another device
    std::vector<core::DeviceCandidate> unusable = devices;
    unusable[1].fragment_stores_and_atomics = false;
    const core::DeviceChoice refused = core::ChooseDevice(unusable, "KosmicKrisp", false);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error.find("fragmentStoresAndAtomics") != std::string::npos);
}

TEST_CASE("choice/present_wait_is_available_only_with_a_surface_the_bit_and_both_features") {
    using core::PresentWaitAvailable;
    for (bool surface : {false, true}) {
        for (bool bit : {false, true}) {
            for (bool features : {false, true}) {
                CHECK(PresentWaitAvailable(surface, bit, features) == (surface && bit && features));
            }
        }
    }
}

TEST_CASE("choice/present_wait_measures_display_rule") {
    using core::PresentWaitMeasuresDisplay;
    CHECK_FALSE(PresentWaitMeasuresDisplay(false, 3, "radv"));  // never when the wait is not available
    CHECK_FALSE(PresentWaitMeasuresDisplay(false, 0, ""));
    CHECK(PresentWaitMeasuresDisplay(true, 3, "radv"));
    CHECK(PresentWaitMeasuresDisplay(true, 1, "AMD proprietary driver"));
    CHECK_FALSE(PresentWaitMeasuresDisplay(true, core::kDriverIdMoltenVk, "MoltenVK"));
    CHECK_FALSE(PresentWaitMeasuresDisplay(true, core::kDriverIdMoltenVk, ""));
    CHECK_FALSE(PresentWaitMeasuresDisplay(true, core::kDriverIdKosmicKrispUnverified, ""));
    CHECK_FALSE(PresentWaitMeasuresDisplay(true, 0, "KosmicKrisp"));
    // the Apple test: either driver id, either name, or Apple's vendor id
    CHECK(core::IsAppleDriver(14, "", 0));
    CHECK(core::IsAppleDriver(28, "", 0));
    CHECK(core::IsAppleDriver(0, "Mesa KosmicKrisp", 0));
    CHECK(core::IsAppleDriver(0, "", 0x106B));
    CHECK_FALSE(core::IsAppleDriver(13, "llvmpipe", 0x10005));
}

// ---- the translation of the pipeline state (no device) -----------------------------------------------------------

namespace {
using ::plume::RenderBlend;
using ::plume::RenderBlendOperation;
using ::plume::RenderComparisonFunction;
using ::plume::RenderGraphicsPipelineDesc;
using ::plume::RenderLogicOperation;
using ::plume::RenderStencilOp;

RenderGraphicsPipelineDesc Translate(const FixedFunctionState& state, bool color = true) {
    RenderGraphicsPipelineDesc desc;
    const Result<void> result = core::ToPlume(state, color, &desc);
    REQUIRE_MESSAGE(result.ok(), result.error());
    return desc;
}
}  // namespace

TEST_CASE("to_plume/blend_equations") {
    struct Row {
        BlendOp op;
        RenderBlendOperation expected;
    };
    const Row rows[] = {{BlendOp::Add, RenderBlendOperation::ADD},
                        {BlendOp::Subtract, RenderBlendOperation::SUBTRACT},
                        {BlendOp::ReverseSubtract, RenderBlendOperation::REV_SUBTRACT},
                        {BlendOp::Min, RenderBlendOperation::MIN},
                        {BlendOp::Max, RenderBlendOperation::MAX}};
    for (const Row& row : rows) {
        for (const Row& alpha : rows) {
            FixedFunctionState state;
            state.blend_enabled = true;
            state.color_op = row.op;
            state.alpha_op = alpha.op;
            const RenderGraphicsPipelineDesc desc = Translate(state);
            CHECK(desc.renderTargetBlend[0].blendEnabled);
            CHECK(desc.renderTargetBlend[0].blendOp == row.expected);
            CHECK(desc.renderTargetBlend[0].blendOpAlpha == alpha.expected);
        }
    }
    FixedFunctionState off;
    CHECK_FALSE(Translate(off).renderTargetBlend[0].blendEnabled);
}

TEST_CASE("to_plume/blend_factors") {
    struct Row {
        BlendFactor factor;
        RenderBlend expected;
    };
    const Row rows[] = {{BlendFactor::Zero, RenderBlend::ZERO},
                        {BlendFactor::One, RenderBlend::ONE},
                        {BlendFactor::SrcColor, RenderBlend::SRC_COLOR},
                        {BlendFactor::OneMinusSrcColor, RenderBlend::INV_SRC_COLOR},
                        {BlendFactor::DstColor, RenderBlend::DEST_COLOR},
                        {BlendFactor::OneMinusDstColor, RenderBlend::INV_DEST_COLOR},
                        {BlendFactor::SrcAlpha, RenderBlend::SRC_ALPHA},
                        {BlendFactor::OneMinusSrcAlpha, RenderBlend::INV_SRC_ALPHA},
                        {BlendFactor::DstAlpha, RenderBlend::DEST_ALPHA},
                        {BlendFactor::OneMinusDstAlpha, RenderBlend::INV_DEST_ALPHA},
                        {BlendFactor::ConstantColor, RenderBlend::BLEND_FACTOR},
                        {BlendFactor::OneMinusConstantColor, RenderBlend::INV_BLEND_FACTOR},
                        {BlendFactor::SrcAlphaSaturate, RenderBlend::SRC_ALPHA_SAT}};
    for (const Row& row : rows) {
        // each factor in each of the four places, so a swapped pair of fields shows
        FixedFunctionState state;
        state.blend_enabled = true;
        state.src_color = row.factor;
        state.dst_color = BlendFactor::Zero;
        state.src_alpha = BlendFactor::One;
        state.dst_alpha = BlendFactor::Zero;
        RenderGraphicsPipelineDesc desc = Translate(state);
        CHECK(desc.renderTargetBlend[0].srcBlend == row.expected);
        CHECK(desc.renderTargetBlend[0].dstBlend == RenderBlend::ZERO);
        CHECK(desc.renderTargetBlend[0].srcBlendAlpha == RenderBlend::ONE);
        CHECK(desc.renderTargetBlend[0].dstBlendAlpha == RenderBlend::ZERO);
        state = FixedFunctionState();
        state.blend_enabled = true;
        state.src_color = BlendFactor::Zero;
        state.dst_color = row.factor;
        state.src_alpha = BlendFactor::Zero;
        state.dst_alpha = row.factor;
        desc = Translate(state);
        CHECK(desc.renderTargetBlend[0].srcBlend == RenderBlend::ZERO);
        CHECK(desc.renderTargetBlend[0].dstBlend == row.expected);
        CHECK(desc.renderTargetBlend[0].srcBlendAlpha == RenderBlend::ZERO);
        CHECK(desc.renderTargetBlend[0].dstBlendAlpha == row.expected);
    }
}

TEST_CASE("to_plume/constant_alpha_cannot_be_expressed") {
    for (BlendFactor factor : {BlendFactor::ConstantAlpha, BlendFactor::OneMinusConstantAlpha}) {
        FixedFunctionState state;
        state.blend_enabled = true;
        state.dst_alpha = factor;
        RenderGraphicsPipelineDesc desc;
        const Result<void> result = core::ToPlume(state, true, &desc);
        CHECK_FALSE(result.ok());
        CHECK(result.error().find("constant-alpha") != std::string::npos);
        // not an error while blending is off: the factor is not used
        // not an error while blending is off: the factor is not used, and the backend is given neutral ones
        // (it converts all four factors whether or not blending is on)
        state.blend_enabled = false;
        state.src_color = state.dst_color = state.src_alpha = factor;
        REQUIRE(core::ToPlume(state, true, &desc).ok());
        CHECK(desc.renderTargetBlend[0].srcBlend == RenderBlend::ONE);
        CHECK(desc.renderTargetBlend[0].srcBlendAlpha == RenderBlend::ONE);
        CHECK(desc.renderTargetBlend[0].dstBlend == RenderBlend::ZERO);
        CHECK(desc.renderTargetBlend[0].dstBlendAlpha == RenderBlend::ZERO);
    }
}

TEST_CASE("to_plume/colour_write_mask_and_logic_op") {
    for (u8 mask = 0; mask <= kWriteAll; ++mask) {
        FixedFunctionState state;
        state.color_write_mask = mask;
        CHECK(Translate(state).renderTargetBlend[0].renderTargetWriteMask == mask);
    }
    struct Row {
        LogicOp op;
        RenderLogicOperation expected;
    };
    const Row rows[] = {{LogicOp::Clear, RenderLogicOperation::CLEAR},
                        {LogicOp::Set, RenderLogicOperation::SET},
                        {LogicOp::Copy, RenderLogicOperation::COPY},
                        {LogicOp::CopyInverted, RenderLogicOperation::COPY_INVERTED},
                        {LogicOp::NoOp, RenderLogicOperation::NOOP},
                        {LogicOp::Invert, RenderLogicOperation::INVERT},
                        {LogicOp::And, RenderLogicOperation::AND},
                        {LogicOp::Nand, RenderLogicOperation::NAND},
                        {LogicOp::Or, RenderLogicOperation::OR},
                        {LogicOp::Nor, RenderLogicOperation::NOR},
                        {LogicOp::Xor, RenderLogicOperation::XOR},
                        {LogicOp::Equiv, RenderLogicOperation::EQUIV},
                        {LogicOp::AndReverse, RenderLogicOperation::AND_REVERSE},
                        {LogicOp::AndInverted, RenderLogicOperation::AND_INVERTED},
                        {LogicOp::OrReverse, RenderLogicOperation::OR_REVERSE},
                        {LogicOp::OrInverted, RenderLogicOperation::OR_INVERTED}};
    for (const Row& row : rows) {
        FixedFunctionState state;
        state.logic_op_enabled = true;
        state.logic_op = row.op;
        RenderGraphicsPipelineDesc desc = Translate(state);
        CHECK(desc.logicOpEnabled);
        CHECK(desc.logicOp == row.expected);
        state.logic_op_enabled = false;
        CHECK_FALSE(Translate(state).logicOpEnabled);
    }
}

TEST_CASE("to_plume/depth_stencil_cull_and_bias") {
    struct Compare {
        CompareOp op;
        RenderComparisonFunction expected;
    };
    const Compare compares[] = {{CompareOp::Never, RenderComparisonFunction::NEVER},
                                {CompareOp::Less, RenderComparisonFunction::LESS},
                                {CompareOp::Equal, RenderComparisonFunction::EQUAL},
                                {CompareOp::LessEqual, RenderComparisonFunction::LESS_EQUAL},
                                {CompareOp::Greater, RenderComparisonFunction::GREATER},
                                {CompareOp::NotEqual, RenderComparisonFunction::NOT_EQUAL},
                                {CompareOp::GreaterEqual, RenderComparisonFunction::GREATER_EQUAL},
                                {CompareOp::Always, RenderComparisonFunction::ALWAYS}};
    for (const Compare& row : compares) {
        FixedFunctionState state;
        state.depth_test = true;
        state.depth_write = true;
        state.depth_compare = row.op;
        RenderGraphicsPipelineDesc desc = Translate(state);
        CHECK(desc.depthEnabled);
        CHECK(desc.depthWriteEnabled);
        CHECK(desc.depthFunction == row.expected);
        state.depth_test = false;
        state.depth_write = false;
        desc = Translate(state);
        CHECK_FALSE(desc.depthEnabled);
        CHECK_FALSE(desc.depthWriteEnabled);
        // the stencil compare of each face, front and back kept apart
        state = FixedFunctionState();
        state.stencil_test = true;
        state.stencil_front.compare = row.op;
        state.stencil_back.compare = CompareOp::Never;
        desc = Translate(state);
        CHECK(desc.stencilEnabled);
        CHECK(desc.stencilFrontFace.compareFunction == row.expected);
        CHECK(desc.stencilBackFace.compareFunction == RenderComparisonFunction::NEVER);
    }
    struct Op {
        StencilOp op;
        RenderStencilOp expected;
    };
    const Op ops[] = {{StencilOp::Keep, RenderStencilOp::KEEP},
                      {StencilOp::Zero, RenderStencilOp::ZERO},
                      {StencilOp::Replace, RenderStencilOp::REPLACE},
                      {StencilOp::IncrementClamp, RenderStencilOp::INCREMENT_AND_CLAMP},
                      {StencilOp::DecrementClamp, RenderStencilOp::DECREMENT_AND_CLAMP},
                      {StencilOp::Invert, RenderStencilOp::INVERT},
                      {StencilOp::IncrementWrap, RenderStencilOp::INCREMENT_AND_WRAP},
                      {StencilOp::DecrementWrap, RenderStencilOp::DECREMENT_AND_WRAP}};
    for (const Op& row : ops) {
        FixedFunctionState state;
        state.stencil_test = true;
        state.stencil_front = {row.op, StencilOp::Keep, StencilOp::Zero, CompareOp::Always};
        state.stencil_back = {StencilOp::Zero, row.op, StencilOp::Keep, CompareOp::Always};
        RenderGraphicsPipelineDesc desc = Translate(state);
        CHECK(desc.stencilFrontFace.failOp == row.expected);
        CHECK(desc.stencilFrontFace.passOp == RenderStencilOp::KEEP);
        CHECK(desc.stencilFrontFace.depthFailOp == RenderStencilOp::ZERO);
        CHECK(desc.stencilBackFace.failOp == RenderStencilOp::ZERO);
        CHECK(desc.stencilBackFace.passOp == row.expected);
        CHECK(desc.stencilBackFace.depthFailOp == RenderStencilOp::KEEP);
        state.stencil_front = {StencilOp::Keep, StencilOp::Zero, row.op, CompareOp::Always};
        desc = Translate(state);
        CHECK(desc.stencilFrontFace.depthFailOp == row.expected);
    }
    for (CullMode mode : {CullMode::None, CullMode::Front, CullMode::Back}) {
        FixedFunctionState state;
        state.cull = mode;
        const ::plume::RenderCullMode expected = mode == CullMode::None    ? ::plume::RenderCullMode::NONE
                                                 : mode == CullMode::Front ? ::plume::RenderCullMode::FRONT
                                                                           : ::plume::RenderCullMode::BACK;
        CHECK(Translate(state).cullMode == expected);
    }
    for (FrontFace face : {FrontFace::Clockwise, FrontFace::CounterClockwise}) {
        FixedFunctionState state;
        state.front_face = face;
        CHECK(Translate(state).frontFace == (face == FrontFace::Clockwise ? ::plume::RenderFrontFace::CLOCKWISE : ::plume::RenderFrontFace::COUNTER_CLOCKWISE));
    }
    // depth bias: baked; a whole constant factor
    FixedFunctionState bias;
    bias.depth_bias_enabled = true;
    bias.depth_bias_constant = -3.0f;
    bias.depth_bias_slope = 1.25f;
    RenderGraphicsPipelineDesc desc = Translate(bias);
    CHECK(desc.depthBias == -3);
    CHECK(desc.depthBiasClamp == 0.0f);  // the device does not enable depthBiasClamp
    CHECK(desc.slopeScaledDepthBias == 1.25f);
    CHECK_FALSE(desc.dynamicDepthBiasEnabled);
    bias.depth_bias_constant = 1.5f;
    RenderGraphicsPipelineDesc rejected;
    const Result<void> fractional = core::ToPlume(bias, true, &rejected);
    CHECK_FALSE(fractional.ok());
    bias.depth_bias_enabled = false;
    desc = Translate(bias);
    CHECK(desc.depthBias == 0);
    CHECK(desc.depthBiasClamp == 0.0f);
}

TEST_CASE("to_plume/blend_constants_and_stencil_are_always_dynamic") {
    const FixedFunctionState any;
    RenderGraphicsPipelineDesc desc = Translate(any);
    CHECK(desc.dynamicBlendConstantsEnabled);
    CHECK(desc.dynamicStencilEnabled);
    FixedFunctionState busy;
    busy.blend_enabled = true;
    busy.src_color = BlendFactor::ConstantColor;
    busy.stencil_test = true;
    desc = Translate(busy);
    CHECK(desc.dynamicBlendConstantsEnabled);
    CHECK(desc.dynamicStencilEnabled);
}

TEST_CASE("to_plume/whole_description_attachments_topology_and_vertex_layout") {
    GraphicsPipelineDesc pipeline;
    pipeline.color_format = Format::Rgba8Unorm;
    pipeline.depth_format = Format::D32S8;
    pipeline.topology = Topology::TriangleFan;
    pipeline.vertex_layout.bindings = {{0, 16, false}, {3, 4, true}};
    pipeline.vertex_layout.attributes = {{0, 0, Format::Rgba32Float, 0}, {1, 3, Format::Rgba8Uint, 0}, {2, 0, Format::Rg8Sint, 12}};
    pipeline.state.depth_test = true;
    core::PlumeGraphicsDesc out;
    const Result<void> result = core::ToPlume(pipeline, nullptr, nullptr, nullptr, nullptr, &out);
    REQUIRE_MESSAGE(result.ok(), result.error());
    CHECK(out.desc.renderTargetCount == 1);
    CHECK(out.desc.renderTargetFormat[0] == ::plume::RenderFormat::R8G8B8A8_UNORM);
    CHECK(out.desc.depthTargetFormat == ::plume::RenderFormat::D32_FLOAT_S8_UINT);
    CHECK(out.desc.primitiveTopology == ::plume::RenderPrimitiveTopology::TRIANGLE_FAN);
    REQUIRE(out.desc.inputSlotsCount == 2);
    CHECK(out.desc.inputSlots[0].index == 0);
    CHECK(out.desc.inputSlots[0].stride == 16);
    CHECK(out.desc.inputSlots[0].classification == ::plume::RenderInputSlotClassification::PER_VERTEX_DATA);
    CHECK(out.desc.inputSlots[1].index == 3);
    CHECK(out.desc.inputSlots[1].classification == ::plume::RenderInputSlotClassification::PER_INSTANCE_DATA);
    REQUIRE(out.desc.inputElementsCount == 3);
    CHECK(out.desc.inputElements[1].location == 1);
    CHECK(out.desc.inputElements[1].slotIndex == 3);
    CHECK(out.desc.inputElements[1].format == ::plume::RenderFormat::R8G8B8A8_UINT);
    CHECK(out.desc.inputElements[2].format == ::plume::RenderFormat::R8G8_SINT);
    CHECK(out.desc.inputElements[2].alignedByteOffset == 12);
    // no colour attachment: no render target
    pipeline.color_format = Format::Undefined;
    pipeline.depth_format = Format::Undefined;
    core::PlumeGraphicsDesc none;
    REQUIRE(core::ToPlume(pipeline, nullptr, nullptr, nullptr, nullptr, &none).ok());
    CHECK(none.desc.renderTargetCount == 0);
    CHECK(none.desc.depthTargetFormat == ::plume::RenderFormat::UNKNOWN);
    // a depth format as a vertex attribute is refused
    pipeline.vertex_layout.attributes = {{0, 0, Format::D32S8, 0}};
    core::PlumeGraphicsDesc bad;
    CHECK_FALSE(core::ToPlume(pipeline, nullptr, nullptr, nullptr, nullptr, &bad).ok());
}

TEST_CASE("to_plume/every_format_has_its_own_plume_format") {
    // The table from every Format of rhi.h: UNKNOWN only for Undefined, and the 16-bit and depth formats
    // are the ones patch 11 added.
    CHECK(core::ToPlumeFormat(Format::Undefined) == ::plume::RenderFormat::UNKNOWN);
    CHECK(core::ToPlumeFormat(Format::D24S8) == ::plume::RenderFormat::D24_UNORM_S8_UINT);
    CHECK(core::ToPlumeFormat(Format::D32S8) == ::plume::RenderFormat::D32_FLOAT_S8_UINT);
    CHECK(core::ToPlumeFormat(Format::B5G6R5) == ::plume::RenderFormat::B5G6R5_UNORM);
    CHECK(core::ToPlumeFormat(Format::A1R5G5B5) == ::plume::RenderFormat::A1R5G5B5_UNORM);
    CHECK(core::ToPlumeFormat(Format::Rgb8) == ::plume::RenderFormat::R8G8B8A8_UNORM);  // RGB8 is always RGBA8
    CHECK(core::ToPlumeFormat(Format::Bc1Unorm) == ::plume::RenderFormat::BC1_UNORM);
    CHECK(core::ToPlumeFormat(Format::Bc3Unorm) == ::plume::RenderFormat::BC3_UNORM);
    CHECK(core::ToPlumeFormat(Format::Bc5Unorm) == ::plume::RenderFormat::BC5_UNORM);
    CHECK(core::ToPlumeFormat(Format::Bc7Unorm) == ::plume::RenderFormat::BC7_UNORM);
    // the packed 16-bit formats: the first letter is in the highest bits, as in Vulkan
    CHECK(core::ToPlumeFormat(Format::R5G6B5) == ::plume::RenderFormat::R5G6B5_UNORM);
    CHECK(core::ToPlumeFormat(Format::R5G5B5A1) == ::plume::RenderFormat::R5G5B5A1_UNORM);
    CHECK(core::ToPlumeFormat(Format::R4G4B4A4) == ::plume::RenderFormat::R4G4B4A4_UNORM);
    CHECK(core::ToPlumeFormat(Format::Rgb565) == ::plume::RenderFormat::R5G6B5_UNORM);  // the PICA names default to the R-first ones
    for (u32 i = 1; i <= static_cast<u32>(Format::Rgba16Sint); ++i) {
        CHECK_MESSAGE(core::ToPlumeFormat(static_cast<Format>(i)) != ::plume::RenderFormat::UNKNOWN, "format " << i);
    }
}

// ---- the ring's bookkeeping (no device) --------------------------------------------------------------------------

TEST_CASE("ring_alloc/wraps_and_reuses_only_after_the_value_has_passed") {
    using core::RingAllocator;
    RingAllocator ring(256);
    TimelineValue completed = 0;
    // fill in two spans, stamp them with values 1 and 2
    RingAllocator::Reservation r = ring.TryMap(100, 16, completed);
    REQUIRE(r.status == RingAllocator::Status::Ok);
    CHECK(r.offset == 0);
    CHECK_FALSE(r.wrapped);
    const u64 a = ring.Commit(100);
    r = ring.TryMap(100, 16, completed);
    REQUIRE(r.status == RingAllocator::Status::Ok);
    CHECK(r.offset == 112);  // aligned up from 100
    const u64 b = ring.Commit(100);
    // a third span does not fit at the end: it must wrap, and the start is in use by an unsubmitted span
    r = ring.TryMap(100, 16, completed);
    CHECK(r.status == RingAllocator::Status::Failed);
    CHECK(r.error.find("not been submitted") != std::string::npos);
    ring.Stamp(a, 1);
    r = ring.TryMap(100, 16, completed);
    CHECK(r.status == RingAllocator::Status::MustWait);  // value 1 has not passed
    CHECK(r.wait_for == 1);
    completed = 0;
    r = ring.TryMap(100, 16, completed);
    CHECK(r.status == RingAllocator::Status::MustWait);
    ring.Stamp(b, 2);
    completed = 1;  // a is free, b is not: the start [0, 100) is free but the wrapped allocation needs 100 bytes below b's start (112)
    r = ring.TryMap(100, 16, completed);
    REQUIRE(r.status == RingAllocator::Status::Ok);
    CHECK(r.offset == 0);
    CHECK(r.wrapped);
    const u64 c = ring.Commit(100);
    // now [0,100) is c (unstamped) and [112,212) is b (value 2): a 100-byte request has nowhere to go
    r = ring.TryMap(100, 16, completed);
    CHECK(r.status == RingAllocator::Status::MustWait);  // the oldest span is b (value 2): wait for it
    CHECK(r.wait_for == 2);
    (void)c;
}

TEST_CASE("ring_alloc/oldest_span_is_waited_for_and_everything_free_restarts") {
    using core::RingAllocator;
    RingAllocator ring(128);
    const auto fill = [&](TimelineValue stamp) {
        RingAllocator::Reservation r = ring.TryMap(64, 1, 0);
        REQUIRE(r.status == RingAllocator::Status::Ok);
        const u64 id = ring.Commit(64);
        ring.Stamp(id, stamp);
        return r.offset;
    };
    CHECK(fill(5) == 0);
    CHECK(fill(6) == 64);
    // the ring is full of stamped spans: the allocation must wait for the oldest, value 5
    RingAllocator::Reservation r = ring.TryMap(64, 1, 4);
    REQUIRE(r.status == RingAllocator::Status::MustWait);
    CHECK(r.wait_for == 5);
    // value 5 has passed: the start is free, the allocation wraps
    r = ring.TryMap(64, 1, 5);
    REQUIRE(r.status == RingAllocator::Status::Ok);
    CHECK(r.offset == 0);
    CHECK(r.wrapped);
    ring.Stamp(ring.Commit(64), 7);
    // everything has passed: the ring restarts at the beginning
    r = ring.TryMap(128, 1, 7);
    REQUIRE(r.status == RingAllocator::Status::Ok);
    CHECK(r.offset == 0);
    // a request above the capacity, and a bad alignment, fail
    CHECK(ring.TryMap(129, 1, 7).status == RingAllocator::Status::Failed);
    CHECK(ring.TryMap(8, 3, 7).status == RingAllocator::Status::Failed);
    // a span dropped with value 0 is free at once
    RingAllocator dropped(64);
    r = dropped.TryMap(64, 1, 0);
    REQUIRE(r.status == RingAllocator::Status::Ok);
    dropped.Stamp(dropped.Commit(64), 0);
    r = dropped.TryMap(64, 1, 0);
    CHECK(r.status == RingAllocator::Status::Ok);
    CHECK(r.offset == 0);
}

// ---- the instance and the device --------------------------------------------------------------------------------

TEST_CASE("instance/default_options_enable_no_surface_and_no_debug_extension") {
    std::shared_ptr<Instance> instance = PlainInstance();
    if (!instance) return;
    const std::vector<std::string>& names = instance->EnabledExtensions();
    CHECK_FALSE(AnyContains(names, "surface"));
    CHECK_FALSE(Has(names, "VK_EXT_debug_utils"));
    CHECK_FALSE(Has(names, "VK_EXT_headless_surface"));
}

TEST_CASE("instance/the_headless_surface_and_the_debug_messenger_come_when_asked") {
    InstanceOptions options;
    options.headless_surface = true;
    std::atomic<int> messages{0};
    options.debug_messages = [&](DebugSeverity, std::string_view) { ++messages; };
    std::shared_ptr<Instance> instance = test::RequireInstance(options);
    if (!instance) return;
    const std::vector<std::string>& names = instance->EnabledExtensions();
    // each only if the loader offers it; the loader of the cloud image and of Linux CI offers all three
    if (test::VulkanRequired()) {
        CHECK(Has(names, "VK_KHR_surface"));
        CHECK(Has(names, "VK_EXT_headless_surface"));
        CHECK(Has(names, "VK_EXT_debug_utils"));
    }
    CHECK((Has(names, "VK_EXT_headless_surface") == Has(names, "VK_KHR_surface")));
}

TEST_CASE("instance/an_extension_the_loader_lacks_fails_with_its_name") {
    InstanceOptions options;
    options.extensions = {"VK_MH4N_not_an_extension"};
    Result<std::shared_ptr<Instance>> instance = CreateInstance(options);
    if (instance.ok()) {
        // no Vulkan at all fails differently; with a loader this must fail
        FAIL("an instance was made with an extension nobody offers");
    }
    CHECK_FALSE(instance.error().empty());
    if (!AnyContains({instance.error()}, "loader") && !AnyContains({instance.error()}, "volkInitialize")) {
        CHECK(instance.error().find("VK_MH4N_not_an_extension") != std::string::npos);
    }
}

TEST_CASE("device/no_surface") {
    std::shared_ptr<Instance> instance = PlainInstance();
    if (!instance) return;
    std::shared_ptr<Backend> backend = test::RequireBackend({}, instance);
    if (!backend) return;
    const std::vector<std::string>& device = backend->EnabledExtensions();
    CHECK_FALSE(backend->HasSurface());
    CHECK_FALSE(AnyContains(device, "surface"));
    CHECK_FALSE(Has(device, "VK_KHR_swapchain"));
    CHECK_FALSE(Has(device, "VK_EXT_debug_utils"));
    CHECK_FALSE(AnyContains(instance->EnabledExtensions(), "surface"));
    CHECK_FALSE(Has(instance->EnabledExtensions(), "VK_EXT_debug_utils"));
    // never ray tracing, and none of what only ray tracing needs
    CHECK_FALSE(AnyContains(device, "ray_tracing"));
    CHECK_FALSE(AnyContains(device, "acceleration_structure"));
    CHECK_FALSE(AnyContains(device, "pipeline_library"));
    CHECK_FALSE(AnyContains(device, "deferred_host_operations"));
    CHECK_FALSE(AnyContains(device, "present"));
    CHECK_FALSE(AnyContains(device, "display_timing"));
    // the handles are there
    const VulkanHandles handles = Handles(*backend);
    CHECK(handles.instance != VK_NULL_HANDLE);
    CHECK(handles.physical_device != VK_NULL_HANDLE);
    CHECK(handles.device != VK_NULL_HANDLE);
    CHECK(handles.queue != VK_NULL_HANDLE);
}

TEST_CASE("device/headless_surface") {
    InstanceOptions options;
    options.headless_surface = true;
    std::shared_ptr<Instance> instance = test::RequireInstance(options);
    if (!instance) return;
    std::unique_ptr<test::HeadlessSurface> surface = test::MakeHeadlessSurface(instance);
    if (!surface) return;
    BackendOptions backend_options;
    backend_options.surface = surface->Handle();
    std::shared_ptr<Backend> backend = test::RequireBackend(backend_options, instance);
    if (!backend) return;
    CHECK(backend->HasSurface());
    CHECK(Has(backend->EnabledExtensions(), "VK_KHR_swapchain"));
    // the queue family the device chose can present to the surface
    const VulkanHandles handles = Handles(*backend);
    VkBool32 supported = VK_FALSE;
    REQUIRE(vkGetPhysicalDeviceSurfaceSupportKHR(handles.physical_device, handles.queue_family_index, surface->surface, &supported) == VK_SUCCESS);
    CHECK(supported == VK_TRUE);
    // the lock can be taken and released
    {
        QueueLock lock(*backend);
    }
}

TEST_CASE("device/chosen_by_capability_and_override") {
    std::shared_ptr<Instance> instance = test::RequireInstance();
    if (!instance) return;
    std::shared_ptr<Backend> first = test::RequireBackend({}, instance);
    if (!first) return;
    // the override picks the device by its "<name> (<driver>)" label
    const Capabilities& caps = first->Caps();
    BackendOptions options;
    options.device = caps.device_name + " (" + caps.driver_name + ")";
    Result<std::shared_ptr<Backend>> again = CreateBackend(instance, options);
    REQUIRE_MESSAGE(again.ok(), again.error());
    CHECK((*again)->Caps().device_index == caps.device_index);
    // the environment variable too; the option wins over it
    {
        ScopedEnv env("MH4N_VULKAN_DEVICE", caps.device_name.c_str());
        Result<std::shared_ptr<Backend>> from_env = CreateBackend(instance, {});
        REQUIRE_MESSAGE(from_env.ok(), from_env.error());
        CHECK((*from_env)->Caps().device_index == caps.device_index);
        BackendOptions nothing;
        nothing.device = "no such device";
        CHECK_FALSE(CreateBackend(instance, nothing).ok());
    }
}

TEST_CASE("device/override_that_matches_nothing_fails_with_the_list") {
    std::shared_ptr<Instance> instance = test::RequireInstance();
    if (!instance) return;
    BackendOptions options;
    options.device = "definitely not a GPU";
    Result<std::shared_ptr<Backend>> backend = CreateBackend(instance, options);
    REQUIRE_FALSE(backend.ok());
    CHECK(backend.error().find("definitely not a GPU") != std::string::npos);
    CHECK(backend.error().find("devices:") != std::string::npos);
    // the list names the real devices
    std::shared_ptr<Backend> real = test::RequireBackend({}, instance);
    if (!real) return;
    CHECK(backend.error().find(real->Caps().device_name) != std::string::npos);
}

// ---- the capabilities and the mask -------------------------------------------------------------------------------

TEST_CASE("caps/agree_with_the_probes_live_lines") {
    ScopedEnv no_mask("MH4N_RHI_MASK", nullptr);  // this case compares the unmasked device
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    const Capabilities& caps = backend->Caps();
    REQUIRE(caps.masked.bits == 0);  // an unmasked run (MH4N_RHI_MASK is not set for this case)

    std::vector<vkprobe::Facts> devices;
    std::string error;
    if (!vkprobe::ProbeLive(vkprobe::DefaultLoaderNames(), &devices, &error)) {
        FAIL("the probe found no device although the backend did: " << error);
    }
    REQUIRE(caps.device_index < devices.size());
    const vkprobe::Facts& facts = devices[caps.device_index];
    REQUIRE(facts.device_name == caps.device_name);
    const vkprobe::DeviceReport report = vkprobe::Evaluate(facts, "live");
    const auto yes = [&](const char* id) {
        const vkprobe::FeatureResult* line = report.Find(id);
        REQUIRE_MESSAGE(line != nullptr, "the probe has no line " << id);
        return line->status == vkprobe::Tri::Yes;
    };
    CHECK(caps.interlock == yes("fragmentShaderPixelInterlock"));
    CHECK(caps.index_uint8 == yes("indexTypeUint8"));
    CHECK(caps.custom_border_color == yes("customBorderColors"));
    CHECK(caps.triangle_fans == yes("triangleFans"));
    CHECK(caps.logic_op == yes("logicOp"));
    CHECK(caps.d24s8 == yes("D24_UNORM_S8_UINT"));
    CHECK(caps.bc7 == yes("BC7"));
    CHECK(caps.dynamic_rendering == yes("dynamicRendering"));
    CHECK(caps.pipeline_library == yes("graphicsPipelineLibrary"));
    CHECK(caps.timeline == yes("timelineSemaphore"));
    CHECK(caps.geometry_shader == yes("geometryShader"));
    CHECK(caps.present_wait == (yes("presentId") && yes("presentWait")));
    // extended dynamic state 3's four blend bits, from the member names the probe read
    const auto bit = [&](const char* name) {
        const auto it = facts.features.find(name);
        return it != facts.features.end() && it->second == vkprobe::Tri::Yes;
    };
    CHECK(caps.eds3_blend == (bit("extendedDynamicState3ColorBlendEnable") && bit("extendedDynamicState3ColorBlendEquation") &&
                              bit("extendedDynamicState3ColorWriteMask") && bit("extendedDynamicState3LogicOpEnable")));
    // the values
    CHECK(caps.device_name == facts.device_name);
    CHECK(caps.api_version == facts.api_version);
    CHECK(caps.vendor_id == facts.vendor_id);
    CHECK(caps.driver_name == facts.driver_name);
    CHECK(caps.apple == (caps.driver_id == 14 || caps.vendor_id == 0x106B));
    CHECK(caps.primitive_restart_always_on == caps.apple);
    CHECK(caps.portability_subset == (facts.extensions.count("VK_KHR_portability_subset") != 0));
    CHECK(caps.stride_alignment >= 1);
    CHECK(caps.timestamp_period > 0.0f);
    CHECK(caps.max_texel_buffer_elements >= 65536);
}

TEST_CASE("mask/each_bit_a_list_and_all") {
    ScopedEnv no_ambient_mask("MH4N_RHI_MASK", nullptr);  // the case sets the variable itself
    LogCapture log;
    std::vector<std::string> unmasked_extensions;
    const std::optional<Capabilities> base = CapsWith({}, &unmasked_extensions);
    if (!base) return;
    CHECK(log.Count("rhi: mask none") == 1);

    const auto check = [&](CapabilityMask mask, const std::string& text, bool through_environment) {
        const int before = log.Count("rhi: mask " + text);
        std::optional<Capabilities> masked;
        if (through_environment) {
            ScopedEnv env("MH4N_RHI_MASK", text.c_str());
            masked = CapsWith({});
        } else {
            masked = CapsWith(mask);
        }
        REQUIRE(masked.has_value());
        for (Cap cap : AllCaps()) {
            // exactly the masked bits are off; a bit the mask does not name is as it was; no mask turns a bit on
            CHECK_MESSAGE(masked->Has(cap) == (base->Has(cap) && !mask.Has(cap)), "bit " << CapName(cap) << " under the mask " << text);
        }
        CHECK(masked->masked == mask);
        CHECK(log.Count("rhi: mask " + text) == before + 1);  // logged once per backend
    };
    for (Cap cap : AllCaps()) {
        const CapabilityMask mask = CapabilityMask{}.With(cap);
        check(mask, std::string(CapName(cap)), false);
        check(mask, std::string(CapName(cap)), true);
    }
    check(CapabilityMask{}.With(Cap::D24S8).With(Cap::Bc7).With(Cap::Timeline), "d24s8,bc7,timeline", true);
    check(CapabilityMask{}.With(Cap::Interlock).With(Cap::LogicOp), "interlock,logic_op", false);
    check(CapabilityMask::All(), "all", true);
    check(CapabilityMask::All(), "all", false);
}

TEST_CASE("mask/an_unknown_name_fails_creation_with_the_list") {
    std::shared_ptr<Instance> instance = test::RequireInstance();
    if (!instance) return;
    ScopedEnv env("MH4N_RHI_MASK", "interlock,flux_capacitor");
    Result<std::shared_ptr<Backend>> backend = CreateBackend(instance, {});
    REQUIRE_FALSE(backend.ok());
    CHECK(backend.error().find("flux_capacitor") != std::string::npos);
    CHECK(backend.error().find("valid names") != std::string::npos);
    CHECK(backend.error().find("direct_upload") != std::string::npos);
}

TEST_CASE("mask/a_masked_feature_is_not_enabled_on_the_device") {
    // Lavapipe has these; the mask must make the device behave as one that lacks them.
    std::vector<std::string> on;
    const std::optional<Capabilities> base = CapsWith({}, &on);
    if (!base) return;
    const struct {
        Cap cap;
        const char* extension;
    } extensions[] = {{Cap::Interlock, "VK_EXT_fragment_shader_interlock"},
                      {Cap::IndexUint8, "VK_EXT_index_type_uint8"},
                      {Cap::CustomBorderColor, "VK_EXT_custom_border_color"}};
    for (const auto& row : extensions) {
        std::vector<std::string> off;
        BackendOptions options;
        options.mask = CapabilityMask{}.With(row.cap);
        std::shared_ptr<Backend> masked_backend = test::RequireBackend(options);
        if (!masked_backend) return;
        off = masked_backend->EnabledExtensions();
        const std::optional<Capabilities> masked = masked_backend->Caps();
        // the features the device was created with, as the vendored queue reports them
        const ::plume::RenderDeviceCapabilities& device_caps = masked_backend->impl().device->getCapabilities();
        if (row.cap == Cap::Interlock) CHECK_FALSE(device_caps.fragmentShaderInterlock);
        if (row.cap == Cap::IndexUint8) CHECK_FALSE(device_caps.indexTypeUint8);
        if (row.cap == Cap::CustomBorderColor) CHECK_FALSE(device_caps.customBorderColor);
        if (base->Has(row.cap)) {
            CHECK_MESSAGE(Has(on, row.extension), row.extension << " is enabled when the bit is on (unmasked)");
        }
        CHECK_MESSAGE(!Has(off, row.extension), row.extension << " must not be enabled under the mask " << CapName(row.cap));
        CHECK_FALSE(masked->Has(row.cap));
    }
    // the core features
    {
        BackendOptions options;
        options.mask = CapabilityMask{}.With(Cap::LogicOp).With(Cap::Bc7);
        std::shared_ptr<Backend> backend = test::RequireBackend(options);
        if (!backend) return;
        CHECK(backend->impl().device->enabledCoreFeatures.logicOp == VK_FALSE);
        CHECK(backend->impl().device->enabledCoreFeatures.textureCompressionBC == VK_FALSE);
        // the fallbacks: a pipeline with a logic op is refused, so the caller emulates it in the shader
        GraphicsPipelineDesc desc;
        desc.vertex.spirv = tri_vert;
        desc.fragment.spirv = color_frag;
        desc.color_format = Format::Rgba8Unorm;
        desc.state.logic_op_enabled = true;
        desc.state.logic_op = LogicOp::Xor;
        Result<std::shared_ptr<Pipeline>> pipeline = backend->CreateGraphicsPipeline(desc);
        CHECK_FALSE(pipeline.ok());
        CHECK(pipeline.error().find("logic") != std::string::npos);
    }
    // without the mask the same pipeline is made when the device has logic ops
    if (base->logic_op) {
        std::shared_ptr<Backend> backend = test::RequireBackend();
        if (!backend) return;
        CHECK(backend->impl().device->enabledCoreFeatures.logicOp == VK_TRUE);
        GraphicsPipelineDesc desc;
        desc.vertex.spirv = tri_vert;
        desc.fragment.spirv = color_frag;
        desc.color_format = Format::Rgba8Unorm;
        desc.state.logic_op_enabled = true;
        desc.state.logic_op = LogicOp::Xor;
        Result<std::shared_ptr<Pipeline>> pipeline = backend->CreateGraphicsPipeline(desc);
        CHECK_MESSAGE(pipeline.ok(), pipeline.error());
    }
}

TEST_CASE("mask/triangle_fans_are_refused_when_masked") {
    GraphicsPipelineDesc desc;
    desc.vertex.spirv = tri_vert;
    desc.fragment.spirv = color_frag;
    desc.color_format = Format::Rgba8Unorm;
    desc.topology = Topology::TriangleFan;
    std::shared_ptr<Backend> full = test::RequireBackend();
    if (!full) return;
    if (full->Caps().triangle_fans) {
        Result<std::shared_ptr<Pipeline>> pipeline = full->CreateGraphicsPipeline(desc);
        CHECK_MESSAGE(pipeline.ok(), pipeline.error());
    }
    BackendOptions options;
    options.mask = CapabilityMask{}.With(Cap::TriangleFans);
    std::shared_ptr<Backend> masked = test::RequireBackend(options);
    if (!masked) return;
    Result<std::shared_ptr<Pipeline>> pipeline = masked->CreateGraphicsPipeline(desc);
    REQUIRE_FALSE(pipeline.ok());
    CHECK(pipeline.error().find("fan") != std::string::npos);
    desc.topology = Topology::TriangleList;  // the rewrite the caller owes
    CHECK(masked->CreateGraphicsPipeline(desc).ok());
}

TEST_CASE("mask/the_fence_fallback_runs_without_the_timeline") {
    BackendOptions options;
    options.mask = CapabilityMask{}.With(Cap::Timeline);
    std::shared_ptr<Backend> backend = test::RequireBackend(options);
    if (!backend) return;
    CHECK_FALSE(backend->Caps().timeline);
    CHECK(backend->impl().timeline == VK_NULL_HANDLE);
    CHECK_FALSE(backend->impl().device->getCapabilities().timelineSemaphore);  // not enabled on the device either
    // submissions still return rising values, and waiting for them works
    const Scene scene = MakeScene(*backend, 2, 9);
    const std::shared_ptr<Pipeline> pipeline = ComputeWrite(*backend);
    TimelineValue last = 0;
    for (int i = 0; i < 4; ++i) {
        Recorder rec = backend->BeginRecording();
        rec.SetPipeline(*pipeline);
        rec.Bind(*scene.set, 0);
        rec.Dispatch(2, 1, 1);
        const TimelineValue value = backend->Submit(std::move(rec));
        CHECK(value > last);
        last = value;
    }
    backend->WaitIdle(last);
    CHECK(backend->CompletedValue() >= last);
    const std::vector<u32> words = ReadWords(*backend, *scene.storage, 8, last);
    for (u32 i = 0; i < 8; ++i) {
        CHECK(words[i] == i * 2 + 9);
    }
}

TEST_CASE("presentation/without_a_surface_nothing_is_available") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    CHECK_FALSE(backend->PresentWaitAvailable());
    CHECK_FALSE(backend->PresentWaitMeasuresDisplay());
    CHECK_FALSE(backend->DisplayTimingAvailable());
}

TEST_CASE("presentation/with_a_headless_surface_agrees_with_the_probe") {
    InstanceOptions options;
    options.headless_surface = true;
    std::shared_ptr<Instance> instance = test::RequireInstance(options);
    if (!instance) return;
    std::unique_ptr<test::HeadlessSurface> surface = test::MakeHeadlessSurface(instance);
    if (!surface) return;
    BackendOptions backend_options;
    backend_options.surface = surface->Handle();
    std::shared_ptr<Backend> backend = test::RequireBackend(backend_options, instance);
    if (!backend) return;

    std::vector<vkprobe::Facts> devices;
    std::string error;
    REQUIRE_MESSAGE(vkprobe::ProbeLive(vkprobe::DefaultLoaderNames(), &devices, &error), error);
    const vkprobe::DeviceReport report = vkprobe::Evaluate(devices[backend->Caps().device_index], "live");
    const vkprobe::FeatureResult* wait = report.Find("presentWait");
    REQUIRE(wait != nullptr);
    const bool probe_has_wait = wait->status == vkprobe::Tri::Yes && report.Find("presentId")->status == vkprobe::Tri::Yes;
    CHECK(backend->PresentWaitAvailable() == probe_has_wait);
    // what it measures follows the rule: false whenever the wait is not available
    CHECK(backend->PresentWaitMeasuresDisplay() ==
          core::PresentWaitMeasuresDisplay(backend->PresentWaitAvailable(), backend->Caps().driver_id, backend->Caps().driver_name));
    if (!backend->PresentWaitAvailable()) {
        CHECK_FALSE(backend->PresentWaitMeasuresDisplay());
    }
    CHECK(backend->DisplayTimingAvailable() == Has(backend->EnabledExtensions(), "VK_GOOGLE_display_timing"));

    // masking present_wait turns the availability off whatever the device offers
    BackendOptions masked_options = backend_options;
    masked_options.mask = CapabilityMask{}.With(Cap::PresentWait);
    std::shared_ptr<Backend> masked = test::RequireBackend(masked_options, instance);
    if (!masked) return;
    CHECK_FALSE(masked->PresentWaitAvailable());
    CHECK_FALSE(Has(masked->EnabledExtensions(), "VK_KHR_present_wait"));
    CHECK_FALSE(Has(masked->EnabledExtensions(), "VK_KHR_present_id"));
}

// ---- buffers, binding sets, pipelines ----------------------------------------------------------------------------

TEST_CASE("buffer/heaps_mapping_and_readback") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    std::shared_ptr<Buffer> upload = Make(*backend, 64, Heap::Upload, BufferUsage::Transfer);
    std::shared_ptr<Buffer> device = Make(*backend, 64, Heap::Device, BufferUsage::Transfer);
    std::shared_ptr<Buffer> readback = Make(*backend, 64, Heap::Readback, BufferUsage::Transfer);
    CHECK(upload->Mapped().size() == 64);
    CHECK(readback->Mapped().size() == 64);
    CHECK(device->Mapped().empty());
    CHECK(device->GetHeap() == Heap::Device);
    CHECK_FALSE(backend->CreateBuffer(BufferDesc{}).ok());  // size 0

    for (u32 i = 0; i < 16; ++i) {
        reinterpret_cast<u32*>(upload->Mapped().data())[i] = 0xC0DE0000u + i;
    }
    upload->Flush(0, 64);
    Recorder rec = backend->BeginRecording();
    rec.CopyBuffer(*device, 0, *upload, 0, 64);
    rec.Barrier(Stage::Transfer, Stage::Transfer);
    rec.CopyBuffer(*readback, 0, *device, 0, 64);
    const TimelineValue value = backend->Submit(std::move(rec));
    // from the device heap (through an internal copy) and from the readback heap (mapped)
    const std::vector<u32> from_device = ReadWords(*backend, *device, 16, value);
    const std::vector<u32> from_readback = ReadWords(*backend, *readback, 16, value);
    for (u32 i = 0; i < 16; ++i) {
        CHECK(from_device[i] == 0xC0DE0000u + i);
        CHECK(from_readback[i] == 0xC0DE0000u + i);
    }
    // at an offset that is not 0, from both heaps
    u32 four[4] = {};
    for (const std::shared_ptr<Buffer>& source : {device, readback}) {
        REQUIRE(backend->ReadBuffer(*source, 20, std::span<u8>(reinterpret_cast<u8*>(four), sizeof(four)), value).ok());
        for (u32 i = 0; i < 4; ++i) {
            CHECK(four[i] == 0xC0DE0000u + 5 + i);
        }
    }
    // a value that was never issued is an error, not an old read
    CHECK_FALSE(backend->ReadBuffer(*device, 0, std::span<u8>(reinterpret_cast<u8*>(four), sizeof(four)), value + 1000).ok());
    CHECK(backend->ReadBuffer(*readback, 0, std::span<u8>(reinterpret_cast<u8*>(four), sizeof(four)), value + 1000).error().find("issued") !=
          std::string::npos);
    // a read outside the buffer is an error, not a crash
    std::vector<u8> big(128);
    CHECK_FALSE(backend->ReadBuffer(*device, 0, big).ok());
}

TEST_CASE("bindings/checks") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    std::shared_ptr<Buffer> uniform = Make(*backend, 1024, Heap::Upload, BufferUsage::Uniform);
    std::shared_ptr<Buffer> storage = Make(*backend, 1024, Heap::Device, BufferUsage::Storage);
    std::shared_ptr<Buffer> plain = Make(*backend, 1024, Heap::Device, BufferUsage::Transfer);
    Result<std::shared_ptr<BindingSet>> set = backend->CreateBindingSet(StorageAndUniform());
    REQUIRE(set.ok());
    BindingSet& s = **set;
    CHECK(s.SetStorageBuffer(0, *storage, 0, 1024).ok());
    CHECK(s.SetUniformBuffer(1, *uniform, 0, 16).ok());
    CHECK_FALSE(s.SetUniformBuffer(0, *uniform, 0, 16).ok());  // binding 0 is a storage buffer
    CHECK_FALSE(s.SetStorageBuffer(7, *storage, 0, 16).ok());   // no such binding
    CHECK_FALSE(s.SetStorageBuffer(0, *plain, 0, 16).ok());     // made without Storage usage
    CHECK_FALSE(s.SetUniformBuffer(1, *uniform, 0, 4096).ok());  // out of the buffer
    CHECK_FALSE(s.SetUniformBuffer(1, *uniform, 1, 16).ok());    // not a multiple of the offset alignment
    const Capabilities& caps = backend->Caps();
    CHECK(caps.uniform_buffer_offset_alignment >= 1);
    CHECK(caps.storage_buffer_offset_alignment >= 1);
    CHECK(caps.max_uniform_buffer_range >= 16384);
    CHECK(caps.max_storage_buffer_range >= 1u << 27);
    const u64 alignment = std::max<u64>(caps.uniform_buffer_offset_alignment, 1);
    CHECK(s.SetUniformBuffer(1, *uniform, alignment, 16).ok());  // an offset that is
    // shapes with images are the targets module's
    BindingSetDesc with_image;
    with_image.slots = {{0, BindingType::SampledImage}};
    CHECK_FALSE(backend->CreateBindingSet(with_image).ok());
    BindingSetDesc twice;
    twice.slots = {{0, BindingType::UniformBuffer}, {0, BindingType::StorageBuffer}};
    CHECK_FALSE(backend->CreateBindingSet(twice).ok());
}

TEST_CASE("pipelines/state_variants_are_created_on_the_device") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    int made = 0;
    int attempted = 0;
    const auto create = [&](const FixedFunctionState& state, const char* what) {
        ++attempted;
        GraphicsPipelineDesc desc;
        desc.vertex.spirv = tri_vert;
        desc.fragment.spirv = color_frag;
        desc.color_format = Format::Rgba8Unorm;
        desc.depth_format = Format::D32S8;
        desc.state = state;
        desc.name = what;
        const Result<std::shared_ptr<Pipeline>> pipeline = backend->CreateGraphicsPipeline(desc);
        CHECK_MESSAGE(pipeline.ok(), what << ": " << pipeline.error());
        made += pipeline.ok() ? 1 : 0;
    };
    for (BlendOp op : {BlendOp::Add, BlendOp::Subtract, BlendOp::ReverseSubtract, BlendOp::Min, BlendOp::Max}) {
        FixedFunctionState s;
        s.blend_enabled = true;
        s.color_op = op;
        s.alpha_op = op;
        create(s, "blend op");
    }
    for (BlendFactor f : {BlendFactor::Zero, BlendFactor::One, BlendFactor::SrcColor, BlendFactor::OneMinusSrcColor, BlendFactor::DstColor,
                          BlendFactor::OneMinusDstColor, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendFactor::DstAlpha,
                          BlendFactor::OneMinusDstAlpha, BlendFactor::ConstantColor, BlendFactor::OneMinusConstantColor, BlendFactor::SrcAlphaSaturate}) {
        FixedFunctionState s;
        s.blend_enabled = true;
        s.src_color = f;
        s.dst_color = f;
        s.src_alpha = f;
        s.dst_alpha = f;
        create(s, "blend factor");
    }
    for (u8 mask : {u8(0), kWriteRed, u8(kWriteGreen | kWriteBlue), kWriteAlpha, kWriteAll}) {
        FixedFunctionState s;
        s.color_write_mask = mask;
        create(s, "write mask");
    }
    if (backend->Caps().logic_op) {
        for (LogicOp op : {LogicOp::Clear, LogicOp::Copy, LogicOp::Invert, LogicOp::Xor, LogicOp::OrInverted}) {
            FixedFunctionState s;
            s.logic_op_enabled = true;
            s.logic_op = op;
            create(s, "logic op");
        }
    }
    for (CompareOp op : {CompareOp::Never, CompareOp::Less, CompareOp::Equal, CompareOp::LessEqual, CompareOp::Greater, CompareOp::NotEqual,
                         CompareOp::GreaterEqual, CompareOp::Always}) {
        FixedFunctionState s;
        s.depth_test = true;
        s.depth_write = (op == CompareOp::Less);
        s.depth_compare = op;
        create(s, "depth compare");
    }
    for (StencilOp op : {StencilOp::Keep, StencilOp::Zero, StencilOp::Replace, StencilOp::IncrementClamp, StencilOp::DecrementClamp, StencilOp::Invert,
                         StencilOp::IncrementWrap, StencilOp::DecrementWrap}) {
        FixedFunctionState s;
        s.stencil_test = true;
        s.stencil_front = {op, op, op, CompareOp::Equal};
        s.stencil_back = {StencilOp::Keep, op, StencilOp::Zero, CompareOp::Less};
        create(s, "stencil op");
    }
    for (CullMode cull : {CullMode::None, CullMode::Front, CullMode::Back}) {
        for (FrontFace face : {FrontFace::Clockwise, FrontFace::CounterClockwise}) {
            FixedFunctionState s;
            s.cull = cull;
            s.front_face = face;
            create(s, "cull");
        }
    }
    {
        FixedFunctionState s;
        s.depth_bias_enabled = true;
        s.depth_bias_constant = 2.0f;
        s.depth_bias_slope = 1.5f;
        create(s, "depth bias");
    }
    CHECK(made == attempted);  // every state variant is made (the logic ops only where the device has them)
    CHECK(attempted >= 46);
}

TEST_CASE("pipelines/refused_before_the_driver_sees_them") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    GraphicsPipelineDesc base;
    base.vertex.spirv = tri_vert;
    base.fragment.spirv = color_frag;
    base.color_format = Format::Rgba8Unorm;
    CHECK(backend->CreateGraphicsPipeline(base).ok());
    GraphicsPipelineDesc d = base;
    d.vertex.spirv = {};
    CHECK_FALSE(backend->CreateGraphicsPipeline(d).ok());  // no vertex shader
    const u32 not_spirv[] = {1, 2, 3, 4};
    d = base;
    d.vertex.spirv = not_spirv;
    CHECK_FALSE(backend->CreateGraphicsPipeline(d).ok());
    d = base;
    d.state.logic_op_enabled = true;
    d.state.blend_enabled = true;
    CHECK_FALSE(backend->CreateGraphicsPipeline(d).ok());  // a logic op and blending
    d = base;
    d.state.blend_enabled = true;
    d.state.dst_alpha = BlendFactor::ConstantAlpha;
    const auto constant_alpha = backend->CreateGraphicsPipeline(d);
    CHECK_FALSE(constant_alpha.ok());
    CHECK(constant_alpha.error().find("constant-alpha") != std::string::npos);
    d = base;
    d.color_format = Format::D32S8;
    CHECK_FALSE(backend->CreateGraphicsPipeline(d).ok());
    d = base;
    d.depth_format = Format::Rgba8Unorm;
    CHECK_FALSE(backend->CreateGraphicsPipeline(d).ok());
    d = base;
    d.vertex_layout.bindings = {{0, 4, false}};
    d.vertex_layout.attributes = {{0, 1, Format::Rgba8Uint, 0}};  // binding 1 is not declared
    CHECK_FALSE(backend->CreateGraphicsPipeline(d).ok());
    d = base;
    d.vertex_layout.bindings.resize(kMaxVertexBindings + 1);
    CHECK_FALSE(backend->CreateGraphicsPipeline(d).ok());
    ComputePipelineDesc c;
    c.shader.spirv = {};
    CHECK_FALSE(backend->CreateComputePipeline(c).ok());
}

// ---- drawing, dispatching, reading back ---------------------------------------------------------------------------

TEST_CASE("pass/vertex_stage_in_a_pass_with_no_attachments_writes_a_storage_buffer") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    const Scene scene = MakeScene(*backend, 3, 5);
    const std::shared_ptr<Pipeline> pipeline = GraphicsWrite(*backend, vs_write_vert);
    Recorder rec = backend->BeginRecording();
    rec.BeginPass({16, 16});
    rec.SetPipeline(*pipeline);
    rec.Bind(*scene.set, 0);
    rec.Draw(12);
    rec.EndPass();
    const TimelineValue value = backend->Submit(std::move(rec));
    CHECK(value >= 1);
    const std::vector<u32> words = ReadWords(*backend, *scene.storage, 12, value);
    for (u32 i = 0; i < 12; ++i) {
        CHECK(words[i] == i * 3 + 5);
    }
}

TEST_CASE("pass/vertex_buffers_one_buffer_at_two_offsets_and_indexed_draws") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    const Scene scene = MakeScene(*backend, 0, 0);
    // one buffer, 12 vertices of 4 bytes at offset 0 (a) and again at offset 64 (b)
    std::shared_ptr<Buffer> vertices = Make(*backend, 256, Heap::Upload, BufferUsage::Vertex | BufferUsage::Index);
    u8* bytes = vertices->Mapped().data();
    for (u32 i = 0; i < 12; ++i) {
        const u8 a[4] = {u8(i), u8(i % 3), 0, 0};
        const u8 b[4] = {u8(i + 1), 0, 0, 0};
        std::memcpy(bytes + 4 * i, a, 4);
        std::memcpy(bytes + 64 + 4 * i, b, 4);
    }
    const u16 indices[3] = {2, 7, 9};
    std::memcpy(bytes + 128, indices, sizeof(indices));
    vertices->Flush(0, 256);

    VertexLayout layout;
    layout.bindings = {{0, 4, false}, {1, 4, false}};
    layout.attributes = {{0, 0, Format::Rgba8Uint, 0}, {1, 1, Format::Rgba8Uint, 0}};
    BindingSetDesc only_storage;
    only_storage.slots = {{0, BindingType::StorageBuffer}};
    GraphicsPipelineDesc desc;
    desc.vertex.spirv = vs_attr_vert;
    desc.vertex_layout = layout;
    desc.sets = {only_storage};
    Result<std::shared_ptr<Pipeline>> pipeline = backend->CreateGraphicsPipeline(desc);
    REQUIRE_MESSAGE(pipeline.ok(), pipeline.error());
    Result<std::shared_ptr<BindingSet>> set = backend->CreateBindingSet(only_storage);
    REQUIRE(set.ok());
    REQUIRE((*set)->SetStorageBuffer(0, *scene.storage, 0, 64 * 4).ok());
    std::shared_ptr<Buffer> zeros = Make(*backend, 64 * 4, Heap::Upload, BufferUsage::Transfer);
    std::memset(zeros->Mapped().data(), 0, 64 * 4);
    zeros->Flush(0, 64 * 4);

    Recorder rec = backend->BeginRecording();
    rec.CopyBuffer(*scene.storage, 0, *zeros, 0, 64 * 4);
    rec.Barrier(Stage::Transfer, Stage::VertexShader);
    rec.BeginPass({8, 8});
    rec.SetPipeline(**pipeline);
    rec.Bind(**set, 0);
    const VertexBufferBinding bindings[2] = {{vertices.get(), 0}, {vertices.get(), 64}};
    rec.SetVertexBuffers(0, bindings);
    rec.Draw(12);
    rec.EndPass();
    rec.Barrier(Stage::VertexShader, Stage::VertexShader);
    // indexed: only the vertices 2, 7 and 9 run, so only those words change (they are rewritten with the same values)
    rec.BeginPass({8, 8});
    rec.SetPipeline(**pipeline);
    rec.Bind(**set, 0);
    rec.SetVertexBuffers(0, bindings);
    rec.SetIndexBuffer(*vertices, 128, IndexType::U16);
    rec.DrawIndexed(3);
    rec.EndPass();
    const TimelineValue value = backend->Submit(std::move(rec));
    const std::vector<u32> words = ReadWords(*backend, *scene.storage, 12, value);
    for (u32 i = 0; i < 12; ++i) {
        CHECK(words[i] == i + 256u * (i % 3) + 1000000u * (i + 1));
    }

    // the indexed draw alone writes exactly the indexed words
    Recorder zero = backend->BeginRecording();
    zero.CopyBuffer(*scene.storage, 0, *zeros, 0, 64 * 4);
    zero.Barrier(Stage::Transfer, Stage::VertexShader);
    zero.BeginPass({8, 8});
    zero.SetPipeline(**pipeline);
    zero.Bind(**set, 0);
    zero.SetVertexBuffers(0, bindings);
    zero.SetIndexBuffer(*vertices, 128, IndexType::U16);
    zero.DrawIndexed(3);
    zero.EndPass();
    const std::vector<u32> only = ReadWords(*backend, *scene.storage, 12, backend->Submit(std::move(zero)));
    for (u32 i = 0; i < 12; ++i) {
        const bool drawn = (i == 2 || i == 7 || i == 9);
        CHECK_MESSAGE(only[i] == (drawn ? i + 256u * (i % 3) + 1000000u * (i + 1) : 0u), "word " << i);
    }
}

TEST_CASE("compute/a_compute_pipeline_writes_a_storage_buffer") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    const Scene scene = MakeScene(*backend, 7, 1);
    const std::shared_ptr<Pipeline> pipeline = ComputeWrite(*backend);
    CHECK(pipeline->IsCompute());
    Recorder rec = backend->BeginRecording();
    rec.SetPipeline(*pipeline);
    rec.Bind(*scene.set, 0);
    rec.Dispatch(4, 1, 1);  // 16 invocations
    const TimelineValue value = backend->Submit(std::move(rec));
    const std::vector<u32> words = ReadWords(*backend, *scene.storage, 16, value);
    for (u32 i = 0; i < 16; ++i) {
        CHECK(words[i] == i * 7 + 1);
    }
}

// ---- rings --------------------------------------------------------------------------------------------------------

TEST_CASE("ring/wraps_and_reuses_space_only_after_its_value_has_passed") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    Result<std::unique_ptr<Ring>> made = backend->CreateRing(256, BufferUsage::Uniform | BufferUsage::Storage, "ring");
    REQUIRE_MESSAGE(made.ok(), made.error());
    Ring& ring = **made;
    CHECK(ring.Size() == 256);
    CHECK(ring.GetBuffer()->GetHeap() == Heap::Upload);

    // Commit needs a recorder
    Result<RingAllocation> first = ring.Map(100, 16);
    REQUIRE(first.ok());
    CHECK(first->offset == 0);
    CHECK(first->data.size() == 100);
    CHECK_FALSE(first->wrapped);
    std::memset(first->data.data(), 0xAB, 100);
    CHECK_FALSE(ring.Commit(100).ok());
    Recorder rec = backend->BeginRecording();
    REQUIRE(ring.Commit(100).ok());
    Result<RingAllocation> second = ring.Map(100, 16);
    REQUIRE(second.ok());
    CHECK(second->offset == 112);
    REQUIRE(ring.Commit(100).ok());
    CHECK_FALSE(ring.Commit(200).ok());  // more than the last Map reserved

    // the end is reached and the start is held by a recorder that has not been submitted
    Result<RingAllocation> third = ring.Map(100, 16);
    REQUIRE_FALSE(third.ok());
    CHECK(third.error().find("submit") != std::string::npos);
    CHECK_FALSE(ring.Map(1000, 1).ok());  // larger than the buffer

    // submitted and passed: the space is reused from the start
    const TimelineValue value = backend->Submit(std::move(rec));
    backend->WaitIdle(value);
    third = ring.Map(100, 16);
    REQUIRE_MESSAGE(third.ok(), third.error());
    CHECK(third->wrapped);
    CHECK(third->offset == 0);

    // a recorder dropped without a submission frees its space at once
    {
        Recorder dropped = backend->BeginRecording();
        REQUIRE(ring.Commit(100).ok());
        Result<RingAllocation> next = ring.Map(100, 16);
        REQUIRE(next.ok());
        REQUIRE(ring.Commit(100).ok());
        CHECK_FALSE(ring.Map(100, 16).ok());  // held by `dropped`
    }
    Result<RingAllocation> after_drop = ring.Map(100, 16);
    CHECK_MESSAGE(after_drop.ok(), after_drop.error());
}

// ---- submission, threads, the owner rule --------------------------------------------------------------------------

TEST_CASE("submit/two_threads_each_recording_and_submitting_through_the_lock") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    BreachCapture breaches;
    const std::shared_ptr<Pipeline> pipeline = ComputeWrite(*backend);
    // Four threads, fifty rounds each. Removing the submit mutex would break the order of the values only
    // now and then, so a mutation of the lock is caught reliably only by the layer run (synchronisation
    // validation) and by ThreadSanitizer, which run this case.
    constexpr int kThreads = 4;
    constexpr int kRounds = 50;
    std::atomic<int> wrong{0};
    std::atomic<int> unordered{0};
    std::vector<Scene> scenes;
    for (int t = 0; t < kThreads; ++t) {
        scenes.push_back(MakeScene(*backend, u32(t + 2), u32(10 * t), 16));
    }
    std::vector<std::vector<TimelineValue>> values(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int round = 0; round < kRounds; ++round) {
                Recorder rec = backend->BeginRecording();
                rec.SetPipeline(*pipeline);
                rec.Bind(*scenes[t].set, 0);
                rec.Dispatch(2, 1, 1);
                const TimelineValue value = backend->Submit(std::move(rec));
                if (!values[t].empty() && value <= values[t].back()) {
                    ++unordered;
                }
                values[t].push_back(value);
                std::vector<u32> words(8);
                if (!backend->ReadBuffer(*scenes[t].storage, 0, std::span<u8>(reinterpret_cast<u8*>(words.data()), 32), value).ok()) {
                    ++wrong;
                    continue;
                }
                for (u32 i = 0; i < 8; ++i) {
                    wrong += (words[i] != i * u32(t + 2) + u32(10 * t)) ? 1 : 0;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(wrong.load() == 0);
    CHECK(unordered.load() == 0);
    CHECK(breaches.Total() == 0);
    // all the values are distinct, across both threads
    std::vector<TimelineValue> all;
    for (const auto& v : values) {
        all.insert(all.end(), v.begin(), v.end());
    }
    std::sort(all.begin(), all.end());
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
}

TEST_CASE("owner_rule/a_second_open_recorder_on_one_thread_calls_the_handler") {
    if (!kRhiChecks) {
        WARN_MESSAGE(false, "skipped: the owner-rule checks are compiled out (MH4N_RHI_CHECKS=OFF)");
        return;
    }
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    BreachCapture breaches;
    {
        Recorder a = backend->BeginRecording();
        Recorder b = backend->BeginRecording();
        CHECK(breaches.Count(OwnerRule::OneOpenRecorder) == 1);
        backend->Submit(std::move(b));
        backend->Submit(std::move(a));  // submitted after a later one: the second breach
    }
    CHECK(breaches.Count(OwnerRule::OneOpenRecorder) == 1);
    CHECK(breaches.Count(OwnerRule::SubmissionOrder) == 1);
    CHECK(breaches.Total() == 2);
    // a thread that submits what it began, one at a time, breaks nothing; neither does dropping one
    const size_t before = breaches.Total();
    for (int i = 0; i < 3; ++i) {
        backend->Submit(backend->BeginRecording());
    }
    { Recorder dropped = backend->BeginRecording(); }
    backend->Submit(backend->BeginRecording());
    CHECK(breaches.Total() == before);
    // another thread has its own count
    std::thread other([&] {
        Recorder r = backend->BeginRecording();
        backend->Submit(std::move(r));
    });
    Recorder mine = backend->BeginRecording();
    other.join();
    backend->Submit(std::move(mine));
    CHECK(breaches.Total() == before);
}

TEST_CASE("owner_rule/a_recorder_with_a_required_wait_needs_a_wait_that_covers_it") {
    if (!kRhiChecks) {
        WARN_MESSAGE(false, "skipped: the owner-rule checks are compiled out (MH4N_RHI_CHECKS=OFF)");
        return;
    }
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    BreachCapture breaches;
    const TimelineValue produced = backend->Submit(backend->BeginRecording());

    // through Submit(Recorder&&): any required wait is a breach, there is no wait to cover it
    {
        Recorder rec = backend->BeginRecording();
        rec.impl().required_waits.push_back(Wait{produced, Stage::FragmentShader});
        backend->Submit(std::move(rec));
        CHECK(breaches.Count(OwnerRule::RequiredWait) == 1);
    }
    // through the internal path N2-28's Submit uses: waits that cover it, then ones that do not
    const auto submit_with = [&](std::vector<Wait> required, std::vector<Wait> given) {
        Recorder rec = backend->BeginRecording();
        rec.impl().required_waits = std::move(required);
        core::SubmitRecorder(backend->impl(), std::move(rec), given);
    };
    const size_t base = breaches.Total();
    submit_with({Wait{produced, Stage::FragmentShader}}, {Wait{produced, Stage::FragmentShader}});
    CHECK(breaches.Total() == base);  // the same value and stage
    submit_with({Wait{produced, Stage::FragmentShader}}, {Wait{produced + 0, Stage::FragmentShader}, Wait{produced, Stage::VertexShader}});
    CHECK(breaches.Total() == base);  // among several waits
    submit_with({Wait{produced, Stage::FragmentShader}}, {Wait{produced, Stage::VertexShader}});
    CHECK(breaches.Total() == base + 1);  // another stage
    submit_with({Wait{produced, Stage::FragmentShader}}, {});
    CHECK(breaches.Total() == base + 2);  // no wait
    submit_with({Wait{produced + 1, Stage::FragmentShader}}, {Wait{produced, Stage::FragmentShader}});
    CHECK(breaches.Total() == base + 3);  // a lower value
    submit_with({Wait{produced, Stage::FragmentShader}}, {Wait{produced + 1, Stage::FragmentShader}});
    CHECK(breaches.Total() == base + 3);  // a higher value covers a lower one
    CHECK(breaches.Count(OwnerRule::RequiredWait) == 4);
    backend->WaitIdle();
}

TEST_CASE("owner_rule/the_default_handler_is_replaceable_and_restored") {
    int seen = 0;
    const OwnerRuleHandler previous = SetOwnerRuleHandler([&](const OwnerRuleBreach& b) {
        seen += (b.rule == OwnerRule::WrongThread && b.message == "x") ? 1 : 0;
    });
    ReportOwnerRuleBreach(OwnerRule::WrongThread, "x");
    CHECK(seen == 1);
    const OwnerRuleHandler mine = SetOwnerRuleHandler(previous);
    CHECK(static_cast<bool>(mine));  // the call returned the handler it replaced
}

// ---- more checks on names, reads, the queue lock and pipelines ---------------------------------------------------

TEST_CASE("mask/the_name_of_each_bit_is_a_literal") {
    // Written out, not derived from the table under test: a swapped pair of names shows.
    const struct {
        const char* name;
        Cap cap;
    } table[] = {{"d24s8", Cap::D24S8},
                 {"interlock", Cap::Interlock},
                 {"index_uint8", Cap::IndexUint8},
                 {"custom_border_color", Cap::CustomBorderColor},
                 {"triangle_fans", Cap::TriangleFans},
                 {"logic_op", Cap::LogicOp},
                 {"dynamic_rendering", Cap::DynamicRendering},
                 {"pipeline_library", Cap::PipelineLibrary},
                 {"eds3_blend", Cap::Eds3Blend},
                 {"bc7", Cap::Bc7},
                 {"timeline", Cap::Timeline},
                 {"present_wait", Cap::PresentWait},
                 {"direct_upload", Cap::DirectUpload}};
    CHECK(sizeof(table) / sizeof(table[0]) == kCapCount);
    for (const auto& row : table) {
        CHECK(CapName(row.cap) == row.name);
        const Result<CapabilityMask> parsed = CapabilityMask::Parse(row.name);
        REQUIRE(parsed.ok());
        CHECK(parsed->bits == (1u << static_cast<u32>(row.cap)));
        CHECK(CapabilityMask{}.With(row.cap).ToString() == row.name);
    }
    // and Capabilities::Has reads the field of the same name
    Capabilities caps;
    caps.dynamic_rendering = true;
    for (const auto& row : table) {
        CHECK(caps.Has(row.cap) == (row.cap == Cap::DynamicRendering));
    }
}

TEST_CASE("buffer/a_read_right_after_a_submission_waits_for_it") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    // Big copies so that the GPU is not done when Submit returns: the read must wait for `after`.
    constexpr u64 kSize = 48ull << 20;
    std::shared_ptr<Buffer> upload = Make(*backend, kSize, Heap::Upload, BufferUsage::Transfer);
    std::shared_ptr<Buffer> device = Make(*backend, kSize, Heap::Device, BufferUsage::Transfer);
    std::shared_ptr<Buffer> readback = Make(*backend, kSize, Heap::Readback, BufferUsage::Transfer);
    std::memset(upload->Mapped().data(), 0xA5, kSize);
    std::memset(readback->Mapped().data(), 0, kSize);
    upload->Flush(0, kSize);
    readback->Flush(0, kSize);
    Recorder rec = backend->BeginRecording();
    rec.CopyBuffer(*device, 0, *upload, 0, kSize);
    rec.Barrier(Stage::Transfer, Stage::Transfer);
    rec.CopyBuffer(*readback, 0, *device, 0, kSize);
    const TimelineValue value = backend->Submit(std::move(rec));
    u8 bytes[16] = {};
    REQUIRE(backend->ReadBuffer(*readback, kSize - 1000, bytes, value).ok());  // no other wait
    for (u8 byte : bytes) {
        CHECK(byte == 0xA5);
    }
    backend->WaitIdle();
}

TEST_CASE("submit/the_queue_lock_holds_back_a_submission") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    std::atomic<bool> submitted{false};
    std::thread other;
    {
        QueueLock lock(*backend);
        other = std::thread([&] {
            backend->Submit(backend->BeginRecording());
            submitted = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK_FALSE(submitted.load());  // Submit takes the same lock
    }
    other.join();
    CHECK(submitted.load());
    // the lock is released: it can be taken again, by another thread too
    std::thread again([&] { QueueLock lock(*backend); });
    again.join();
}

TEST_CASE("pipelines/constant_alpha_and_fractional_bias_at_the_device") {
    std::shared_ptr<Backend> backend = test::RequireBackend();
    if (!backend) return;
    GraphicsPipelineDesc desc;
    desc.vertex.spirv = tri_vert;
    desc.fragment.spirv = color_frag;
    desc.color_format = Format::Rgba8Unorm;
    // blending off: the factors are not used, and the driver is given valid ones (clean under the layer)
    desc.state.blend_enabled = false;
    desc.state.src_color = BlendFactor::ConstantAlpha;
    desc.state.dst_color = BlendFactor::OneMinusConstantAlpha;
    desc.state.src_alpha = BlendFactor::ConstantAlpha;
    desc.state.dst_alpha = BlendFactor::OneMinusConstantAlpha;
    const Result<std::shared_ptr<Pipeline>> off = backend->CreateGraphicsPipeline(desc);
    CHECK_MESSAGE(off.ok(), off.error());
    // blending on: refused with a message
    desc.state.blend_enabled = true;
    const Result<std::shared_ptr<Pipeline>> on = backend->CreateGraphicsPipeline(desc);
    REQUIRE_FALSE(on.ok());
    CHECK(on.error().find("constant-alpha") != std::string::npos);
    // a fractional constant depth bias is refused by CreateGraphicsPipeline, a whole one is made
    GraphicsPipelineDesc bias;
    bias.vertex.spirv = tri_vert;
    bias.fragment.spirv = color_frag;
    bias.color_format = Format::Rgba8Unorm;
    bias.state.depth_bias_enabled = true;
    bias.state.depth_bias_constant = 0.5f;
    const Result<std::shared_ptr<Pipeline>> fractional = backend->CreateGraphicsPipeline(bias);
    REQUIRE_FALSE(fractional.ok());
    CHECK(fractional.error().find("whole number") != std::string::npos);
    bias.state.depth_bias_constant = 4.0f;
    bias.state.depth_bias_slope = 0.25f;
    const Result<std::shared_ptr<Pipeline>> whole = backend->CreateGraphicsPipeline(bias);
    CHECK_MESSAGE(whole.ok(), whole.error());
}

// ---- loading ----------------------------------------------------------------------------------------------------

// Run by the case below in a process whose Vulkan drivers have been taken away.
TEST_CASE("loading/child" * doctest::skip()) {
    Result<std::shared_ptr<Instance>> instance = CreateInstance();
    std::string message;
    if (!instance.ok()) {
        message = instance.error();
    } else {
        Result<std::shared_ptr<Backend>> backend = CreateBackend(*instance);
        REQUIRE_FALSE(backend.ok());
        message = backend.error();
    }
    REQUIRE_FALSE(message.empty());
    std::printf("child: error: %s\n", message.c_str());
    std::fflush(stdout);
}

TEST_CASE("loading/no_driver_gives_an_error_with_a_message_and_the_process_goes_on") {
#if defined(__linux__)
    namespace fs = std::filesystem;
    char exe[4096] = {};
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    REQUIRE(length > 0);
    const fs::path empty = fs::temp_directory_path() / ("mh4n_empty_icd_" + std::to_string(getpid()));
    fs::create_directories(empty);
    const fs::path out = empty / "out.txt";
    // a driver folder with no driver in it, for the current and the legacy variable; the layers are taken away too
    const std::string command = "env -u VK_INSTANCE_LAYERS VK_DRIVER_FILES='" + empty.string() + "' VK_ICD_FILENAMES='" + empty.string() + "' '" +
                                std::string(exe) + "' --test-case='loading/child' --no-skip > '" + out.string() + "' 2>&1";
    const int status = std::system(command.c_str());
    std::ifstream in(out);
    std::stringstream text;
    text << in.rdbuf();
    fs::remove_all(empty);
    CHECK_MESSAGE((status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0), "the child's output: " << text.str());
    CHECK(text.str().find("child: error: ") != std::string::npos);
    CHECK(text.str().find("error: \n") == std::string::npos);  // the message is not empty
#else
    WARN_MESSAGE(false, "skipped: the subprocess check runs on Linux");
#endif
}
