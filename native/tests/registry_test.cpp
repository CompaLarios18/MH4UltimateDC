// mh4n/registry.h: Registry<T> and MH4N_REGISTER.
#include <doctest/doctest.h>

#include <functional>
#include <string>

#include "mh4n/registry.h"

using namespace mh4n;

namespace {
struct Widget {
    int id;
};
using WidgetMaker = std::function<int(int)>;
} // namespace

// Registered while the test binary starts, like a module's registrations.
MH4N_REGISTER(Widget, "zeta", Widget{3});
MH4N_REGISTER(Widget, "alpha", Widget{1});
MH4N_REGISTER(Widget, "mid", Widget{2});
MH4N_REGISTER(WidgetMaker, "double", [](int x) { return x * 2; });
MH4N_REGISTER(WidgetMaker, "pair", [](int x) { return std::pair<int, int>(x, x).second; });

TEST_CASE("MH4N_REGISTER registers from a static object") {
    auto& widgets = Registry<Widget>::Instance();
    REQUIRE(widgets.Find("alpha") != nullptr);
    CHECK(widgets.Find("alpha")->id == 1);
    CHECK(widgets.Find("zeta")->id == 3);
    CHECK(widgets.Find("missing") == nullptr);
}

TEST_CASE("All lists the entries in name order") {
    std::string names;
    for (const auto& entry : Registry<Widget>::Instance().All()) {
        names += entry.first + ",";
    }
    CHECK(names == "alpha,mid,zeta,");
}

TEST_CASE("every type has its own registry") {
    CHECK(Registry<WidgetMaker>::Instance().All().size() == 2);
    CHECK(Registry<Widget>::Instance().Find("double") == nullptr);
    const WidgetMaker* maker = Registry<WidgetMaker>::Instance().Find("double");
    REQUIRE(maker != nullptr);
    CHECK((*maker)(21) == 42);
    // a value with commas in it
    CHECK((*Registry<WidgetMaker>::Instance().Find("pair"))(7) == 7);
}

TEST_CASE("a duplicate name is refused") {
    Registry<int> registry; // a private one
    CHECK(registry.TryAdd("a", 1));
    CHECK(registry.TryAdd("b", 2));
    CHECK_FALSE(registry.TryAdd("a", 3));
    CHECK(*registry.Find("a") == 1);
    CHECK(registry.All().size() == 2);
    // Find accepts a string_view that is not null-terminated
    const std::string_view view("abc", 1);
    CHECK(*registry.Find(view) == 1);
}
