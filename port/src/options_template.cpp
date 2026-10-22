#include "options_template.h"

namespace Port {

std::string OptionsTemplateText() {
    return R"(# MH4U PC port options. Changes apply the next time the game starts.
# One "key = value" per line; a line starting with # is ignored. All of them, and what was
# tried: docs/PLAYING.md in the port's repository.

# frame-rate cap (the game runs at 30 on a 3DS; its speed stays right at any rate), e.g. your
# monitor's refresh rate; above 60 a busy scene may need Emulation > Configure > Debug > CPU
# Clock Speed raised
fps = 60

# the top screen's shape: 16:9, 16:10, 21:9, 32:9, 4:3, match (the window's shape: the whole
# screen on Android; callers pass their size to Port::SetWindowSize) or off.
# wide_render = 1 draws the wider picture itself (0 stretches a 400x240 one); hud_edges = 1
# moves the hunting HUD's sides (health, stamina, party list; the item selection) out to the
# screen's edges
widescreen = 16:9
wide_render = 1
hud_edges = 1

# View > Screen Layout > Custom Layout: the game fills the window and the touch screen sits
# in a corner (top-left, top-right, bottom-left, bottom-right), its height in % of the
# window's, its opacity in %. off = Azahar's own custom layout
touch_overlay = top-right 30 85
# in quests that corner shows only the map (off = the whole touch screen); F12 switches
# between the two while you play. map 40 = 40% of the window's height (without a size: the
# touch screen's scale); map 40 at 100 0 also places it: across and down in % of the room the
# window leaves (0 0 = top left, 100 0 = top right, 50 100 = bottom middle), so it stays in the
# window at any size. The in-game menu (Esc) > Game > Edit HUD layout places it with the mouse
quest_overlay = map
# with the map, the target camera panel (whose monster the camera follows; a tap on it changes
# the target) in a small view of its own, if your panel layout has it: on = under the map at
# the touch screen's scale, on 12 at 100 30 = 12% of the window's height, placed as above; off
target_overlay = on

# buttons for the kick and the target camera, touch-screen panels on a 3DS
# (A B X Y L R ZL ZR START UP DOWN LEFT RIGHT; ZL and ZR are a controller's triggers)
kick = ZR
target = ZL
# a button for the touch screen's Skip, which replaces the kick button once a quest is
# complete and skips the scene of the monster's end (the kick does nothing then, so ZR suits
# both); buttons held together for skip_end_hold seconds to end the wait before the return,
# the carving time, as if it had run out (online, once every player with the port has held
# them; the screen says who hasn't); skip_hint: lines on the screen say which keys do what
# (on, flash or off)
skip = ZR
skip_end = ZL+ZR
skip_end_hold = 1.0
skip_hint = on

# the game's dither pattern: off for a clean picture at an Internal Resolution above 1x
dither = off
# the game's stereoscopic 3D: it draws the top screen twice (one pass per eye) even with the 3D
# slider at 0, which costs a full scene pass and can show as a doubled, soft picture; off draws
# it once, as the community's "3D off" cheat does. on = as on a 3DS
stereo_3d = off
# the game's own anti-aliasing (on, as on a 3DS): off saves its pass, which at a high Internal
# Resolution costs frames for little; the edges get a little jagged
antialiasing = on

# the game's screen filters (on = as on a 3DS, off = skipped): the glow over bright parts, which
# softens the picture; the focus blur of cutscenes, which emulation gets wrong (it can blur the
# whole picture); the blur some effects blend in; the zoom blur of roars; heat shimmer
bloom = off
depth_of_field = off
motion_blur = on
radial_blur = on
haze = on

# multiplayer: wait a moment for a player with the port whose PC stalls, instead of the game
# dropping them (on, off; all = with every port player in the room)
net_sync = on
)";
}

} // namespace Port
