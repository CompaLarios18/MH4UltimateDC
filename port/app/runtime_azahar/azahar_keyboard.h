// The software keyboard applet of Azahar's core, as the app's ISoftwareKeyboard sees it.
#pragma once

#include <memory>
#include <string>

#include "core/frontend/applets/swkbd.h"
#include "runtime.h"

namespace App::Azahar {

/// Registered with the core in place of its default keyboard: the requests go to `target` in the
/// app's types, and Finish validates the player's text against the request like Azahar's own
/// frontends do (Finalize) and hands it to the applet.
class AzaharKeyboard final : public Frontend::SoftwareKeyboard {
public:
    explicit AzaharKeyboard(std::shared_ptr<ISoftwareKeyboard> target_) : target(std::move(target_)) {}

    void Execute(const Frontend::KeyboardConfig& config) override;
    void ShowError(const std::string& error) override;

    KeyboardError Finish(const std::string& text, std::uint8_t button);

private:
    std::shared_ptr<ISoftwareKeyboard> target;
};

} // namespace App::Azahar
