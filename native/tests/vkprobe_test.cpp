// mh4n_vkprobe (N2-24): the JSON reader, the judgement of a device's facts, the table and the JSON
// report, and the command line. Every input is synthetic: the files below are written by this test in the
// layout of `vulkaninfo --json` as the reader assumes it (see native/src/vkprobe/input.cpp), they are not
// the output of a real device.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "gpu/backend/rhi.h"
#include "vkprobe/json.h"
#include "vkprobe/live.h"
#include "vkprobe/report.h"
#include "vkprobe/rhi_report.h"
#include "vkprobe/tool.h"

using namespace mh4n::vkprobe;

namespace {

// ---- synthetic vulkaninfo documents -----------------------------------------------------------

struct Sample {
    std::string device_name = "Synthetic Full GPU";
    unsigned api_version = (1u << 22) | (3u << 12) | 280u;
    std::string device_type = "VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU";
    // extension name -> spec version
    std::vector<std::string> extensions;
    bool include_extensions = true;
    // member name -> value, written into "VkPhysicalDeviceFeatures" or the structure of the extension
    std::vector<std::pair<std::string, bool>> features;
    // format -> optimalTilingFeatures mask (a number) or flag names
    std::vector<std::pair<std::string, unsigned>> formats_numeric;
    std::vector<std::pair<std::string, std::vector<std::string>>> formats_named;
};

std::string Join(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        out += (i ? "," : "") + parts[i];
    }
    return out;
}

std::string Vulkaninfo(const Sample& s) {
    std::vector<std::string> features;
    for (const auto& [name, value] : s.features) {
        features.push_back("\"" + name + "\":" + (value ? "true" : "false"));
    }
    std::vector<std::string> exts;
    for (const std::string& e : s.extensions) {
        exts.push_back("\"" + e + "\":1");
    }
    std::vector<std::string> formats;
    for (const auto& [name, mask] : s.formats_numeric) {
        formats.push_back("\"VK_FORMAT_" + name + "\":{\"VkFormatProperties\":{\"linearTilingFeatures\":0,"
                          "\"optimalTilingFeatures\":" + std::to_string(mask) + ",\"bufferFeatures\":0}}");
    }
    for (const auto& [name, flags] : s.formats_named) {
        std::vector<std::string> quoted;
        for (const std::string& f : flags) {
            quoted.push_back("\"" + f + "\"");
        }
        formats.push_back("\"VK_FORMAT_" + name + "\":{\"VkFormatProperties\":{\"linearTilingFeatures\":[],"
                          "\"optimalTilingFeatures\":[" + Join(quoted) + "],\"bufferFeatures\":[]}}");
    }
    std::string doc = "{\"$schema\":\"synthetic\",\"capabilities\":{\"device\":{";
    doc += "\"properties\":{\"VkPhysicalDeviceProperties\":{\"apiVersion\":" + std::to_string(s.api_version) +
           ",\"deviceName\":\"" + s.device_name + "\",\"deviceType\":\"" + s.device_type +
           "\",\"vendorID\":4318,\"deviceID\":9999,\"limits\":{\"maxImageDimension2D\":16384}},"
           "\"VkPhysicalDeviceDriverProperties\":{\"driverName\":\"synthetic driver\",\"driverInfo\":\"1.0\"}},";
    doc += "\"features\":{\"VkPhysicalDeviceFeatures\":{" + Join(features) + "}},";
    if (s.include_extensions) {
        doc += "\"extensions\":{" + Join(exts) + "},";
    }
    doc += "\"formats\":{" + Join(formats) + "}}}}";
    return doc;
}

// Everything on, as a desktop GPU that has it all would report.
Sample FullDevice() {
    Sample s;
    s.extensions = {"VK_KHR_swapchain",
                    "VK_EXT_fragment_shader_interlock",
                    "VK_KHR_fragment_shader_barycentric",
                    "VK_EXT_index_type_uint8",
                    "VK_EXT_custom_border_color",
                    "VK_EXT_shader_stencil_export",
                    "VK_EXT_rasterization_order_attachment_access",
                    "VK_EXT_graphics_pipeline_library",
                    "VK_KHR_push_descriptor",
                    "VK_KHR_dynamic_rendering",
                    "VK_KHR_timeline_semaphore",
                    "VK_EXT_extended_dynamic_state",
                    "VK_EXT_extended_dynamic_state2",
                    "VK_EXT_extended_dynamic_state3",
                    "VK_KHR_present_id",
                    "VK_KHR_present_wait"};
    s.features = {{"logicOp", true},
                  {"geometryShader", true},
                  {"fragmentStoresAndAtomics", true},
                  {"shaderClipDistance", true},
                  {"textureCompressionBC", true},
                  {"fragmentShaderPixelInterlock", true},
                  {"fragmentShaderSampleInterlock", true},
                  {"fragmentShaderBarycentric", true},
                  {"indexTypeUint8", true},
                  {"customBorderColors", true},
                  {"rasterizationOrderColorAttachmentAccess", true},
                  {"graphicsPipelineLibrary", true},
                  {"dynamicRendering", true},
                  {"timelineSemaphore", true},
                  {"extendedDynamicState", true},
                  {"extendedDynamicState2", true},
                  {"extendedDynamicState3PolygonMode", true},
                  {"presentId", true},
                  {"presentWait", true}};
    s.formats_numeric = {{"D24_UNORM_S8_UINT", 0x200 | 0x1}, {"D32_SFLOAT_S8_UINT", 0x200 | 0x1}, {"BC7_UNORM_BLOCK", 0x1 | 0x1000}};
    return s;
}

std::vector<DeviceReport> MustParse(const std::string& text) {
    std::vector<DeviceReport> reports;
    std::string error;
    REQUIRE_MESSAGE(ParseReports(text, &reports, &error), error);
    return reports;
}

std::string Status(const DeviceReport& r, const char* id) {
    const FeatureResult* f = r.Find(id);
    REQUIRE_MESSAGE(f != nullptr, id);
    return StatusText(*f);
}

// A file in the temp folder with a name no other run or test shares, removed when it goes out of scope
// (also when a REQUIRE throws).
class TempFile {
public:
    TempFile(const std::string& name, const std::string& content) {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mh4n_vkprobe_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++) + "_" + name);
        std::ofstream(path_, std::ios::binary) << content;
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

int Run(const std::vector<std::string>& args, std::string* out, std::string* err, size_t limit = kMaxInputBytes) {
    std::ostringstream o;
    std::ostringstream e;
    const int rc = RunVkProbe(args, o, e, limit);
    *out = o.str();
    *err = e.str();
    return rc;
}

} // namespace

TEST_CASE("a device with everything: every line is yes") {
    const auto reports = MustParse(Vulkaninfo(FullDevice()));
    REQUIRE(reports.size() == 1);
    const DeviceReport& r = reports[0];
    CHECK(r.device_name == "Synthetic Full GPU");
    CHECK(r.device_type == "discrete GPU");
    CHECK(r.api_version == "1.3.280");
    CHECK(r.driver == "synthetic driver 1.0");
    CHECK(r.features.size() == 24);
    for (const FeatureResult& f : r.features) {
        INFO(f.id << ": " << StatusText(f) << " [" << f.detail << "]");
        CHECK(f.status == Tri::Yes);
    }
    CHECK(Status(r, "triangleFans") == "yes"); // no portability subset: fans are core
}

TEST_CASE("a portability-subset device: no triangle fans, no logic ops, no D24, missing extensions") {
    Sample s = FullDevice();
    s.device_name = "Synthetic Portability GPU";
    s.device_type = "VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU";
    s.extensions.push_back("VK_KHR_portability_subset");
    // no custom border colours, no index type uint8 on this one
    s.extensions.erase(std::remove(s.extensions.begin(), s.extensions.end(), "VK_EXT_custom_border_color"), s.extensions.end());
    s.extensions.erase(std::remove(s.extensions.begin(), s.extensions.end(), "VK_EXT_index_type_uint8"), s.extensions.end());
    s.features.erase(std::remove_if(s.features.begin(), s.features.end(),
                                    [](const auto& f) { return f.first == "customBorderColors" || f.first == "indexTypeUint8"; }),
                     s.features.end());
    for (auto& f : s.features) {
        if (f.first == "logicOp" || f.first == "geometryShader") {
            f.second = false;
        }
    }
    s.features.push_back({"triangleFans", false});
    s.formats_numeric = {{"D24_UNORM_S8_UINT", 0}, {"D32_SFLOAT_S8_UINT", 0x200 | 0x1}, {"BC7_UNORM_BLOCK", 0x1}};

    const auto reports = MustParse(Vulkaninfo(s));
    REQUIRE(reports.size() == 1);
    const DeviceReport& r = reports[0];
    CHECK(r.device_type == "integrated GPU");
    CHECK(Status(r, "triangleFans") == "no (fallback: rewrite fans as triangle lists on the CPU)");
    CHECK(Status(r, "logicOp").rfind("no (fallback: ", 0) == 0);
    CHECK(Status(r, "geometryShader").rfind("no (fallback: ", 0) == 0);
    // the missing extension is a no, with the fallback
    CHECK(Status(r, "customBorderColors") == "no (fallback: emulate the border colour in the shader)");
    CHECK(Status(r, "indexTypeUint8") == "no (fallback: widen the indices to 16 bits on the CPU)");
    CHECK(r.Find("customBorderColors")->detail == "VK_EXT_custom_border_color is not on the device");
    // D24 is missing and D32S8 is the fallback
    CHECK(Status(r, "D24_UNORM_S8_UINT") == "no (fallback: D32_SFLOAT_S8_UINT)");
    CHECK(r.Find("D24_UNORM_S8_UINT")->detail == "D32_SFLOAT_S8_UINT is available");
    CHECK(Status(r, "D32_SFLOAT_S8_UINT") == "yes");
    // what the device has stays yes
    CHECK(Status(r, "shaderClipDistance") == "yes");
    CHECK(Status(r, "BC7") == "yes");
    CHECK(Status(r, "fragmentShaderPixelInterlock") == "yes");
}

TEST_CASE("a portability-subset device that does not report triangleFans is unknown, not no") {
    Sample s = FullDevice();
    s.extensions.push_back("VK_KHR_portability_subset");
    const auto r = MustParse(Vulkaninfo(s))[0];
    CHECK(Status(r, "triangleFans") == "unknown");
}

TEST_CASE("absent is unknown, never no: the extension is listed, its feature structure is not") {
    Sample s = FullDevice();
    s.features.erase(std::remove_if(s.features.begin(), s.features.end(),
                                    [](const auto& f) { return f.first == "graphicsPipelineLibrary" || f.first == "customBorderColors"; }),
                     s.features.end());
    s.formats_numeric.clear(); // no format entries at all
    s.formats_named.clear();
    const auto r = MustParse(Vulkaninfo(s))[0];
    CHECK(Status(r, "graphicsPipelineLibrary") == "unknown");
    CHECK(Status(r, "customBorderColors") == "unknown");
    CHECK(Status(r, "D24_UNORM_S8_UINT") == "unknown");
    // BC7 falls back on textureCompressionBC, which is reported
    CHECK(Status(r, "BC7") == "yes");
    CHECK(r.Find("BC7")->detail == "from textureCompressionBC");
    // the rest is still judged
    CHECK(Status(r, "logicOp") == "yes");
}

TEST_CASE("no extension list in the file: extension features are unknown, core features are judged") {
    Sample s = FullDevice();
    s.include_extensions = false;
    const auto r = MustParse(Vulkaninfo(s))[0];
    CHECK(Status(r, "fragmentShaderPixelInterlock") == "unknown");
    CHECK(Status(r, "pushDescriptor") == "unknown");
    CHECK(Status(r, "shaderStencilExport") == "unknown");
    CHECK(Status(r, "triangleFans") == "unknown");
    CHECK(Status(r, "logicOp") == "yes");
    CHECK(Status(r, "geometryShader") == "yes");
    CHECK(Status(r, "timelineSemaphore") == "yes");
}

TEST_CASE("a feature bit alone is not a yes when the extension is missing") {
    Sample s = FullDevice();
    s.extensions.erase(std::remove(s.extensions.begin(), s.extensions.end(), "VK_EXT_fragment_shader_interlock"), s.extensions.end());
    const auto r = MustParse(Vulkaninfo(s))[0];
    CHECK(Status(r, "fragmentShaderPixelInterlock").rfind("no (fallback: ", 0) == 0);
}

TEST_CASE("older devices: Vulkan 1.1 without the extensions of 1.2 and 1.3") {
    Sample s = FullDevice();
    s.api_version = (1u << 22) | (1u << 12) | 100u;
    for (const char* name : {"VK_KHR_dynamic_rendering", "VK_KHR_timeline_semaphore", "VK_EXT_extended_dynamic_state",
                             "VK_EXT_extended_dynamic_state2", "VK_KHR_push_descriptor"}) {
        s.extensions.erase(std::remove(s.extensions.begin(), s.extensions.end(), name), s.extensions.end());
    }
    s.features.erase(std::remove_if(s.features.begin(), s.features.end(),
                                    [](const auto& f) {
                                        return f.first == "dynamicRendering" || f.first == "timelineSemaphore" ||
                                               f.first == "extendedDynamicState" || f.first == "extendedDynamicState2";
                                    }),
                     s.features.end());
    const auto r = MustParse(Vulkaninfo(s))[0];
    CHECK(Status(r, "dynamicRendering") == "no (fallback: render passes and framebuffers)");
    CHECK(Status(r, "timelineSemaphore") == "no (fallback: binary semaphores and fences)");
    CHECK(Status(r, "pushDescriptor").rfind("no (fallback: ", 0) == 0);
    CHECK(Status(r, "extendedDynamicState").rfind("no (fallback: ", 0) == 0);
}

TEST_CASE("Vulkan 1.3 is extended dynamic state 1 and 2 even when the extension is not listed") {
    Sample s = FullDevice();
    s.extensions.erase(std::remove(s.extensions.begin(), s.extensions.end(), "VK_EXT_extended_dynamic_state"), s.extensions.end());
    s.extensions.erase(std::remove(s.extensions.begin(), s.extensions.end(), "VK_EXT_extended_dynamic_state2"), s.extensions.end());
    const auto r = MustParse(Vulkaninfo(s))[0];
    CHECK(Status(r, "extendedDynamicState") == "yes");
    CHECK(r.Find("extendedDynamicState")->detail == "core in Vulkan 1.3");
    CHECK(Status(r, "extendedDynamicState2") == "yes");
}

TEST_CASE("format features as flag names, enumerations as numbers, extensions as a list") {
    Sample s = FullDevice();
    s.device_type = "1"; // integrated, as a number below
    s.formats_numeric.clear();
    s.formats_named = {{"D24_UNORM_S8_UINT", {"VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT"}},
                       {"D32_SFLOAT_S8_UINT", {"VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT", "VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT"}},
                       {"BC7_UNORM_BLOCK", {"VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT"}}};
    std::string text = Vulkaninfo(s);
    // rewrite the device type as a number and the extensions as a list of names
    const std::string type_name = "\"deviceType\":\"1\"";
    text.replace(text.find(type_name), type_name.size(), "\"deviceType\":1");
    const auto r = MustParse(text)[0];
    CHECK(r.device_type == "integrated GPU");
    CHECK(Status(r, "D24_UNORM_S8_UINT") == "no (fallback: D32_SFLOAT_S8_UINT)"); // sampled only
    CHECK(Status(r, "D32_SFLOAT_S8_UINT") == "yes");
    CHECK(Status(r, "BC7") == "no (fallback: decode to RGBA8 at load (four times the memory))"); // filter bit is not the sampled bit

    const std::string list_form =
        R"({"capabilities":{"device":{"extensions":["VK_KHR_push_descriptor",{"extensionName":"VK_EXT_shader_stencil_export"}],)"
        R"("features":{"A":{"geometryShader":false}}}}})";
    const auto l = MustParse(list_form)[0];
    CHECK(Status(l, "pushDescriptor") == "yes");
    CHECK(Status(l, "shaderStencilExport") == "yes");
    CHECK(Status(l, "customBorderColors").rfind("no (fallback: ", 0) == 0);
    CHECK(Status(l, "geometryShader").rfind("no (fallback: ", 0) == 0);
    CHECK(Status(l, "logicOp") == "unknown");
}

TEST_CASE("a document with no device is an error") {
    std::vector<DeviceReport> reports;
    std::string error;
    CHECK_FALSE(ParseReports(R"({"hello": 1})", &reports, &error));
    CHECK(error.find("no device") != std::string::npos);
    CHECK_FALSE(ParseReports(R"([1, 2])", &reports, &error));
    CHECK(reports.empty());
}

TEST_CASE("the table has one line per feature with yes / no (fallback) / unknown") {
    Sample s = FullDevice();
    s.features.erase(std::remove_if(s.features.begin(), s.features.end(), [](const auto& f) { return f.first == "geometryShader"; }),
                     s.features.end());
    s.features.push_back({"geometryShader", false});
    s.extensions.erase(std::remove(s.extensions.begin(), s.extensions.end(), "VK_KHR_push_descriptor"), s.extensions.end());
    const auto r = MustParse(Vulkaninfo(s))[0];
    const std::string table = RenderTable(r);
    CHECK(table.rfind("device: Synthetic Full GPU\n", 0) == 0);
    CHECK(table.find("api version: 1.3.280") != std::string::npos);
    CHECK(table.find("vendor 0x10de, device 0x270f") != std::string::npos);
    CHECK(table.find("logic ops (logicOp)") != std::string::npos);
    const size_t geometry = table.find("    geometry shaders ");
    REQUIRE(geometry != std::string::npos);
    const std::string line = table.substr(geometry, table.find('\n', geometry) - geometry);
    CHECK(line.find("  no (fallback: skip the per-triangle quaternion fix-up, or run it in a compute pass)") != std::string::npos);
    CHECK(line.find("geometry shaders") == 4);
    size_t lines = 0;
    for (const char c : table) {
        lines += c == '\n';
    }
    CHECK(lines == 6 + r.features.size());
    CHECK(table.back() == '\n');
}

TEST_CASE("the JSON report reads back to the same table, several devices in one file") {
    Sample a = FullDevice();
    Sample b = FullDevice();
    b.extensions.push_back("VK_KHR_portability_subset");
    b.features.push_back({"triangleFans", false});
    const DeviceReport ra = MustParse(Vulkaninfo(a))[0];
    DeviceReport rb = MustParse(Vulkaninfo(b))[0];
    rb.device_name = "Synthetic \"Second\" GPU \xC3\xA9"; // quotes and non-ASCII survive the JSON
    const std::string json = RenderJson(ra) + RenderJson(rb);

    const auto back = MustParse(json);
    REQUIRE(back.size() == 2);
    CHECK(RenderTable(back[0]) == RenderTable(ra));
    CHECK(RenderTable(back[1]) == RenderTable(rb));
    CHECK(RenderJson(back[1]) == RenderJson(rb));
    CHECK(back[1].device_name == rb.device_name);
    CHECK(Status(back[1], "triangleFans").rfind("no (fallback: ", 0) == 0);
}

TEST_CASE("truncated or invalid files are rejected with a clear error") {
    const std::string whole = Vulkaninfo(FullDevice());
    std::vector<DeviceReport> reports;
    std::string error;
    for (const size_t cut : {whole.size() - 1, whole.size() / 2, size_t{10}, size_t{1}}) {
        INFO("cut at " << cut);
        error.clear();
        CHECK_FALSE(ParseReports(whole.substr(0, cut), &reports, &error));
        CHECK(reports.empty());
        CHECK_FALSE(error.empty());
    }
    CHECK_FALSE(ParseReports(whole.substr(0, whole.size() / 2), &reports, &error));
    CHECK(error.find("truncated") != std::string::npos);
    CHECK(error.find("line 1") != std::string::npos);

    CHECK_FALSE(ParseReports("", &reports, &error));
    CHECK(error.find("empty") != std::string::npos);
    CHECK_FALSE(ParseReports("   \n ", &reports, &error));
    CHECK_FALSE(ParseReports("{\"a\": tru}", &reports, &error));
    CHECK_FALSE(ParseReports("{\"a\": 1,}", &reports, &error));
    CHECK_FALSE(ParseReports("{\"a\" 1}", &reports, &error));
    CHECK_FALSE(ParseReports("{\"a\": 01}", &reports, &error));
    CHECK_FALSE(ParseReports("{\"a\": \"\\q\"}", &reports, &error));
    CHECK_FALSE(ParseReports(whole + " garbage", &reports, &error));
}

TEST_CASE("a deep nesting is an error, not a crash") {
    std::vector<Json> docs;
    std::string error;
    const std::string deep(100000, '[');
    CHECK_FALSE(ParseJsonDocuments(deep, &docs, &error));
    CHECK(error.find("nesting") != std::string::npos);
    std::string objects;
    for (int i = 0; i < 100000; ++i) {
        objects += "{\"a\":";
    }
    CHECK_FALSE(ParseJsonDocuments(objects, &docs, &error));
    CHECK(error.find("nesting") != std::string::npos);
    // at the limit it parses
    std::string ok(kJsonMaxDepth, '[');
    ok += std::string(kJsonMaxDepth, ']');
    CHECK(ParseJsonDocuments(ok, &docs, &error));
    std::string over(kJsonMaxDepth + 1, '[');
    over += std::string(kJsonMaxDepth + 1, ']');
    CHECK_FALSE(ParseJsonDocuments(over, &docs, &error));
}

TEST_CASE("the JSON reader: values, escapes, a byte order mark, several documents") {
    std::vector<Json> docs;
    std::string error;
    REQUIRE_MESSAGE(ParseJsonDocuments("\xEF\xBB\xBF{\"s\":\"a\\n\\u00e9\\ud83d\\ude00\",\"n\":-1.5e2,\"t\":true,\"z\":null,\"l\":[1,[]]} [2]", &docs, &error), error);
    REQUIRE(docs.size() == 2);
    CHECK(docs[0].Find("s")->string == "a\n\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(docs[0].Find("n")->number == -150.0);
    CHECK(docs[0].Find("t")->boolean);
    CHECK(docs[0].Find("z")->type == Json::Type::Null);
    CHECK(docs[0].Find("l")->array.size() == 2);
    CHECK(docs[0].Find("missing") == nullptr);
    CHECK(docs[1].IsArray());
    CHECK(JsonQuote("a\"b\\\n\x01") == "\"a\\\"b\\\\\\n\\u0001\"");
}

TEST_CASE("the live probe without a loader says so") {
    std::vector<Facts> devices;
    std::string error;
    CHECK_FALSE(ProbeLive({"mh4n-no-such-vulkan-loader.so.1"}, &devices, &error));
    CHECK(error.find("no Vulkan loader found") != std::string::npos);
    CHECK(error.find("mh4n-no-such-vulkan-loader") != std::string::npos);
    CHECK(devices.empty());
}

TEST_CASE("the live probe, when this machine has a Vulkan device") {
    std::vector<Facts> devices;
    std::string error;
    if (!ProbeLive(DefaultLoaderNames(), &devices, &error)) {
        MESSAGE("no Vulkan loader or device here (" << error << "): the live probe is not exercised");
        return;
    }
    REQUIRE_FALSE(devices.empty());
    for (const Facts& facts : devices) {
        CHECK_FALSE(facts.device_name.empty());
        CHECK(facts.api_version != 0);
        CHECK(facts.extensions_known);
        const DeviceReport r = Evaluate(facts, "live");
        CHECK(r.features.size() == 24);
        // a live device reports every core bit, so none of these is unknown
        for (const char* id : {"logicOp", "geometryShader", "shaderClipDistance", "fragmentStoresAndAtomics",
                               "D24_UNORM_S8_UINT", "D32_SFLOAT_S8_UINT", "BC7", "shaderStencilExport", "pushDescriptor"}) {
            CHECK_MESSAGE(r.Find(id)->status != Tri::Unknown, id);
        }
        // the report survives its own JSON
        const auto back = MustParse(RenderJson(r));
        CHECK(RenderTable(back[0]) == RenderTable(r));
    }
}

TEST_CASE("the command line") {
    std::string out;
    std::string err;
    CHECK(Run({"--help"}, &out, &err) == 0);
    CHECK(out.rfind("usage: mh4n_vkprobe", 0) == 0);
    CHECK(out.find("--from-json") != std::string::npos);

    CHECK(Run({"--no-such-option"}, &out, &err) == 2);
    CHECK(Run({"stray"}, &out, &err) == 2);
    CHECK(Run({"--from-json", "/no/such/file.json"}, &out, &err) == 2);
    CHECK(err.find("cannot open") != std::string::npos);

    const TempFile good("good.json", Vulkaninfo(FullDevice()));
    REQUIRE(Run({"--from-json", good.str()}, &out, &err) == 0);
    CHECK(out.rfind("device: Synthetic Full GPU\n", 0) == 0);
    CHECK(out.find("yes") != std::string::npos);
    CHECK(out.find("unknown") == std::string::npos);

    // --json of a file is the report's own format, which reads back
    std::string json;
    REQUIRE(Run({"--from-json", good.str(), "--json"}, &json, &err) == 0);
    CHECK(json.find("\"mh4n_vkprobe\": 1") != std::string::npos);
    const TempFile own("own.json", json);
    std::string again;
    REQUIRE(Run({"--from-json", own.str()}, &again, &err) == 0);
    CHECK(again == out);

    // a truncated file: exit 2 and the file name and reason on stderr, nothing on stdout
    const std::string whole = Vulkaninfo(FullDevice());
    const TempFile cut("cut.json", whole.substr(0, whole.size() - 20));
    CHECK(Run({"--from-json", cut.str()}, &out, &err) == 2);
    CHECK(out.empty());
    CHECK(err.find("truncated") != std::string::npos);
    CHECK(err.find("cut.json") != std::string::npos);

    CHECK(Run({"--from-json", good.str(), "--loader", "x"}, &out, &err) == 2);

    // a loader that does not exist: exit 3, and --from-json is mentioned as the way out
    CHECK(Run({"--loader", "mh4n-no-such-vulkan-loader.so.1"}, &out, &err) == 3);
    CHECK(err.find("no Vulkan loader found") != std::string::npos);
    CHECK(err.find("--from-json") != std::string::npos);
}

TEST_CASE("--from-json refuses a folder, a missing file and an oversized file, with exit status 2") {
    std::string out;
    std::string err;
    CHECK(Run({"--from-json", std::filesystem::temp_directory_path().string()}, &out, &err) == 2);
    CHECK(err.find("is not a file") != std::string::npos);
    CHECK(Run({"--from-json", (std::filesystem::temp_directory_path() / "mh4n_vkprobe_test_missing_file.json").string()}, &out, &err) == 2);
    CHECK(err.find("cannot open") != std::string::npos);

    const TempFile big("big.json", Vulkaninfo(FullDevice()));
    CHECK(Run({"--from-json", big.str()}, &out, &err, 100) == 2);
    CHECK(err.find("the limit is 100 bytes") != std::string::npos);
    CHECK(out.empty());
    // the same file under the real limit reads
    CHECK(Run({"--from-json", big.str()}, &out, &err) == 0);

    // a UTF-16 file (what PowerShell 5.1's > writes) gets its own message
    const TempFile utf16("utf16.json", std::string("\xFF\xFE{\0\"\0a\0\"\0:\0" "1\0}\0", 14));
    CHECK(Run({"--from-json", utf16.str()}, &out, &err) == 2);
    CHECK(err.find("this file is UTF-16 (PowerShell's > does that); save it as UTF-8") != std::string::npos);
}

TEST_CASE("a flag mask that does not fit 64 bits is an error, not undefined behaviour") {
    for (const char* value : {"1e308", "18446744073709551616", "-1"}) {
        INFO(value);
        const std::string doc = std::string(R"({"capabilities":{"device":{"formats":{"VK_FORMAT_D24_UNORM_S8_UINT":)") +
                                R"({"VkFormatProperties":{"optimalTilingFeatures":)" + value + "}}}}}}";
        std::vector<DeviceReport> reports;
        std::string error;
        CHECK_FALSE(ParseReports(doc, &reports, &error));
        CHECK(error.find("not a valid flag mask") != std::string::npos);
        CHECK(error.find("VK_FORMAT_D24_UNORM_S8_UINT") != std::string::npos);
    }
    // the largest value that fits is fine
    std::vector<DeviceReport> reports;
    std::string error;
    CHECK(ParseReports(R"({"capabilities":{"device":{"formats":{"VK_FORMAT_D24_UNORM_S8_UINT":{"VkFormatProperties":{"optimalTilingFeatures":18446744073709549568}}}}}})", &reports, &error));
}

TEST_CASE("explicit triangleFans: false is no, whatever the extension list says") {
    Sample s = FullDevice(); // a full list without VK_KHR_portability_subset
    s.features.push_back({"triangleFans", false});
    const auto r = MustParse(Vulkaninfo(s))[0];
    CHECK(Status(r, "triangleFans") == "no (fallback: rewrite fans as triangle lists on the CPU)");
    CHECK(r.Find("triangleFans")->detail == "triangleFans is false");
    // and with no extension list at all
    s.include_extensions = false;
    CHECK(Status(MustParse(Vulkaninfo(s))[0], "triangleFans").rfind("no (fallback: ", 0) == 0);
}

TEST_CASE("formats that vulkaninfo left out stay unknown and say why") {
    Sample s = FullDevice();
    s.formats_numeric = {{"D32_SFLOAT_S8_UINT", 0x200 | 0x1}}; // a formats section without D24 and BC7
    s.features.erase(std::remove_if(s.features.begin(), s.features.end(), [](const auto& f) { return f.first == "textureCompressionBC"; }),
                     s.features.end());
    const auto r = MustParse(Vulkaninfo(s))[0];
    CHECK(Status(r, "D24_UNORM_S8_UINT") == "unknown");
    CHECK(r.Find("D24_UNORM_S8_UINT")->detail.find("vulkaninfo omits formats the device has no feature for, so this most likely means no; the live probe settles it") != std::string::npos);
    CHECK(Status(r, "BC7") == "unknown");
    CHECK(r.Find("BC7")->detail.find("most likely means no") != std::string::npos);
    CHECK(r.Find("BC7")->detail.find("textureCompressionBC") != std::string::npos);
    CHECK(Status(r, "D32_SFLOAT_S8_UINT") == "yes");
    // no formats section at all: the plain message
    const auto none = MustParse(R"({"capabilities":{"device":{"features":{"A":{"logicOp":true}}}}})")[0];
    CHECK(none.Find("D24_UNORM_S8_UINT")->detail == "the format was not reported");
}

TEST_CASE("VK_KHR_present_id and VK_KHR_present_wait, with and without") {
    const auto with = MustParse(Vulkaninfo(FullDevice()))[0];
    CHECK(Status(with, "presentId") == "yes");
    CHECK(Status(with, "presentWait") == "yes");
    CHECK(with.Find("presentId")->detail.find("a swapchain's support needs a surface") != std::string::npos);

    Sample s = FullDevice();
    s.extensions.erase(std::remove(s.extensions.begin(), s.extensions.end(), "VK_KHR_present_wait"), s.extensions.end());
    s.features.erase(std::remove_if(s.features.begin(), s.features.end(), [](const auto& f) { return f.first == "presentId"; }), s.features.end());
    const auto without = MustParse(Vulkaninfo(s))[0];
    CHECK(Status(without, "presentWait").rfind("no (fallback: ", 0) == 0);
    CHECK(Status(without, "presentId") == "unknown"); // extension listed, bit not reported
    s.features.push_back({"presentId", false});
    CHECK(Status(MustParse(Vulkaninfo(s))[0], "presentId").rfind("no (fallback: ", 0) == 0);
    const std::string table = RenderTable(with);
    CHECK(table.find("VK_KHR_present_id (presentId)") != std::string::npos);
    CHECK(table.find("VK_KHR_present_wait (presentWait)") != std::string::npos);
}

TEST_CASE("error messages show non-printable bytes as 0x..") {
    std::vector<Json> docs;
    std::string error;
    CHECK_FALSE(ParseJsonDocuments("{\"a\": \x01}", &docs, &error));
    CHECK(error.find("unexpected byte 0x01") != std::string::npos);
    CHECK_FALSE(ParseJsonDocuments("{\"a\": \xC3\xA9}", &docs, &error));
    CHECK(error.find("unexpected byte 0xc3") != std::string::npos);
    CHECK_FALSE(ParseJsonDocuments("{\"a\": @}", &docs, &error));
    CHECK(error.find("unexpected '@'") != std::string::npos);
    CHECK_FALSE(ParseJsonDocuments("\"\\\xFF\"", &docs, &error));
    CHECK(error.find("0xff") != std::string::npos);
}

TEST_CASE("the probe's own report: every error branch") {
    std::vector<DeviceReport> reports;
    std::string error;
    CHECK_FALSE(ParseReports(R"({"mh4n_vkprobe": 2, "features": []})", &reports, &error));
    CHECK(error.find("unsupported report version") != std::string::npos);
    CHECK_FALSE(ParseReports(R"({"mh4n_vkprobe": "1", "features": []})", &reports, &error));
    CHECK(error.find("unsupported report version") != std::string::npos);
    CHECK_FALSE(ParseReports(R"({"mh4n_vkprobe": 1, "source": "live"})", &reports, &error));
    CHECK(error.find("no \"features\" list") != std::string::npos);
    CHECK_FALSE(ParseReports(R"({"mh4n_vkprobe": 1, "features": {}})", &reports, &error));
    CHECK(error.find("no \"features\" list") != std::string::npos);
    CHECK_FALSE(ParseReports(R"({"mh4n_vkprobe": 1, "features": [{"id": "x", "status": "maybe"}]})", &reports, &error));
    CHECK(error.find("status other than yes, no or unknown") != std::string::npos);
    CHECK_FALSE(ParseReports(R"({"mh4n_vkprobe": 1, "features": [{"status": "yes"}]})", &reports, &error));
    CHECK_FALSE(ParseReports(R"({"mh4n_vkprobe": 1, "features": [{"id": "x"}]})", &reports, &error));
    CHECK(reports.empty());
    // a minimal valid report: the title defaults to the id
    REQUIRE(ParseReports(R"({"mh4n_vkprobe": 1, "features": [{"id": "x", "status": "no"}]})", &reports, &error));
    CHECK(reports[0].features[0].title == "x");
    CHECK(StatusText(reports[0].features[0]) == "no");
    // a document that is not an object
    CHECK_FALSE(ParseReports(R"("text")", &reports, &error));
    CHECK(error.find("not a JSON object") != std::string::npos);
    // a second bad document after a good one rejects the whole file
    const std::string good = RenderJson(MustParse(Vulkaninfo(FullDevice()))[0]);
    CHECK_FALSE(ParseReports(good + R"({"mh4n_vkprobe": 9})", &reports, &error));
    CHECK(reports.empty());
}

// ---- --rhi (N2-30) ----------------------------------------------------------------------------------

namespace {

namespace rhi = mh4n::gpu::rhi;

// Sets an environment variable for one scope (null: unset) and restores the old value.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_ = true;
            old_ = old;
        }
        Set(value);
    }
    ~ScopedEnv() { Set(had_ ? old_.c_str() : nullptr); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void Set(const char* value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value != nullptr ? value : "");
#else
        if (value != nullptr) {
            setenv(name_.c_str(), value, 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }
    std::string name_;
    std::string old_;
    bool had_ = false;
};

bool VulkanRequired() {
    const char* value = std::getenv("MH4N_REQUIRE_VULKAN");
    return value != nullptr && std::string(value) == "1";
}

// The backend on this machine's first device, or null with the reason (a skip, or a failure under
// MH4N_REQUIRE_VULKAN=1).
std::shared_ptr<rhi::Backend> MakeBackend() {
    auto instance = rhi::CreateInstance();
    if (!instance) {
        if (VulkanRequired()) {
            FAIL("no Vulkan and MH4N_REQUIRE_VULKAN=1: " << instance.error());
        }
        WARN_MESSAGE(false, "skipped: no Vulkan (" << instance.error() << ")");
        return nullptr;
    }
    auto backend = rhi::CreateBackend(*instance);
    if (!backend) {
        if (VulkanRequired()) {
            FAIL("no Vulkan device and MH4N_REQUIRE_VULKAN=1: " << backend.error());
        }
        WARN_MESSAGE(false, "skipped: no Vulkan device (" << backend.error() << ")");
        return nullptr;
    }
    return *backend;
}

// "rhi <name>: <value>" lines of a report, name -> value (the first of a repeated name).
std::map<std::string, std::string> RhiLines(const std::string& text) {
    std::map<std::string, std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("rhi ", 0) != 0) {
            continue;
        }
        const size_t colon = line.find(": ");
        if (colon == std::string::npos) {
            continue;
        }
        lines.emplace(line.substr(4, colon - 4), line.substr(colon + 2));
    }
    return lines;
}

std::string B(bool v) { return v ? "true" : "false"; }

std::string G(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

// What the report must print for `caps`, written out member by member. The structured binding names ALL
// members of Capabilities, in declaration order: a member added to the structure is a compile error here, so
// whoever adds one must add it to the report (rhi_report.cpp) and to this list.
std::vector<std::pair<std::string, std::string>> ExpectedFields(const rhi::Capabilities& caps) {
    const auto& [d24s8, interlock, index_uint8, custom_border_color, triangle_fans, logic_op, dynamic_rendering,
                 pipeline_library, eds3_blend, bc7, timeline, present_wait, direct_upload, geometry_shader,
                 stride_alignment, apple, primitive_restart_always_on, portability_subset, driver_id, driver_name,
                 device_name, api_version, timestamp_period, max_texel_buffer_elements,
                 uniform_buffer_offset_alignment, storage_buffer_offset_alignment, max_uniform_buffer_range,
                 max_storage_buffer_range, vendor_id, device_index, masked] = caps;
    const std::string masked_text = masked.ToString();
    return {
        {"d24s8", B(d24s8)},
        {"interlock", B(interlock)},
        {"index_uint8", B(index_uint8)},
        {"custom_border_color", B(custom_border_color)},
        {"triangle_fans", B(triangle_fans)},
        {"logic_op", B(logic_op)},
        {"dynamic_rendering", B(dynamic_rendering)},
        {"pipeline_library", B(pipeline_library)},
        {"eds3_blend", B(eds3_blend)},
        {"bc7", B(bc7)},
        {"timeline", B(timeline)},
        {"present_wait", B(present_wait)},
        {"direct_upload", B(direct_upload)},
        {"geometry_shader", B(geometry_shader)},
        {"stride_alignment", std::to_string(stride_alignment)},
        {"apple", B(apple)},
        {"primitive_restart_always_on", B(primitive_restart_always_on)},
        {"portability_subset", B(portability_subset)},
        {"driver_id", std::to_string(driver_id)},
        {"driver_name", driver_name},
        {"device_name", device_name},
        {"api_version", std::to_string(api_version)},
        {"timestamp_period", G(timestamp_period)},
        {"max_texel_buffer_elements", std::to_string(max_texel_buffer_elements)},
        {"uniform_buffer_offset_alignment", std::to_string(uniform_buffer_offset_alignment)},
        {"storage_buffer_offset_alignment", std::to_string(storage_buffer_offset_alignment)},
        {"max_uniform_buffer_range", std::to_string(max_uniform_buffer_range)},
        {"max_storage_buffer_range", std::to_string(max_storage_buffer_range)},
        {"vendor_id", std::to_string(vendor_id)},
        {"device_index", std::to_string(device_index)},
        {"masked", masked_text.empty() ? "none" : masked_text},
    };
}

}  // namespace

TEST_CASE("--rhi: the field list is every Capabilities field, in order, and prints what it holds") {
    // Two synthetic structures. The 13 bits of the first follow an irregular pattern, the second holds the
    // complement, so a report that swaps or copies two bits shows in at least one of the two. The values are
    // all different from each other (and between the two) for the same reason.
    constexpr uint32_t kPattern = 0b1011001101010;
    rhi::Capabilities a;
    rhi::Capabilities b;
    bool* bits_a[] = {&a.d24s8, &a.interlock, &a.index_uint8, &a.custom_border_color, &a.triangle_fans,
                      &a.logic_op, &a.dynamic_rendering, &a.pipeline_library, &a.eds3_blend, &a.bc7, &a.timeline,
                      &a.present_wait, &a.direct_upload};
    bool* bits_b[] = {&b.d24s8, &b.interlock, &b.index_uint8, &b.custom_border_color, &b.triangle_fans,
                      &b.logic_op, &b.dynamic_rendering, &b.pipeline_library, &b.eds3_blend, &b.bc7, &b.timeline,
                      &b.present_wait, &b.direct_upload};
    static_assert(sizeof(bits_a) / sizeof(bits_a[0]) == rhi::kCapCount);
    for (uint32_t i = 0; i < rhi::kCapCount; ++i) {
        *bits_a[i] = ((kPattern >> i) & 1u) != 0;
        *bits_b[i] = !*bits_a[i];
    }
    a.geometry_shader = true;
    a.stride_alignment = 4;
    a.apple = true;
    a.primitive_restart_always_on = false;
    a.portability_subset = true;
    a.driver_id = 7;
    a.driver_name = "synthetic driver";
    a.device_name = "Synthetic RHI";
    a.api_version = 4206808;
    a.timestamp_period = 2.5f;
    a.max_texel_buffer_elements = 134217;
    a.uniform_buffer_offset_alignment = 64;
    a.storage_buffer_offset_alignment = 32;
    a.max_uniform_buffer_range = 65536;
    a.max_storage_buffer_range = 1048576;
    a.vendor_id = 4318;
    a.device_index = 2;
    a.masked = rhi::CapabilityMask{}.With(rhi::Cap::Interlock).With(rhi::Cap::Bc7);

    b.geometry_shader = false;
    b.stride_alignment = 16;
    b.apple = false;
    b.primitive_restart_always_on = true;
    b.portability_subset = false;
    b.driver_id = 19;
    b.driver_name = "other driver";
    b.device_name = "Other RHI";
    b.api_version = 4194304;
    b.timestamp_period = 0.125f;
    b.max_texel_buffer_elements = 16384;
    b.uniform_buffer_offset_alignment = 256;
    b.storage_buffer_offset_alignment = 128;
    b.max_uniform_buffer_range = 16384;
    b.max_storage_buffer_range = 2097152;
    b.vendor_id = 4098;
    b.device_index = 5;
    // b.masked stays empty

    for (const rhi::Capabilities* caps : {&a, &b}) {
        const auto expected = ExpectedFields(*caps);
        const std::vector<RhiField> fields = RhiFields(*caps);
        REQUIRE(fields.size() == expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {  // same names, same order, same values
            CHECK_MESSAGE(fields[i].name == expected[i].first, "field " << i);
            CHECK_MESSAGE(fields[i].value == expected[i].second, expected[i].first);
        }
        // the first 13 are the bits, named as the mask names them
        for (uint32_t i = 0; i < rhi::kCapCount; ++i) {
            CHECK(fields[i].name == rhi::CapName(static_cast<rhi::Cap>(i)));
            CHECK(fields[i].value == B(caps->Has(static_cast<rhi::Cap>(i))));
        }
        std::map<std::string, std::string> by_name;
        for (const RhiField& f : fields) {
            CHECK(by_name.emplace(f.name, f.value).second);  // no name twice
        }
    }
    CHECK(RhiFields(a)[0].value != RhiFields(b)[0].value);  // the two runs really differ
    CHECK(RhiFields(b).back().value == "none");
}

TEST_CASE("--rhi: the report prints every Capabilities field, each equal to Caps()") {
    ScopedEnv no_mask("MH4N_RHI_MASK", nullptr);
    const auto backend = MakeBackend();
    if (!backend) {
        return;
    }
    const rhi::Capabilities& caps = backend->Caps();
    auto lines = RhiLines(RenderRhiReport(*backend));
    // every field of Capabilities, checked against the structure itself (not against RhiFields)
    const auto expected = ExpectedFields(caps);
    REQUIRE(expected.size() == rhi::kCapCount + 18);
    for (const auto& [name, value] : expected) {
        const auto it = lines.find(name);
        REQUIRE_MESSAGE(it != lines.end(), "no line for " << name);
        CHECK_MESSAGE(it->second == value, name);
    }
    CHECK(caps.masked.bits == 0);
    // the present-wait answer of the backend itself (no surface here, so false)
    CHECK(lines["present_wait_available"] == B(backend->PresentWaitAvailable()));
    CHECK(lines["surface"] == "false");
    CHECK(lines["device"] == caps.device_name + " (" + caps.driver_name + ")");
    // and the same structure is what the field list prints
    for (const RhiField& f : RhiFields(caps)) {
        CHECK_MESSAGE(lines[f.name] == f.value, f.name);
    }
    MESSAGE("device: " << lines["device"]);
}

TEST_CASE("--rhi: MH4N_RHI_MASK=interlock prints rhi interlock: false and the mask") {
    ScopedEnv mask("MH4N_RHI_MASK", "interlock");
    const auto backend = MakeBackend();
    if (!backend) {
        return;
    }
    const std::string report = RenderRhiReport(*backend);
    CHECK(report.find("rhi interlock: false\n") != std::string::npos);
    CHECK(report.find("rhi masked: interlock\n") != std::string::npos);
    CHECK_FALSE(backend->Caps().interlock);
}

TEST_CASE("--rhi: the command, end to end") {
    ScopedEnv no_mask("MH4N_RHI_MASK", nullptr);
    std::string out;
    std::string err;
    const int rc = Run({"--rhi"}, &out, &err);
    if (rc == 3) {
        if (VulkanRequired()) {
            FAIL("no Vulkan device and MH4N_REQUIRE_VULKAN=1: " << err);
        }
        WARN_MESSAGE(false, "skipped: no Vulkan device (" << err << ")");
        return;
    }
    REQUIRE(rc == 0);
    CHECK(out.rfind("rhi device: ", 0) == 0);
    CHECK(out.find("rhi present_wait: ") != std::string::npos);
    CHECK(err.empty());

    // --rhi is for the live device only
    CHECK(Run({"--rhi", "--json"}, &out, &err) == 2);
    CHECK(Run({"--rhi", "--from-json", "x"}, &out, &err) == 2);
    CHECK(Run({"--rhi", "--loader", "x"}, &out, &err) == 2);
}

TEST_CASE("--rhi: an unknown name in MH4N_RHI_MASK is exit 2 and lists the valid names") {
    ScopedEnv mask("MH4N_RHI_MASK", "interlock,flux_capacitor");
    std::string out;
    std::string err;
    CHECK(Run({"--rhi"}, &out, &err) == 2);
    CHECK(err.find("MH4N_RHI_MASK") != std::string::npos);
    CHECK(out.empty());
}

#if !defined(_WIN32)
TEST_CASE("--rhi: no usable driver is exit 3 with a message, nothing on stdout") {
    // A loader with an empty driver list: the instance cannot be made. (Without any loader the answer is the
    // same: exit 3.)
    ScopedEnv a("VK_ICD_FILENAMES", (std::filesystem::temp_directory_path() / "mh4n_no_such_icd.json").string().c_str());
    ScopedEnv b("VK_DRIVER_FILES", (std::filesystem::temp_directory_path() / "mh4n_no_such_icd.json").string().c_str());
    ScopedEnv c("MH4N_RHI_MASK", nullptr);
    std::string out;
    std::string err;
    CHECK(Run({"--rhi"}, &out, &err) == 3);
    CHECK(out.empty());
    CHECK(err.find("--rhi") != std::string::npos);
    CHECK(err.find("nothing to report") != std::string::npos);
}
#endif
