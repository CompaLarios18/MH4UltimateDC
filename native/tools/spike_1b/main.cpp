// mh4n_spike_1b (N2-35): option 1b against option 1 on one GPU. A spike, not a product tool: nothing links this,
// there is no ctest, and it is built only with -DMH4N_SPIKE_1B=ON. It prints the device, the keys, the miss latency,
// the code quality and the validity of 24 synthetic keys, evaluates the thresholds T1 and T2 of the spec
// (docs/native/wp/N2a.md, N2-35) and says in one line whether 1b passes. One run is one fresh process.
//
// usage: mh4n_spike_1b [--device TEXT] [--reps N] [--salt TEXT] [--keys N] [--dump DIR] [--force]
//   --device  substring of "<device name> (<driver name>)" (default: $MH4N_VULKAN_DEVICE, else the first discrete)
//   --reps    GPU-time repetitions per pipeline (default 5)
//   --salt    adds an OpSourceExtension with this text to every SPIR-V module: another hash for a driver's shader
//             cache, the same code (see docs/research/spike-1b.md for why a run may need it)
//   --keys    use only the first N keys (a smoke test; the thresholds are evaluated over what ran)
//   --dump    write the GLSL of the ubershader and of every literal shader to DIR
//   --force   measure even if a key is invalid (the verdict then says so)
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpu.h"
#include "key.h"
#include "shaders.h"

namespace {

using spike::Key;

double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double Median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Nearest rank: the ceil(0.95 n)-th smallest.
double P95(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    size_t rank = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(v.size())));
    rank = std::min(std::max<size_t>(rank, 1), v.size());
    return v[rank - 1];
}

double Max(const std::vector<double>& v) { return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()); }

spike::SpecConstants MakeSpec(uint32_t uber, const Key* key) {
    spike::SpecConstants s;
    s.data.assign(spike::kFieldCount + 1, 0);
    s.data[0] = uber;
    if (key != nullptr) {
        for (uint32_t f = 0; f < spike::kFieldCount; ++f) {
            s.data[1 + f] = key->w[f];
        }
    }
    for (uint32_t i = 0; i <= spike::kFieldCount; ++i) {
        s.entries.push_back({i, i * 4, 4});
    }
    return s;
}

// The fragment uniforms in std140 order (see the prologue of shaders.cpp): 33 vec4s of synthetic data.
std::vector<float> MakeFragUniforms() {
    std::vector<float> f(33 * 4, 0.0f);
    auto set = [&](size_t vec, float x, float y, float z, float w) {
        f[vec * 4 + 0] = x;
        f[vec * 4 + 1] = y;
        f[vec * 4 + 2] = z;
        f[vec * 4 + 3] = w;
    };
    for (size_t k = 0; k < 6; ++k) { // tev_const
        const float t = static_cast<float>(k);
        set(k, 0.22f + 0.05f * t, 0.30f - 0.03f * t, 0.25f + 0.04f * t, 0.55f + 0.06f * t);
    }
    set(6, 0.25f, 0.50f, 0.75f, 0.50f);                // tev_cbuf
    set(7, 0.04f, 0.04f, 0.05f, 0.0f);                 // global_ambient
    set(8, 0.50f, 0.30f, 0.80f, 0.0f);                 // light_pos
    set(9, -0.40f, 0.60f, 0.70f, 0.0f);
    set(10, 0.20f, -0.50f, 0.90f, 0.0f);
    set(11, 0.0f, 0.0f, -1.0f, 0.0f);                  // light_spot
    set(12, 0.3f, 0.2f, -0.9f, 0.0f);
    set(13, -0.2f, 0.4f, -0.8f, 0.0f);
    set(14, 0.15f, 0.13f, 0.11f, 1.0f);                // light_diffuse
    set(15, 0.10f, 0.14f, 0.17f, 1.0f);
    set(16, 0.12f, 0.12f, 0.07f, 1.0f);
    set(17, 0.22f, 0.20f, 0.17f, 1.0f);                // light_specular0
    set(18, 0.15f, 0.17f, 0.22f, 1.0f);
    set(19, 0.12f, 0.22f, 0.15f, 1.0f);
    set(20, 0.08f, 0.10f, 0.12f, 1.0f);                // light_specular1
    set(21, 0.12f, 0.08f, 0.10f, 1.0f);
    set(22, 0.10f, 0.12f, 0.08f, 1.0f);
    set(23, 0.02f, 0.02f, 0.01f, 1.0f);                // light_ambient
    set(24, 0.01f, 0.02f, 0.02f, 1.0f);
    set(25, 0.02f, 0.01f, 0.02f, 1.0f);
    // 26 and 27: lut_offset (ivec4[2]), zero bits are the integer 0
    set(28, 0.10f, 0.40f, 0.70f, 0.20f);               // tex_color
    set(29, 0.60f, 0.30f, 0.15f, 0.45f);
    set(30, 0.35f, 0.75f, 0.55f, 0.65f);
    set(31, 0.85f, 0.25f, 0.45f, 0.05f);
    set(32, 16.0f, 1.0f, 0.0f, 0.5f);                  // params: alpha reference, depth scale, offset, distance scale
    return f;
}

// Eight tables of 256 entries: a ramp with a different exponent and a small ripple each.
std::vector<float> MakeLut() {
    std::vector<float> lut(8 * 256);
    for (size_t j = 0; j < 8; ++j) {
        for (size_t i = 0; i < 256; ++i) {
            const double t = static_cast<double>(i) / 255.0;
            const double v = 0.15 + 0.85 * std::pow(t, 0.5 + 0.2 * static_cast<double>(j)) +
                             0.05 * std::sin(static_cast<double>(i) * 0.11 + static_cast<double>(j));
            lut[j * 256 + i] = static_cast<float>(std::min(std::max(v, 0.0), 1.5));
        }
    }
    return lut;
}

struct ImageCompare {
    int max_diff = 0;
    double psnr = 99.0; // 99 = identical
    double max_block_mean = 0.0;
    bool bit_equal = false;
    uint32_t distinct = 0;
    double mean[3] = {0, 0, 0}; // of the first image, RGB in 0..255
};

ImageCompare Compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    ImageCompare c;
    const uint32_t n = spike::kSmallSize;
    double sq = 0.0;
    c.bit_equal = true;
    std::vector<double> block((n / 16) * (n / 16), 0.0);
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            for (uint32_t ch = 0; ch < 4; ++ch) {
                const size_t i = (static_cast<size_t>(y) * n + x) * 4 + ch;
                const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
                c.max_diff = std::max(c.max_diff, d);
                sq += static_cast<double>(d) * d;
                block[(y / 16) * (n / 16) + (x / 16)] += d;
                if (d != 0) {
                    c.bit_equal = false;
                }
            }
        }
    }
    if (sq > 0.0) {
        c.psnr = 10.0 * std::log10(255.0 * 255.0 / (sq / (static_cast<double>(n) * n * 4.0)));
    }
    for (double s : block) {
        c.max_block_mean = std::max(c.max_block_mean, s / (16.0 * 16.0 * 4.0));
    }
    std::map<uint32_t, int> colors;
    for (size_t i = 0; i + 3 < a.size(); i += 4) {
        uint32_t v = 0;
        std::memcpy(&v, &a[i], 4);
        colors[v] = 1;
        for (size_t ch = 0; ch < 3; ++ch) {
            c.mean[ch] += a[i + ch];
        }
    }
    for (double& m : c.mean) {
        m /= static_cast<double>(n) * n;
    }
    c.distinct = static_cast<uint32_t>(colors.size());
    return c;
}

bool ContainsNoCase(const std::string& hay, const char* needle) {
    std::string h = hay;
    std::string n = needle;
    for (char& ch : h) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return h.find(n) != std::string::npos;
}

// The fragment-stage statistic whose name contains one of the words, as the driver names it.
bool FindStat(const std::vector<spike::ExecutableStat>& stats, const std::vector<const char*>& words,
              spike::ExecutableStat& out) {
    for (const char* w : words) {
        for (const spike::ExecutableStat& s : stats) {
            if ((s.stages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0 && ContainsNoCase(s.name, w)) {
                out = s;
                return true;
            }
        }
    }
    return false;
}

// The driver's disassembly of the fragment stage, when it gives one among its text representations.
bool PickIsa(const std::vector<spike::InternalText>& texts, std::string& out) {
    const spike::InternalText* best = nullptr;
    for (const spike::InternalText& t : texts) {
        if ((t.stages & VK_SHADER_STAGE_FRAGMENT_BIT) == 0) {
            continue;
        }
        if (ContainsNoCase(t.name, "isa") || ContainsNoCase(t.name, "disassembl") ||
            ContainsNoCase(t.name, "assembly")) {
            if (best == nullptr || t.text.size() > best->text.size()) {
                best = &t;
            }
        }
    }
    if (best == nullptr) {
        return false;
    }
    out = best->text;
    return true;
}

// Instruction lines of a disassembly: not blank, not a label or a comment, starting with a mnemonic.
size_t CountIsa(const std::string& text) {
    size_t count = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        size_t b = pos;
        while (b < end && (text[b] == ' ' || text[b] == '\t' || text[b] == '\r')) {
            ++b;
        }
        size_t e = end;
        while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r')) {
            --e;
        }
        if (e > b && std::isalpha(static_cast<unsigned char>(text[b])) && text[e - 1] != ':') {
            ++count;
        }
        pos = end + 1;
    }
    return count;
}
struct Options {
    std::string device;
    uint32_t reps = 5;
    std::string salt;
    size_t keys = 0;
    std::string dump;
    bool force = false;
};

struct KeyResult {
    double create_1b = 0, create_uber = 0;
    double emit = 0, glslang = 0, module = 0, create_lit = 0;
    size_t spirv_lit_words = 0;
    double gpu_lit = 0, gpu_1b = 0, gpu_uber = 0;
    ImageCompare valid_1b, valid_uber;
    bool have_stats = false;
    double instr_lit = 0, instr_1b = 0, reg_lit = 0, reg_1b = 0;
    bool have_instr = false, have_reg = false;
    double isa_lit = 0, isa_1b = 0;
    double spread_lit = 0, spread_1b = 0; // (max - min) / median of the repetitions
    bool have_isa = false;
};

Options Parse(int argc, char** argv) {
    Options o;
    if (const char* env = std::getenv("MH4N_VULKAN_DEVICE")) {
        o.device = env;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value after " + a);
            }
            return argv[++i];
        };
        if (a == "--device") {
            o.device = value();
        } else if (a == "--reps") {
            o.reps = static_cast<uint32_t>(std::stoul(value()));
        } else if (a == "--salt") {
            o.salt = value();
        } else if (a == "--keys") {
            o.keys = static_cast<size_t>(std::stoul(value()));
        } else if (a == "--dump") {
            o.dump = value();
        } else if (a == "--force") {
            o.force = true;
        } else {
            throw std::runtime_error("usage: mh4n_spike_1b [--device TEXT] [--reps N] [--salt TEXT] [--keys N] "
                                     "[--dump DIR] [--force]");
        }
    }
    return o;
}

void Dump(const std::string& dir, const std::string& name, const std::string& text) {
    std::ofstream f(dir + "/" + name, std::ios::binary);
    f << text;
}

int Run(const Options& opt) {
    spike::Gpu gpu;
    gpu.Init(opt.device);
    const spike::DeviceInfo& di = gpu.info();

    std::vector<Key> keys = spike::MakeKeys();
    if (opt.keys != 0 && opt.keys < keys.size()) {
        keys.resize(opt.keys);
    }
    const size_t n = keys.size();

    std::printf("== spike_1b run\n");
    std::printf("device: %s (vendor 0x%04x, device 0x%04x, %s)\n", di.name.c_str(), di.vendor, di.device_id,
                di.discrete ? "discrete" : "not discrete");
    std::printf("driver: %s %s, Vulkan %s\n", di.driver_name.c_str(), di.driver_info.c_str(), di.api.c_str());
    std::printf("timestamp period: %.3f ns, VK_KHR_pipeline_executable_properties: %s\n", di.timestamp_period_ns,
                di.executable_properties ? "exposed" : "not exposed");
    std::printf("salt: %s\n", opt.salt.empty() ? "(none)" : opt.salt.c_str());

    // ---- shaders that are compiled once, before any timing ----
    const double glslang_init_ms = spike::InitGlslang();
    std::string error;
    std::vector<uint32_t> vs_spirv, warm_spirv, uber_spirv;
    if (!spike::CompileGlsl(false, spike::VertexSource(), vs_spirv, error)) {
        std::fprintf(stderr, "vertex shader: %s\n", error.c_str());
        return 1;
    }
    if (!spike::CompileGlsl(true, spike::WarmupFragmentSource(), warm_spirv, error)) {
        std::fprintf(stderr, "warm-up shader: %s\n", error.c_str());
        return 1;
    }
    const std::string uber_source = spike::UberSource();
    double t0 = NowMs();
    if (!spike::CompileGlsl(true, uber_source, uber_spirv, error)) {
        std::fprintf(stderr, "ubershader: %s\n", error.c_str());
        return 1;
    }
    const double uber_compile_ms = NowMs() - t0;
    if (!opt.dump.empty()) {
        Dump(opt.dump, "uber.frag.glsl", uber_source);
    }
    vs_spirv = spike::SaltSpirv(vs_spirv, opt.salt);
    warm_spirv = spike::SaltSpirv(warm_spirv, opt.salt);
    uber_spirv = spike::SaltSpirv(uber_spirv, opt.salt);
    std::printf("glslang: first-call initialisation %.2f ms (left out of every figure); ubershader compile %.2f ms "
                "(offline in the product)\n",
                glslang_init_ms, uber_compile_ms);
    std::printf("ubershader SPIR-V: %zu words, %u key fields as specialisation constants (ids 1..%u), spec_uber id 0\n",
                uber_spirv.size(), spike::kFieldCount, spike::kFieldCount);

    VkShaderModule vs = gpu.MakeModule(vs_spirv);
    VkShaderModule warm = gpu.MakeModule(warm_spirv);
    VkShaderModule uber = gpu.MakeModule(uber_spirv);

    std::vector<std::vector<uint32_t>> cfg;
    for (const Key& k : keys) {
        std::vector<uint32_t> words(spike::kBlockVec4s * 4, 0);
        for (uint32_t f = 0; f < spike::kFieldCount; ++f) {
            words[f] = k.w[f];
        }
        cfg.push_back(std::move(words));
    }
    gpu.MakeResources(MakeFragUniforms(), MakeLut(), cfg);

    // Warm-up: the driver's first pipeline creation and the GPU's clocks are not part of any key.
    VkPipeline p_warm = VK_NULL_HANDLE;
    {
        double ms = 0.0;
        p_warm = gpu.MakePipeline(vs, warm, nullptr, 0, &ms);
        for (int i = 0; i < 40; ++i) {
            gpu.TimeDraw(p_warm, gpu.KeySet(0));
        }
        std::printf("warm-up pipeline created in %.2f ms (left out)\n", ms);
    }

    std::vector<KeyResult> res(n);
    std::vector<VkPipeline> p_1b(n, VK_NULL_HANDLE), p_lit(n, VK_NULL_HANDLE), p_uber(n, VK_NULL_HANDLE);
    std::vector<VkShaderModule> m_lit(n, VK_NULL_HANDLE);
    std::vector<std::string> lit_sources(n);

    // ---- 1. miss latency ----
    for (size_t k = 0; k < n; ++k) {
        const spike::SpecConstants spec1b = MakeSpec(0, &keys[k]);
        auto do_1b = [&] { p_1b[k] = gpu.MakePipeline(vs, uber, &spec1b, 0, &res[k].create_1b); };
        auto do_lit = [&] {
            const double e0 = NowMs();
            lit_sources[k] = spike::LiteralSource(keys[k]);
            res[k].emit = NowMs() - e0;
            std::vector<uint32_t> spirv;
            const double g0 = NowMs();
            const bool ok = spike::CompileGlsl(true, lit_sources[k], spirv, error);
            res[k].glslang = NowMs() - g0;
            if (!ok) {
                throw std::runtime_error("literal shader of " + keys[k].name + ": " + error);
            }
            spirv = spike::SaltSpirv(spirv, opt.salt);
            res[k].spirv_lit_words = spirv.size();
            const double m0 = NowMs();
            m_lit[k] = gpu.MakeModule(spirv);
            res[k].module = NowMs() - m0;
            p_lit[k] = gpu.MakePipeline(vs, m_lit[k], nullptr, 0, &res[k].create_lit);
        };
        if (k % 2 == 0) {
            do_1b();
            do_lit();
        } else {
            do_lit();
            do_1b();
        }
        const spike::SpecConstants spec_uber = MakeSpec(1, nullptr);
        p_uber[k] = gpu.MakePipeline(vs, uber, &spec_uber, 0, &res[k].create_uber);
        if (!opt.dump.empty()) {
            Dump(opt.dump, keys[k].name + ".literal.frag.glsl", lit_sources[k]);
        }
    }

    // ---- 3. validity ----
    bool all_valid = true;
    std::vector<std::vector<uint8_t>> lit_images(n);
    for (size_t k = 0; k < n; ++k) {
        const std::vector<uint8_t> a = gpu.Render(p_lit[k], gpu.KeySet(k));
        lit_images[k] = a;
        const std::vector<uint8_t> b = gpu.Render(p_1b[k], gpu.KeySet(k));
        const std::vector<uint8_t> c = gpu.Render(p_uber[k], gpu.KeySet(k));
        res[k].valid_1b = Compare(a, b);
        res[k].valid_uber = Compare(a, c);
        const ImageCompare& v = res[k].valid_1b;
        const bool ok = v.psnr >= 40.0 && v.max_block_mean <= 8.0 && v.distinct >= 64;
        if (!ok) {
            all_valid = false;
        }
    }

    if (!all_valid && !opt.force) {
        std::printf("\n-- validity: DEFECT IN THE HARNESS, nothing is measured (--force measures anyway)\n");
        for (size_t k = 0; k < n; ++k) {
            const ImageCompare& v = res[k].valid_1b;
            std::printf("%s max diff %d psnr %.2f max block mean %.3f distinct %u\n", keys[k].name.c_str(),
                        v.max_diff, v.psnr, v.max_block_mean, v.distinct);
        }
        return 3;
    }

    // ---- 2a. GPU time ----
    for (size_t k = 0; k < n; ++k) {
        VkPipeline ps[3] = {p_lit[k], p_1b[k], p_uber[k]};
        for (VkPipeline p : ps) {
            for (int w = 0; w < 6; ++w) {
                gpu.TimeDraw(p, gpu.KeySet(k)); // untimed: the first draws of a pipeline and the GPU's clocks
            }
        }
        std::vector<double> samples[3];
        for (uint32_t r = 0; r < opt.reps; ++r) {
            for (uint32_t j = 0; j < 3; ++j) {
                const uint32_t which = (j + r) % 3; // rotate the order so none is always first
                samples[which].push_back(gpu.TimeDraw(ps[which], gpu.KeySet(k)));
            }
        }
        for (int j = 0; j < 2; ++j) {
            const double mid = Median(samples[j]);
            const double spread = (*std::max_element(samples[j].begin(), samples[j].end()) -
                                   *std::min_element(samples[j].begin(), samples[j].end())) / mid;
            (j == 0 ? res[k].spread_lit : res[k].spread_1b) = spread;
        }
        res[k].gpu_lit = Median(samples[0]) / 1.0e6;
        res[k].gpu_1b = Median(samples[1]) / 1.0e6;
        res[k].gpu_uber = Median(samples[2]) / 1.0e6;
    }
    // The floor of this target: a shader that only writes the output (bandwidth and the fixed work of 20 triangles).
    double floor_ms = 0.0;
    {
        gpu.TimeDraw(p_warm, gpu.KeySet(0));
        std::vector<double> f;
        for (uint32_t r = 0; r < std::max<uint32_t>(opt.reps, 5); ++r) {
            f.push_back(gpu.TimeDraw(p_warm, gpu.KeySet(0)));
        }
        floor_ms = Median(f) / 1.0e6;
    }

    // ---- 2b. executable statistics (pipelines created again with the capture flag) ----
    std::vector<spike::ExecutableStat> first_lit_stats, first_1b_stats;
    if (di.executable_properties) {
        for (size_t k = 0; k < n; ++k) {
            const spike::SpecConstants spec1b = MakeSpec(0, &keys[k]);
            const VkPipelineCreateFlags capture = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
                                                  VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
            VkPipeline a = gpu.MakePipeline(vs, m_lit[k], nullptr, capture, nullptr);
            VkPipeline b = gpu.MakePipeline(vs, uber, &spec1b, capture, nullptr);
            const auto sa = gpu.Statistics(a);
            const auto sb = gpu.Statistics(b);
            {
                std::string ta, tb;
                const bool got_a = PickIsa(gpu.InternalTexts(a), ta);
                const bool got_b = PickIsa(gpu.InternalTexts(b), tb);
                if (got_a && got_b) {
                    res[k].have_isa = true;
                    res[k].isa_lit = static_cast<double>(CountIsa(ta));
                    res[k].isa_1b = static_cast<double>(CountIsa(tb));
                    if (!opt.dump.empty()) {
                        Dump(opt.dump, keys[k].name + ".literal.isa.txt", ta);
                        Dump(opt.dump, keys[k].name + ".1b.isa.txt", tb);
                    }
                }
            }
            if (k == 0) {
                first_lit_stats = sa;
                first_1b_stats = sb;
            }
            spike::ExecutableStat x, y;
            res[k].have_stats = !sa.empty() && !sb.empty();
            if (FindStat(sa, {"instruction", "instr", "code size"}, x) &&
                FindStat(sb, {"instruction", "instr", "code size"}, y)) {
                res[k].have_instr = true;
                res[k].instr_lit = x.value;
                res[k].instr_1b = y.value;
            }
            if (FindStat(sa, {"vgpr", "vector reg", "register"}, x) &&
                FindStat(sb, {"vgpr", "vector reg", "register"}, y)) {
                res[k].have_reg = true;
                res[k].reg_lit = x.value;
                res[k].reg_1b = y.value;
            }
            gpu.Destroy(a);
            gpu.Destroy(b);
        }
    }

    // ---- report ----
    std::printf("\n-- keys\n| key | description | TEV stages | lights |\n|---|---|---|---|\n");
    for (const Key& k : keys) {
        std::printf("| %s | %s | %u | %u |\n", k.name.c_str(), k.description.c_str(), k.tev_stages, k.lights);
    }

    std::printf("\n-- miss latency (ms), no pipeline cache\n");
    std::printf("| key | 1b create | opt1 glslang | opt1 module | opt1 create | opt1 total | 1b / opt1 | "
                "uber create | emit (not in total) | literal SPIR-V words |\n|---|---|---|---|---|---|---|---|---|---|\n");
    std::vector<double> v1b, vopt1;
    for (size_t k = 0; k < n; ++k) {
        const KeyResult& r = res[k];
        const double total = r.glslang + r.module + r.create_lit;
        v1b.push_back(r.create_1b);
        vopt1.push_back(total);
        std::printf("| %s | %.2f | %.2f | %.3f | %.2f | %.2f | %.2f | %.2f | %.3f | %zu |\n", keys[k].name.c_str(),
                    r.create_1b, r.glslang, r.module, r.create_lit, total, r.create_1b / total, r.create_uber, r.emit,
                    r.spirv_lit_words);
    }

    std::printf("\n-- code quality: GPU time of %u full-screen triangles into %ux%u RGBA8 + D32 (median of %u, ms); "
                "floor (a shader that only writes the output): %.3f ms\n",
                spike::kTriangles, spike::kBigWidth, spike::kBigHeight, opt.reps, floor_ms);
    std::printf("| key | literal | 1b specialised | 1b / literal | ubershader | uber / literal |\n|---|---|---|---|---|---|\n");
    std::vector<double> ratio, uber_ratio;
    for (size_t k = 0; k < n; ++k) {
        const KeyResult& r = res[k];
        ratio.push_back(r.gpu_1b / r.gpu_lit);
        uber_ratio.push_back(r.gpu_uber / r.gpu_lit);
        std::printf("| %s | %.3f | %.3f | %.3f | %.3f | %.2f |\n", keys[k].name.c_str(), r.gpu_lit, r.gpu_1b,
                    ratio.back(), r.gpu_uber, uber_ratio.back());
    }

    std::vector<double> instr_ratio, reg_ratio, isa_ratio;
    if (di.executable_properties) {
        std::printf("\n-- VK_KHR_pipeline_executable_properties, fragment stage, as the driver names them (key k00)\n");
        std::printf("| executable | statistic | literal | 1b specialised |\n|---|---|---|---|\n");
        for (const spike::ExecutableStat& s : first_lit_stats) {
            if ((s.stages & VK_SHADER_STAGE_FRAGMENT_BIT) == 0) {
                continue;
            }
            double other = -1.0;
            for (const spike::ExecutableStat& t : first_1b_stats) {
                if (t.executable == s.executable && t.name == s.name) {
                    other = t.value;
                }
            }
            std::printf("| %s | %s | %.0f | %.0f |\n", s.executable.c_str(), s.name.c_str(), s.value, other);
        }
        std::printf("\n| key | instruction statistic literal | 1b | ratio | register (VGPR) literal | 1b | ratio | "
                    "ISA lines literal | 1b | ratio |\n|---|---|---|---|---|---|---|---|---|---|\n");
        for (size_t k = 0; k < n; ++k) {
            const KeyResult& r = res[k];
            if (r.have_instr) {
                instr_ratio.push_back(r.instr_1b / r.instr_lit);
            }
            if (r.have_reg) {
                reg_ratio.push_back(r.reg_1b / r.reg_lit);
            }
            if (r.have_isa && r.isa_lit > 0) {
                isa_ratio.push_back(r.isa_1b / r.isa_lit);
            }
            std::printf("| %s | %s | %s | %s | %.0f | %.0f | %.3f | %.0f | %.0f | %.3f |\n", keys[k].name.c_str(),
                        r.have_instr ? std::to_string(static_cast<long long>(r.instr_lit)).c_str() : "not exposed",
                        r.have_instr ? std::to_string(static_cast<long long>(r.instr_1b)).c_str() : "not exposed",
                        r.have_instr ? std::to_string(r.instr_1b / r.instr_lit).c_str() : "-", r.reg_lit, r.reg_1b,
                        r.have_reg ? r.reg_1b / r.reg_lit : 0.0, r.isa_lit, r.isa_1b,
                        (r.have_isa && r.isa_lit > 0) ? r.isa_1b / r.isa_lit : 0.0);
        }
    } else {
        std::printf("\n-- VK_KHR_pipeline_executable_properties: not exposed\n");
    }

    std::printf("\n-- validity: 1b specialised against literal, %ux%u RGBA8 (thresholds: PSNR >= 40 dB, 16x16 block "
                "mean error <= 8/255)\n",
                spike::kSmallSize, spike::kSmallSize);
    std::printf("| key | bit-equal | max diff | PSNR dB | max block mean | distinct colours | mean R G B (0..255) | "
                "ubershader: max diff |\n|---|---|---|---|---|---|---|---|\n");
    size_t bit_equal = 0;
    for (size_t k = 0; k < n; ++k) {
        const ImageCompare& v = res[k].valid_1b;
        bit_equal += v.bit_equal ? 1 : 0;
        std::printf("| %s | %s | %d | %.2f | %.3f | %u | %.1f %.1f %.1f | %d |\n", keys[k].name.c_str(),
                    v.bit_equal ? "yes" : "no", v.max_diff, v.psnr, v.max_block_mean, v.distinct, v.mean[0],
                    v.mean[1], v.mean[2], res[k].valid_uber.max_diff);
    }

    // Is the check able to see a difference? The images of two neighbouring keys are different programs.
    size_t neighbours_differ = 0;
    for (size_t k = 0; k + 1 < n; ++k) {
        neighbours_differ += (lit_images[k] != lit_images[k + 1]) ? 1 : 0;
    }

    const double med_ratio = Median(ratio);
    const double max_ratio = Max(ratio);
    const bool t1_gpu = med_ratio <= 1.10 && max_ratio <= 1.25;
    bool t1_stats = true;
    if (!instr_ratio.empty() && Median(instr_ratio) > 1.10) t1_stats = false;
    if (!reg_ratio.empty() && Median(reg_ratio) > 1.10) t1_stats = false;
    const bool t1 = t1_gpu && t1_stats;
    const double med_1b = Median(v1b), med_o1 = Median(vopt1), p95_1b = P95(v1b), p95_o1 = P95(vopt1);
    const bool t2 = med_1b <= med_o1 && p95_1b <= p95_o1;

    std::printf("\n-- summary\n");
    std::printf("keys %zu, valid %s, bit-equal %zu of %zu; sensitivity: the images of %zu of %zu neighbouring key "
                "pairs differ\n",
                n, all_valid ? "all" : "NOT ALL", bit_equal, n, neighbours_differ, n > 0 ? n - 1 : 0);
    std::printf("T1 GPU time: median 1b/literal %.3f (<= 1.10: %s), max %.3f (<= 1.25: %s)\n", med_ratio,
                med_ratio <= 1.10 ? "yes" : "NO", max_ratio, max_ratio <= 1.25 ? "yes" : "NO");
    if (!instr_ratio.empty()) {
        std::printf("T1 instructions: median ratio %.3f (<= 1.10: %s)\n", Median(instr_ratio),
                    Median(instr_ratio) <= 1.10 ? "yes" : "NO");
    } else {
        std::printf("T1 instructions: not exposed as a statistic by this driver\n");
    }
    if (!isa_ratio.empty()) {
        std::printf("informative (not a threshold): ISA instruction lines of the driver's disassembly, median "
                    "1b/literal %.3f (min %.3f, max %.3f)\n",
                    Median(isa_ratio), *std::min_element(isa_ratio.begin(), isa_ratio.end()), Max(isa_ratio));
    }
    {
        std::vector<double> sl, sb;
        for (const KeyResult& r : res) {
            sl.push_back(r.spread_lit);
            sb.push_back(r.spread_1b);
        }
        std::printf("repetition noise, (max - min) / median over the keys: literal median %.1f%% (max %.1f%%), "
                    "1b median %.1f%% (max %.1f%%)\n",
                    100.0 * Median(sl), 100.0 * Max(sl), 100.0 * Median(sb), 100.0 * Max(sb));
    }
    std::printf("bandwidth floor of the target: %.3f ms; keys whose literal time is within 10%% of it: ", floor_ms);
    {
        size_t at_floor = 0;
        for (const KeyResult& r : res) {
            at_floor += (r.gpu_lit <= floor_ms * 1.10) ? 1 : 0;
        }
        std::printf("%zu of %zu\n", at_floor, n);
    }
    if (!reg_ratio.empty()) {
        std::printf("T1 registers: median ratio %.3f (<= 1.10: %s)\n", Median(reg_ratio),
                    Median(reg_ratio) <= 1.10 ? "yes" : "NO");
    } else {
        std::printf("T1 registers: not exposed\n");
    }
    std::printf("T2 miss latency: 1b median %.2f ms, p95 %.2f ms; option 1 (glslang + module + creation) median %.2f ms, "
                "p95 %.2f ms; median ok %s, p95 ok %s\n",
                med_1b, p95_1b, med_o1, p95_o1, med_1b <= med_o1 ? "yes" : "NO", p95_1b <= p95_o1 ? "yes" : "NO");
    std::printf("informative: ubershader / literal GPU-time ratio median %.2f (min %.2f, max %.2f); ubershader "
                "creation median %.2f ms (first %.2f ms)\n",
                Median(uber_ratio), *std::min_element(uber_ratio.begin(), uber_ratio.end()), Max(uber_ratio),
                [&] { std::vector<double> u; for (auto& r : res) u.push_back(r.create_uber); return Median(u); }(),
                res[0].create_uber);
    std::printf("verdict (this run): %s\n", (t1 && t2 && all_valid) ? "1b passes"
                                             : (all_valid ? (t1 ? "1b fails: T2" : (t2 ? "1b fails: T1" : "1b fails: T1 and T2"))
                                                          : "invalid keys"));

    for (size_t k = 0; k < n; ++k) {
        gpu.Destroy(p_1b[k]);
        gpu.Destroy(p_lit[k]);
        gpu.Destroy(p_uber[k]);
        gpu.Destroy(m_lit[k]);
    }
    gpu.Destroy(p_warm);
    gpu.Destroy(vs);
    gpu.Destroy(warm);
    gpu.Destroy(uber);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return Run(Parse(argc, argv));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mh4n_spike_1b: %s\n", e.what());
        return 1;
    }
}
