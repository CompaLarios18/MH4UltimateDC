// The in-game menu's touch taps, a section of its Controls tab (taps.h): keys that touch the touch
// screen at a point while they are held. A row each with a name, two keys (chosen with the menu's
// key capture, as a 3DS button's) and the point's X and Y, and a picture of the touch screen to
// put the points on with the mouse, with a quest's panel slots and the point last touched in the
// game.
//
// What the menu (ui/menu.cpp) calls, a line or two each:
//   - DrawControls, where the section goes:  if (DrawTouchTaps(layer)) Changed(AppSetting::Controls);
//                                            if (TouchTapCapturing()) capture = -1;  (one wait at a time)
//   - where its own capture starts (capture = id), before SetKeyCapture(true):  EndTouchTapEdit(layer);
//   - OnCapturedKey(code), first:            if (TouchTapCapturing()) {
//                                                if (OnTouchTapKey(layer, code)) Changed(AppSetting::Controls);
//                                                return;
//                                            }
//     and where it gives `key` to one of its own inputs:  ReleaseTouchTapKey(config, key);
//   - Draw: `idle` is false while TouchTapCapturing() (so that a controller's B ends the wait instead
//     of closing the menu), and the B that ends its own capture calls EndTouchTapEdit(layer) too
//   - Opened() and Closed(), first:          EndTouchTapEdit(layer);
//   - Closed(), case AppSetting::Controls:   file.touch_taps = now.touch_taps;
//   - Reset to the defaults, after the keys: SettleTouchTaps(config);  (config.h: the taps keep their
//                                            keys, which leave the defaults)
// The taps build the input profile again themselves (Window::RequestInputReload) once a key is set
// or a point let go, so Changed(), not ControlsChanged(), is what the menu calls for them: while a
// point is dragged DrawTouchTaps returns true every frame, and a profile built every frame would
// fill the log.
#pragma once

#include <string>

namespace App {
struct Config;
}

namespace App::Ui {

class Layer;

/// In the menu's frame, on the Controls tab: the section's title, the taps' rows, Add a tap and
/// the touch screen. True when the taps changed (while a point is dragged, every frame it moves):
/// the menu saves the controls when it closes.
bool DrawTouchTaps(Layer& layer);

/// A tap's key is waiting for a key (Layer::SetKeyCapture is on for it).
bool TouchTapCapturing();

/// The key pressed while TouchTapCapturing() (a scancode, Input::MOUSE_KEY + a mouse button,
/// Input::WHEEL_UP, ...): Esc keeps the tap's key, Delete takes it out, the app's own keys aren't
/// taken (a note says so), any other becomes the tap's and leaves the input it was on. The wait
/// ends. True when the controls changed.
bool OnTouchTapKey(Layer& layer, int code);

/// The menu's own capture gave `key` (Input::KeyName's) to another input: it leaves the taps.
void ReleaseTouchTapKey(Config& config, const std::string& key);

/// What the taps are in the middle of ends: a key's wait (the tap keeps its key) and a point being
/// dragged (the input profile takes it). As the menu opens and closes, and on a controller's B.
void EndTouchTapEdit(Layer& layer);

} // namespace App::Ui
