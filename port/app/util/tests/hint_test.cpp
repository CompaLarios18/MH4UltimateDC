// The hint over the game after a quest (ui/hint.cpp) against a fake runtime: what it reads is
// IRuntime::SkipState (a copy the runtime hands over), never the hooks' own state, and the text
// is the keys of mh4u-app.ini and the controller's input. The menu's ControllerInputName is
// defined here as the one line it is. Synthetic data only.
// Built with -DMH4U_APP_TESTS=ON (port/app/CMakeLists.txt) or by hand against doctest and {fmt}.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "config.h"
#include "ui/hint.h"
#include "ui/menu.h"
#include "util/tests/fake_runtime.h"

namespace App::Ui {
std::string ControllerInputName(const std::string& params) {
    return Runtime().DescribeControllerInput(params);
}
} // namespace App::Ui

using namespace App;
using fake::Env;

TEST_CASE("no hint while the skip is not offered, or its hint is off") {
    Env env;
    const Config config = Defaults();
    env.runtime.skip.skip_button = "ZR";
    env.runtime.skip.hint = 1;
    CHECK(Ui::CurrentHint(config).Empty());  // not offered
    env.runtime.skip.offered = true;
    env.runtime.skip.hint = 0;
    CHECK(Ui::CurrentHint(config).Empty());  // skip_hint = 0
    env.runtime.skip.hint = 1;
    CHECK_FALSE(Ui::CurrentHint(config).Empty());
}

TEST_CASE("the hint names the keys of the buttons, and the controller's when it has them") {
    Env env;
    const Config config = Defaults();
    env.runtime.skip.offered = true;
    env.runtime.skip.hint = 1;
    env.runtime.skip.skip_button = "ZR";
    env.runtime.skip.end_buttons = "ZL+ZR";
    Ui::Hint hint = Ui::CurrentHint(config);
    CHECK(hint.skip == "C");  // the default key of ZR
    CHECK(hint.end == "Q+C");
    CHECK_FALSE(hint.flash);
    CHECK_FALSE(hint.requested);
    CHECK(hint.hold == 0.0f);
    // with a controller binding: "keys / controller"
    env.runtime.button_binding = "pad";
    env.runtime.controller_input_name = "RT";
    hint = Ui::CurrentHint(config);
    CHECK(hint.skip == "C / RT");
    CHECK(hint.end == "Q+C / RT+RT");
    // a button the settings don't know: the 3DS's own name
    env.runtime.skip.skip_button = "HOME";
    CHECK(Ui::CurrentHint(config).skip == "the 3DS's HOME");
}

TEST_CASE("the hold, the request, who is waited for and the flash come from the state") {
    Env env;
    const Config config = Defaults();
    env.runtime.skip.offered = true;
    env.runtime.skip.hint = 2;
    env.runtime.skip.end_buttons = "ZL+ZR";
    env.runtime.skip.end_hold = 0.5f;
    Ui::Hint hint = Ui::CurrentHint(config);
    CHECK(hint.flash);
    CHECK(hint.hold == doctest::Approx(0.5f));
    CHECK_FALSE(hint.requested);
    CHECK(hint.waiting_for.empty());
    env.runtime.skip.end_hold = 1.0f;
    env.runtime.skip.end_requested = true;
    env.runtime.end_skip_pending = {"Ana"};
    hint = Ui::CurrentHint(config);
    CHECK(hint.requested);
    CHECK(hint.hold == doctest::Approx(1.0f));
    CHECK(hint.waiting_for == std::vector<std::string>{"Ana"});
}
