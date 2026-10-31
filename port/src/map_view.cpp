#include "map_view.h"

namespace Port::MapView {

Block Blocked(const State& state) {
    if (!state.custom_layout) {
        return Block::Layout;
    }
    if (!state.touch_overlay) {
        return Block::TouchOverlay;
    }
    if (!state.quest_overlay) {
        return Block::QuestOverlay;
    }
    if (!state.panels_hook) {
        return Block::PanelsHook;
    }
    // before the key's switch: with no map panel, switching back wouldn't show one either
    if (state.panels.seen && state.panels.map_slot < 0) {
        return Block::NoMapPanel;
    }
    return state.switched ? Block::Switched : Block::None;
}

std::string MapNote(const State& state) {
    switch (Blocked(state)) {
    case Block::Layout:
        return "The screens aren't set to the game with the touch screen in a corner (Display), so the map view "
               "doesn't show and these settings change nothing.";
    case Block::TouchOverlay:
        return "The touch screen isn't over the game in a corner (Over the game, in a corner is off): Azahar's own "
               "custom layout shows the screens, so the map view doesn't show and these settings change nothing.";
    case Block::QuestOverlay:
        return "In quests, only the map is off: quests keep the whole touch screen in the corner, so these settings "
               "change nothing.";
    case Block::PanelsHook:
        return "Your code library is older than this app: it has no hook point for the quest's touch panels, so the "
               "map view doesn't show and these settings change nothing until you build it again.";
    case Block::NoMapPanel:
        return state.panels.in_quest
                   ? "Your touch screen's panels have no map now (Customize Panels in the quest menu): the map view "
                     "shows the map panel, which is two slots tall, so until it's back the corner shows the whole "
                     "touch screen and these settings change nothing."
                   : "Your touch screen's panels had no map in your last quest (Customize Panels in the quest menu): "
                     "the map view shows the map panel, which is two slots tall, so without it the corner shows the "
                     "whole touch screen and these settings change nothing.";
    case Block::Switched:
        return "Quests show the whole touch screen for now, so these settings change nothing there: the Map / touch "
               "screen key (F12 or M as it comes, or a controller's right-stick click) switched to it. Press it "
               "again for the map.";
    case Block::None:
        break;
    }
    return "";
}

std::string TargetNote(const State& state, bool target_overlay) {
    if (!target_overlay) {
        return "";
    }
    if (Blocked(state) != Block::None) {
        return "The target camera's view shows beside the map's only, so it doesn't show either.";
    }
    if (state.panels.seen && state.panels.target_slot < 0) {
        return state.panels.in_quest
                   ? "Your touch screen's panels have no target camera now (Customize Panels in the quest menu): "
                     "this view shows the target camera panel, so until it's back these settings change nothing."
                   : "Your touch screen's panels had no target camera in your last quest (Customize Panels in the "
                     "quest menu): this view shows the target camera panel, so without it these settings change "
                     "nothing.";
    }
    return "";
}

std::string KeyNote(const State& state) {
    switch (Blocked(state)) {
    case Block::Layout:
        return "No map to show: the screens aren't the game with the touch screen in a corner (Esc > Display)";
    case Block::TouchOverlay:
        return "No map to show: the touch screen isn't over the game in a corner (Esc > Game > Touch screen)";
    case Block::QuestOverlay:
        return "No map to show: In quests, only the map is off (Esc > Game > Touch screen)";
    case Block::PanelsHook:
        return "No map to show: the code library is older than this app (build it again)";
    case Block::NoMapPanel:
        return state.panels.in_quest
                   ? "No map to show: your touch screen's panels have none (Customize Panels in the quest menu)"
                   : "No map to show: your last quest's touch screen panels had none (Customize Panels in the quest "
                     "menu)";
    case Block::Switched:  // the key switches back to the map
    case Block::None:
        break;
    }
    return "";
}

} // namespace Port::MapView
