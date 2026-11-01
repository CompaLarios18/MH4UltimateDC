#include "ui/settings_index.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>

namespace App::Ui::Index {

namespace {

constexpr const char* TAB_NAMES[] = {"Display", "Graphics", "Touch screen", "Controls", "Keys",
                                     "Gameplay", "Audio",    "Online",       "Top of the menu"};

// the Keys tab's buttons (every key row shares one of these)
constexpr const char* KEY_HELP = "Choose, then press the new key or mouse button. Esc keeps the old one, Delete leaves "
                                 "none. If another action had that key, it loses it.";
constexpr const char* STICK_HELP = "Choose, then press the new key (a stick's direction takes a key, not the mouse). Esc "
                                   "keeps the old one, Delete leaves none.";

constexpr Kind S = Kind::Setting, K = Kind::Key, A = Kind::Action, L = Kind::Link, F = Kind::Fixed;

} // namespace

const Section SECTIONS[] = {
    {"display.frame_rate", Tab::Display, "Frame rate"},
    {"display.window", Tab::Display, "Window and renderer"},
    {"display.widescreen", Tab::Display, "Widescreen"},
    {"graphics.quality", Tab::Graphics, "Image quality"},
    {"graphics.effects", Tab::Graphics, "Effects"},
    {"touch.screens", Tab::TouchScreen, "Screens"},
    {"touch.corner", Tab::TouchScreen, "Touch screen in the corner"},
    {"touch.quests", Tab::TouchScreen, "In quests"},
    {"controls.input", Tab::Controls, "Input"},
    {"controls.mouse", Tab::Controls, "Mouse"},
    {"controls.controller", Tab::Controls, "Controller"},
    {"keys.hunter", Tab::Keys, "Hunter"},
    {"keys.moving", Tab::Keys, "Moving"},
    {"keys.camera", Tab::Keys, "Camera"},
    {"keys.items", Tab::Keys, "Items and menus"},
    {"keys.touch", Tab::Keys, "Touch screen"},
    {"keys.port", Tab::Keys, "The port's keys"},
    {"gameplay.buttons", Tab::Gameplay, "Buttons for touch panels"},
    {"gameplay.after_quest", Tab::Gameplay, "After a quest"},
    {"gameplay.speed", Tab::Gameplay, "Speed"},
    {"gameplay.touch_taps", Tab::Gameplay, "Touch taps"},
    {"audio.sound", Tab::Audio, "Sound"},
    // (no titles: Online's page has headings of its own, the search names the tab)
    {"online.net_sync", Tab::Online, ""},
    {"online.rooms", Tab::Online, ""},
    {"header", Tab::Header, ""},
};
static_assert(std::size(SECTIONS) == SECTION_COUNT, "SectionId and SECTIONS[] differ");

// id, section, kind, label, search label, help, search words, parent, shown when, next start,
// contextual, writes
const Row ROWS[] = {
    // ── Display ──
    {"display.fps", "display.frame_rate", S, "Frame rate", nullptr,
     "The most frames per second the game draws; its speed stays right at any rate. Above 60, a busy scene may "
     "need a higher CPU clock speed.",
     "fps framerate frame rate cap limit max smooth 30 60 90 120 144 165 180 240 performance game", "", "", false,
     false, "mh4u.ini fps"},
    {"display.vsync", "display.frame_rate", S, "Vsync", nullptr,
     "Frames shown in step with the monitor, without tearing. Fast forward turns it off while it runs.",
     "vsync v sync vertical sync tearing tear stutter", "", "", false, false, "mh4u-app.ini [Display] vsync"},
    {"display.fps_counter", "display.frame_rate", S, "Frame-rate counter", nullptr,
     "The frames per second and a frame's time in a corner of the screen. {key:fps_counter_key} shows and hides "
     "it while you play.",
     "fps counter frame rate counter show fps frametime ms performance stats overlay corner f3", "", "", false,
     false, "mh4u-app.ini [Display] fps_counter"},
    {"display.cpu_clock", "display.frame_rate", S, "CPU clock speed", nullptr,
     "The emulated CPU's speed, in % of a 3DS's. Raise it if a busy scene can't hold your frame rate; too much can "
     "make the game misbehave.",
     "cpu clock speed overclock emulation slow slowdown lag busy performance game", "", "", false, false,
     "mh4u-app.ini [Game] cpu_clock"},
    {"display.refresh_rate", "display.frame_rate", S, "Screen refresh rate", nullptr,
     "How often the emulated screen refreshes; the game shows at most one frame per refresh. Auto follows the "
     "frame rate above 60.",
     "refresh rate hz monitor 60hz 120hz 144hz 165hz 240hz lcd vblank game", "", "", false, false,
     "mh4u.ini refresh_rate"},
    {"display.fullscreen", "display.window", S, "Full screen", nullptr,
     "The game fills the screen; off, it plays in a window. {key:fullscreen_key} or Alt+Enter switch it too.",
     "full screen fullscreen windowed window borderless mode f11 alt enter", "", "", false, false,
     "mh4u-app.ini [Display] fullscreen"},
    {"display.renderer", "display.window", S, "Renderer", nullptr,
     "How the game is drawn. Vulkan runs on Metal through MoltenVK on a Mac. If the one chosen doesn't start, the "
     "next one does.",
     "renderer vulkan opengl software graphics api backend gpu driver metal moltenvk crash", "", "", true, false,
     "mh4u-app.ini [Display] renderer"},
    {"display.launcher", "display.window", S, "Launcher before the game", nullptr,
     "On: the app opens on its launcher (Play, Setup, Mods, Online, Settings). Off: straight into the game while "
     "the setup is complete.",
     "launcher home screen start startup skip launcher play setup", "", "", true, false,
     "mh4u-app.ini [Game] launcher"},
    {"display.aspect", "display.widescreen", S, "Aspect ratio", nullptr,
     "The top screen's shape: the 3D view is widened (or narrowed) to it. 16:9 fills most monitors, 16:10 a Steam "
     "Deck.",
     "widescreen aspect ratio shape of the top screen 16 9 16 10 21 9 32 9 4 3 ultrawide stretch black bars steam "
     "deck game",
     "", "", true, false, "mh4u.ini widescreen"},
    {"display.wide_render", "display.widescreen", S, "Draw the wider picture", nullptr,
     "On: the game draws the wider picture itself and the HUD keeps its shape. Off: a 400x240 picture is "
     "stretched.",
     "wide render wider picture native widescreen stretch hud game", "", "", true, false, "mh4u.ini wide_render"},
    {"display.hud_edges", "display.widescreen", S, "HUD at the screen's edges", nullptr,
     "Health, stamina and the party list at the left edge and the items at the right one, instead of near the "
     "middle.",
     "hud edges ui interface health stamina sharpness items party ultrawide game", "", "", false, false,
     "mh4u.ini hud_edges"},

    // ── Graphics ──
    {"graphics.resolution", "graphics.quality", S, "Internal resolution", nullptr,
     "The 3D view drawn at this many times the 3DS's 400x240. Higher is sharper and asks more of the graphics "
     "card.",
     "internal resolution render scale upscale upscaling sharp blurry 1080p 1440p 4k quality", "", "", false, false,
     "mh4u-app.ini [Display] resolution"},
    {"graphics.linear_filter", "graphics.quality", S, "Smooth scaling", nullptr,
     "Smooths the picture when it is scaled to the window. Off keeps its pixels sharp.",
     "smooth scaling linear filter bilinear nearest pixelated pixels sharp", "", "", false, false,
     "mh4u-app.ini [Display] linear_filter"},
    {"graphics.antialiasing", "graphics.quality", S, "Anti-aliasing", nullptr,
     "The game's own anti-aliasing, as on a 3DS. Off saves its pass: at a high internal resolution the edges stay "
     "fine.",
     "anti aliasing antialiasing aa fsaa jaggies jagged edges picture game", "", "", false, false,
     "mh4u.ini antialiasing"},
    {"graphics.dither", "graphics.quality", S, "Dither pattern", nullptr,
     "The 3DS's dither pattern. Scaled up it shows as a fine grid over everything: turn it off above 1x internal "
     "resolution.",
     "dither dithering grid pattern dots noise checkerboard trinity picture game", "", "", false, false,
     "mh4u.ini dither"},
    {"graphics.stereo_3d", "graphics.quality", S, "Stereoscopic 3D", nullptr,
     "The game draws the top screen twice, once per eye, even with the 3D slider at 0: a scene pass more and a "
     "soft picture.",
     "stereoscopic stereo 3d double doubled image ghosting soft blurry picture game", "", "", false, false,
     "mh4u.ini stereo_3d"},
    {"graphics.custom_textures", "graphics.quality", S, "Texture packs", nullptr,
     "Texture packs from load/textures/0004000000126300/ in Azahar's user folder, such as the HD pack the "
     "launcher's Mods screen installs.",
     "texture packs textures custom textures hd pack mods high resolution replacement", "", "", true, false,
     "mh4u-app.ini [Display] custom_textures"},
    {"graphics.bloom", "graphics.effects", S, "Bloom", nullptr,
     "The glow over bright parts of the picture, which lays a soft haze over it. Off: sharper, with more contrast.",
     "bloom glow glow over bright parts haze bright soft light picture filters game", "", "", false, false,
     "mh4u.ini bloom"},
    {"graphics.depth_of_field", "graphics.effects", S, "Depth of field", nullptr,
     "The focus blur of cutscenes. Emulation gets it wrong and can blur the whole picture: off keeps cutscenes "
     "sharp.",
     "depth of field dof focus blur focus blur of cutscenes cutscene blurry picture filters game", "", "", false,
     false, "mh4u.ini depth_of_field"},
    {"graphics.motion_blur", "graphics.effects", S, "Motion blur", nullptr,
     "The blending in of earlier frames that some effects add.", "motion blur ghosting trails smear picture filters game",
     "", "", false, false, "mh4u.ini motion_blur"},
    {"graphics.radial_blur", "graphics.effects", S, "Radial blur", nullptr,
     "The zoom blur around a roaring monster, and in the cutscenes that use it.",
     "radial blur zoom blur zoom blur of roars roar picture filters game", "", "", false, false,
     "mh4u.ini radial_blur"},
    {"graphics.haze", "graphics.effects", S, "Heat haze", nullptr, "The heat shimmer over hot places.",
     "heat haze heat shimmer shimmer distortion picture filters game", "", "", false, false, "mh4u.ini haze"},

    // ── Touch screen ──
    {"touch.screens", "touch.screens", S, "Screens", nullptr,
     "Where the 3DS's two screens go. In a corner, the touch screen sits over the game, and in quests it can show "
     "the map alone.",
     "screens layout dual screen bottom screen touch screen stacked side by side single two windows second window "
     "display",
     "", "", false, false, "mh4u-app.ini [Display] screens"},
    {"touch.overlay", "touch.corner", S, "Touch screen over the game", nullptr,
     "On: the touch screen drawn over the game in a corner, and clickable there. Off: Azahar's own custom layout, "
     "with fixed places.",
     "touch screen over the game overlay corner bottom screen show hide over the game in a corner game", "", "",
     false, false, "mh4u.ini touch_overlay"},
    {"touch.corner", "touch.corner", S, "Corner", nullptr,
     "The corner it sits in. The item bar is at the bottom right of the game's screen: choose the corner that "
     "hides the least.",
     "corner position place top bottom left right item bar game", "touch.overlay",
     "Touch screen over the game is on", false, false, "mh4u.ini touch_overlay"},
    {"touch.size", "touch.corner", S, "Size", nullptr, "How tall the touch screen is, in % of the window's height.",
     "size scale big bigger small smaller height touch screen game", "touch.overlay",
     "Touch screen over the game is on", false, false, "mh4u.ini touch_overlay"},
    {"touch.opacity", "touch.corner", S, "Opacity", nullptr,
     "100% hides the game behind it; less lets the game show through.",
     "opacity transparency transparent see through alpha game", "touch.overlay", "Touch screen over the game is on",
     false, false, "mh4u.ini touch_overlay"},
    {"touch.map_only", "touch.quests", S, "Only the map in quests", nullptr,
     "In a quest the corner shows the map alone, and the whole touch screen again for the quest menu. The map key "
     "switches the two.",
     "map minimap quest map in quests only the map f12 game", "", "", false, false, "mh4u.ini quest_overlay"},
    {"touch.map_key", "touch.quests", K, "Map key", nullptr,
     "Switches a quest's corner between the map and the whole touch screen. A controller's right-stick click does "
     "it too.",
     "map key f12 m switch toggle whole touch screen right stick", "touch.map_only", "Only the map in quests is on",
     false, true, "mh4u-app.ini [Controls] touch_screen_key"},
    {"touch.hud_editor", "touch.quests", A, "Map and target camera views", "Edit HUD layout",
     "Shows the map and the target camera over the game to move and resize them, with the mouse or a controller. "
     "B or Esc cancels.",
     "edit hud layout editor move drag resize place position mouse controller map target camera", "", "", false,
     false, ""},
    {"touch.map_size", "touch.quests", S, "Map size", nullptr,
     "How tall the map is, in % of the window's height. 0: its size in the touch screen in the corner.",
     "map size scale big bigger small smaller minimap game", "touch.map_only", "Only the map in quests is on", false,
     false, "mh4u.ini quest_overlay"},
    {"touch.map_where", "touch.quests", S, "Map position", nullptr,
     "In the touch screen's corner, or where you put it: with Edit HUD layout or the two sliders below.",
     "map position place where move corner game", "touch.map_only", "Only the map in quests is on", false, false,
     "mh4u.ini quest_overlay"},
    {"touch.map_x", "touch.quests", S, "    Across", "Map across",
     "0: at the window's left edge, 100: at its right edge, 50: in the middle.",
     "map across horizontal left right x game", "touch.map_where", "Map position is Where you put it", false, false,
     "mh4u.ini quest_overlay"},
    {"touch.map_y", "touch.quests", S, "    Down", "Map down", "0: at the window's top edge, 100: at its bottom edge.",
     "map down vertical top bottom y game", "touch.map_where", "Map position is Where you put it", false, false,
     "mh4u.ini quest_overlay"},
    {"touch.target_view", "touch.quests", S, "Target camera by the map", nullptr,
     "With the map, the target camera panel in a small view of its own: whom the camera targets. A click on it "
     "changes the target.",
     "target camera view beside the map by the map lock on monster icon game", "touch.map_only",
     "Only the map in quests is on", false, false, "mh4u.ini target_overlay"},
    {"touch.target_size", "touch.quests", S, "Target camera size", nullptr,
     "How tall it is, in % of the window's height. 0: its size in the touch screen in the corner.",
     "target camera size scale map game", "touch.target_view", "Target camera by the map is on", false, false,
     "mh4u.ini target_overlay"},
    {"touch.target_where", "touch.quests", S, "Target camera position", nullptr,
     "Under the map (over it when the map is at the bottom), or where you put it: with Edit HUD layout or the two "
     "sliders below.",
     "target camera position place where map game", "touch.target_view", "Target camera by the map is on", false,
     false, "mh4u.ini target_overlay"},
    {"touch.target_x", "touch.quests", S, "    Across", "Target camera across",
     "0: at the window's left edge, 100: at its right edge, 50: in the middle.",
     "target camera across horizontal x map game", "touch.target_where",
     "Target camera position is Where you put it", false, false, "mh4u.ini target_overlay"},
    {"touch.target_y", "touch.quests", S, "    Down", "Target camera down",
     "0: at the window's top edge, 100: at its bottom edge.", "target camera down vertical y map game",
     "touch.target_where", "Target camera position is Where you put it", false, false, "mh4u.ini target_overlay"},

    // ── Controls ──
    {"controls.input_mode", "controls.input", S, "Play with", nullptr,
     "Keyboard and mouse: the mouse turns the camera and the key guides show. Controller: the mouse is the stylus. "
     "Auto: what you used last.",
     "play with input device mode controller gamepad keyboard mouse auto steam deck", "", "", false, false,
     "mh4u-app.ini [Controls] input_mode"},
    {"controls.key_guides", "controls.input", S, "Key guides", nullptr,
     "The keys for what the hunter does, small in a corner while you play with the keyboard and mouse. "
     "{key:key_guides_key} shows and hides them.",
     "key guides hints prompts keys on screen overlay corner f1 hide", "", "", false, false,
     "mh4u-app.ini [Controls] key_guides"},
    {"controls.mouse_sensitivity", "controls.mouse", S, "Mouse sensitivity", nullptr,
     "How fast the mouse turns the camera: 50 is 0.12 degrees a mouse count.",
     "mouse sensitivity camera speed look dpi aim", "", "", false, false,
     "mh4u-app.ini [Controls] mouse_sensitivity"},
    {"controls.mouse_vertical", "controls.mouse", S, "Vertical sensitivity", nullptr,
     "The mouse's speed up and down, in % of its speed left and right.",
     "vertical sensitivity up and down y axis mouse", "", "", false, false, "mh4u-app.ini [Controls] mouse_vertical"},
    {"controls.mouse_invert_y", "controls.mouse", S, "Invert up and down", nullptr,
     "The mouse moved up turns the camera down, and the other way round.",
     "invert up and down y axis vertical inverted flip mouse", "", "", false, false,
     "mh4u-app.ini [Controls] mouse_invert_y"},
    {"controls.mouse_invert_x", "controls.mouse", S, "Invert left and right", nullptr,
     "The mouse moved right turns the camera left, and the other way round.",
     "invert left and right x axis horizontal inverted flip mouse", "", "", false, false,
     "mh4u-app.ini [Controls] mouse_invert_x"},
    {"controls.mouse_deadzone", "controls.mouse", S, "Least push (C-stick way)", nullptr,
     "Where the mouse pushes the C-stick (an older code library): the least push it gives. With less, a slow mouse "
     "may not turn the camera.",
     "least push deadzone dead zone c stick mouse", "", "", false, false, "mh4u-app.ini [Controls] mouse_deadzone"},
    {"controls.controller_auto", "controls.controller", S, "Map a controller automatically", nullptr,
     "The first controller is mapped like MH3U on a Wii U: A, B, X and Y by position, ZL and ZR on the triggers, "
     "the camera on the right stick.",
     "controller gamepad joystick xbox playstation steam deck mapping automatic map a controller on its own", "", "",
     false, false, "mh4u-app.ini [Controls] controller"},

    // ── Keys (in their display order; the menu's BINDINGS keep theirs) ──
    {"keys.x", "keys.hunter", K, "Draw, attack (X)", nullptr, KEY_HELP, "draw weapon attack x", "", "", false, false,
     "mh4u-app.ini [Controls] button_x"},
    {"keys.a", "keys.hunter", K, "Attack, gather, talk (A)", nullptr, KEY_HELP, "attack gather carve talk interact a",
     "", "", false, false, "mh4u-app.ini [Controls] button_a"},
    {"keys.b", "keys.hunter", K, "Evade, crouch (B)", nullptr, KEY_HELP, "evade dodge roll crouch b", "", "", false,
     false, "mh4u-app.ini [Controls] button_b"},
    {"keys.y", "keys.hunter", K, "Use item, sheathe (Y)", nullptr, KEY_HELP, "use item sheathe potion y", "", "",
     false, false, "mh4u-app.ini [Controls] button_y"},
    {"keys.r", "keys.hunter", K, "Dash, guard (R)", nullptr, KEY_HELP, "dash run sprint guard block r", "", "", false,
     false, "mh4u-app.ini [Controls] button_r"},
    {"keys.l", "keys.hunter", K, "Camera, focus (L)", nullptr, KEY_HELP, "camera reset focus l", "", "", false, false,
     "mh4u-app.ini [Controls] button_l"},
    {"keys.zl", "keys.hunter", K, "ZL", nullptr, KEY_HELP, "zl target camera", "", "", false, false,
     "mh4u-app.ini [Controls] button_zl"},
    {"keys.zr", "keys.hunter", K, "ZR", nullptr, KEY_HELP, "zr kick skip", "", "", false, false,
     "mh4u-app.ini [Controls] button_zr"},
    {"keys.move_forward", "keys.moving", K, "Move forward (circle pad up)", nullptr, STICK_HELP,
     "move walk run forward up circle pad wasd", "", "", false, false, "mh4u-app.ini [Controls] circle_pad"},
    {"keys.move_back", "keys.moving", K, "Move back (circle pad down)", nullptr, STICK_HELP,
     "move back backward down circle pad", "", "", false, false, "mh4u-app.ini [Controls] circle_pad"},
    {"keys.move_left", "keys.moving", K, "Move left (circle pad left)", nullptr, STICK_HELP,
     "move left strafe circle pad", "", "", false, false, "mh4u-app.ini [Controls] circle_pad"},
    {"keys.move_right", "keys.moving", K, "Move right (circle pad right)", nullptr, STICK_HELP,
     "move right strafe circle pad", "", "", false, false, "mh4u-app.ini [Controls] circle_pad"},
    {"keys.walk", "keys.moving", K, "Walk (circle pad half way)", nullptr, STICK_HELP, "walk slow half way", "", "",
     false, false, "mh4u-app.ini [Controls] circle_pad"},
    {"keys.camera_up", "keys.camera", K, "Camera up (C-stick up)", nullptr, STICK_HELP, "camera look turn up c stick",
     "", "", false, false, "mh4u-app.ini [Controls] c_stick"},
    {"keys.camera_down", "keys.camera", K, "Camera down (C-stick down)", nullptr, STICK_HELP,
     "camera look turn down c stick", "", "", false, false, "mh4u-app.ini [Controls] c_stick"},
    {"keys.camera_left", "keys.camera", K, "Camera left (C-stick left)", nullptr, STICK_HELP,
     "camera look turn left c stick", "", "", false, false, "mh4u-app.ini [Controls] c_stick"},
    {"keys.camera_right", "keys.camera", K, "Camera right (C-stick right)", nullptr, STICK_HELP,
     "camera look turn right c stick", "", "", false, false, "mh4u-app.ini [Controls] c_stick"},
    {"keys.item_prev", "keys.items", K, "Item bar left (L+Y)", nullptr, KEY_HELP,
     "item bar scroll wheel left previous", "", "", false, false, "mh4u-app.ini [Controls] item_prev"},
    {"keys.item_next", "keys.items", K, "Item bar right (L+A)", nullptr, KEY_HELP, "item bar scroll wheel right next",
     "", "", false, false, "mh4u-app.ini [Controls] item_next"},
    {"keys.start", "keys.items", K, "Game menu (START)", nullptr, KEY_HELP, "start menu game menu", "", "", false,
     false, "mh4u-app.ini [Controls] button_start"},
    {"keys.select", "keys.items", K, "SELECT", nullptr, KEY_HELP, "select", "", "", false, false,
     "mh4u-app.ini [Controls] button_select"},
    {"keys.dpad_up", "keys.items", K, "D-pad up", nullptr, KEY_HELP, "d pad dpad arrow up", "", "", false, false,
     "mh4u-app.ini [Controls] button_up"},
    {"keys.dpad_down", "keys.items", K, "D-pad down", nullptr, KEY_HELP, "d pad dpad arrow down", "", "", false,
     false, "mh4u-app.ini [Controls] button_down"},
    {"keys.dpad_left", "keys.items", K, "D-pad left", nullptr, KEY_HELP, "d pad dpad arrow left", "", "", false,
     false, "mh4u-app.ini [Controls] button_left"},
    {"keys.dpad_right", "keys.items", K, "D-pad right", nullptr, KEY_HELP, "d pad dpad arrow right", "", "", false,
     false, "mh4u-app.ini [Controls] button_right"},
    {"keys.stylus", "keys.touch", K, "Mouse on touch screen (hold)", nullptr, KEY_HELP,
     "stylus mouse touch screen hold alt cursor", "", "", false, false, "mh4u-app.ini [Controls] stylus_key"},
    {"keys.touch_screen", "keys.touch", K, "Map / whole touch screen", nullptr, KEY_HELP,
     "map whole touch screen toggle f12 m", "", "", false, false, "mh4u-app.ini [Controls] touch_screen_key"},
    {"keys.guides", "keys.port", K, "Show / hide the key guides", nullptr, KEY_HELP, "key guides show hide f1", "", "",
     false, false, "mh4u-app.ini [Controls] key_guides_key"},
    {"keys.pause", "keys.port", K, "Pause (playing alone)", nullptr, KEY_HELP, "pause stop p", "", "", false, false,
     "mh4u-app.ini [Controls] pause_key"},
    {"keys.fast_forward", "keys.port", K, "Fast forward (playing alone)", nullptr, KEY_HELP,
     "fast forward turbo speed tab", "", "", false, false, "mh4u-app.ini [Controls] fast_forward_key"},
    {"keys.fixed_fullscreen", "keys.port", F, "Full screen", nullptr,
     "Set in mh4u-app.ini (fullscreen_key), not here. Alt+Enter switches full screen too.",
     "full screen fullscreen f11", "", "", false, false, ""},
    {"keys.fixed_fps_counter", "keys.port", F, "Frame-rate counter", nullptr,
     "Set in mh4u-app.ini (fps_counter_key), not here.", "fps counter frame rate f3", "", "", false, false, ""},
    {"keys.fixed_menu", "keys.port", F, "This menu", nullptr,
     "Always Esc, or a controller's Guide button or Back and Start together.", "menu options esc escape", "", "",
     false, false, ""},
    // the button under the groups (Menu::DrawResetKeys)
    {"keys.reset", "keys.port", A, "Reset keys to the defaults", nullptr,
     "Every key as it came, after Monster Hunter Rise on PC, and the controller mapped automatically. It asks first.",
     "reset default defaults restore rebind remap bindings controls", "", "", false, false, ""},

    // ── Gameplay ──
    {"gameplay.kick", "gameplay.buttons", S, "Kick", nullptr,
     "A button for the kick, which on a 3DS only the touch screen's kick panel does. The button keeps its own "
     "action too.",
     "kick touch panel button shortcut buttons for touch panels game", "", "", false, false, "mh4u.ini kick"},
    {"gameplay.target", "gameplay.buttons", S, "Target camera", nullptr,
     "A button for the target camera panel: the camera targets the large monster (with two: the first, the "
     "second, then off).",
     "target camera lock on monster icon panel button buttons for touch panels game", "", "", false, false,
     "mh4u.ini target"},
    {"gameplay.skip", "gameplay.after_quest", S, "Skip the scene", nullptr,
     "Once a quest is complete, taps the touch screen's Skip, which skips the scene of the monster's end. It can "
     "share the kick's button.",
     "skip the scene skip scene cutscene button after a quest game", "", "", false, false, "mh4u.ini skip"},
    {"gameplay.skip_end", "gameplay.after_quest", S, "End the quest", nullptr,
     "Held together after a quest, they end the wait before the return (the carving time); online, once every port "
     "player has held them.",
     "end the quest end quest carving carve time wait return early timer leave after a quest game", "", "", false,
     false, "mh4u.ini skip_end"},
    {"gameplay.skip_end_hold", "gameplay.after_quest", S, "Hold for", nullptr,
     "How long they are held before the quest ends, so that it doesn't end by accident.",
     "hold for hold time seconds end quest", "gameplay.skip_end", "End the quest isn't None", false, false,
     "mh4u.ini skip_end_hold"},
    {"gameplay.skip_hint", "gameplay.after_quest", S, "Skip hint", nullptr,
     "Lines at the bottom of the screen once a quest is complete: the keys that skip and end it, the hold, and "
     "online who hasn't asked.",
     "skip hint prompt notification flash flashing lines game", "", "", false, false, "mh4u.ini skip_hint"},
    {"gameplay.ff_speed", "gameplay.speed", S, "Fast forward speed", nullptr,
     "How fast the fast forward key runs the game while you play alone, not in a room. The pause key stops it "
     "where it is.",
     "fast forward speed turbo speed up 2x faster game", "", "", false, false,
     "mh4u-app.ini [Game] fast_forward_speed"},
    {"gameplay.ff_mode", "gameplay.speed", S, "Fast forward mode", nullptr,
     "Press the fast forward key once to start and once to stop, or go fast only while it is held.",
     "fast forward mode hold toggle while held press game", "", "", false, false,
     "mh4u-app.ini [Controls] fast_forward_hold"},
    {"gameplay.ff_key", "gameplay.speed", K, "Fast forward key", nullptr,
     "Runs the game at the speed above while you play alone. Choose, then press a key or mouse button: Esc keeps "
     "it, Delete leaves none.",
     "fast forward key turbo tab", "", "", false, true, "mh4u-app.ini [Controls] fast_forward_key"},
    {"gameplay.pause_key", "gameplay.speed", K, "Pause key", nullptr,
     "Stops the game where it is, and again goes on; playing alone only. Choose, then press a key: Esc keeps it, "
     "Delete leaves none.",
     "pause key stop p", "", "", false, true, "mh4u-app.ini [Controls] pause_key"},
    // drawn by ui/touch_taps.cpp, full width (touch_tap_1 to touch_tap_8)
    {"gameplay.touch_taps", "gameplay.touch_taps", Kind::Block, "Touch taps", nullptr,
     "Keys that touch the touch screen at a point while held: an item shortcut, the target camera or any other "
     "panel, without the mouse. Up to 8.",
     "touch taps tap hotkey hotkeys shortcut item shortcut item slot potion crafting press point touch key", "", "",
     false, false, "mh4u-app.ini [Controls] touch_tap_N"},

    // ── Audio ──
    {"audio.volume", "audio.sound", S, "Volume", nullptr, "How loud the game is.",
     "volume sound audio loud quiet mute music", "", "", false, false, "mh4u-app.ini [Audio] volume"},
    {"audio.output", "audio.sound", S, "Output", nullptr,
     "The system the sound goes out through. Try another if the sound crackles; None plays no sound.",
     "audio output device backend cubeb sdl openal crackling no sound", "", "", true, false,
     "mh4u-app.ini [Audio] output"},

    // ── Online: net sync, then the search's ways into the rooms' page (ui/multiplayer_tab.cpp) ──
    {"online.net_sync", "online.net_sync", S, "Net sync", nullptr,
     "In a room, the port's players wait a moment for one whose PC stalls, instead of the game dropping them.",
     "net sync sync lag stall stutter dropped disconnect connection lost room online multiplayer game", "", "", true,
     false, "mh4u.ini net_sync"},
    {"online.join", "online.rooms", L, "Join a room", nullptr,
     "Join another player's room while the game runs: an invite, an address, this network or a lobby.",
     "join room invite address paste friends multiplayer", "", "", false, false, ""},
    {"online.host", "online.rooms", L, "Host a room", nullptr,
     "This PC runs a room and plays in it; you get an invite to send your friends.",
     "host room server port forward password players multiplayer", "", "", false, false, ""},
    {"online.lan", "online.rooms", L, "Rooms on this network", nullptr,
     "Rooms that players on your network host with this app: nothing to type.",
     "lan local network wifi same house on this network multiplayer", "", "", false, false, ""},
    {"online.lobby", "online.rooms", L, "Lobby rooms", nullptr,
     "The rooms a lobby lists (the launcher's Online screen sets the lobby).", "lobby public rooms list multiplayer",
     "", "", false, false, ""},
    {"online.invite", "online.rooms", L, "Invite", nullptr,
     "In a room: its invite, to copy and send your friends.", "invite code copy friends multiplayer", "", "", false,
     false, ""},
    {"online.name", "online.rooms", L, "Your name", nullptr,
     "Your name in rooms: 4 to 20 letters, digits, spaces, '.', '_' or '-'.",
     "nickname name username your name multiplayer", "", "", false, false, ""},
    {"online.leave", "online.rooms", L, "Leave the room", nullptr, "In a room: leave it, or close the one this PC hosts.",
     "leave quit room disconnect close room multiplayer", "", "", false, false, ""},

    // ── The header ──
    {"header.search", "header", A, "Search", nullptr,
     "Type a setting's name or a word like fps, map, kick or bloom. Enter goes to the first match.", "", "", "",
     false, false, ""},
    {"header.quit", "header", A, "Quit game", nullptr,
     "Closes the app, as when its window is closed. Save in the game first.", "quit exit close desktop leave game", "",
     "", false, false, ""},
    {"header.back", "header", A, "Back to the game", nullptr,
     "Closes the menu (Esc, the controller's Guide button, or Back and Start together).", "", "", "", false, false,
     ""},
};
static_assert(std::size(ROWS) == ROW_COUNT, "RowId and ROWS[] differ");

namespace {

/// The tables worked out from the ids, once.
struct Tables {
    std::array<SectionId, ROW_COUNT> section_of{};
    std::array<RowId, ROW_COUNT> parent_of{};
    std::array<std::vector<std::string>, ROW_COUNT> names; ///< the words search matches, but `extra`
    std::array<std::vector<std::string>, ROW_COUNT> labels; ///< those of the label as listed
    std::array<std::vector<std::string>, ROW_COUNT> helps;
};

std::vector<std::string> Words(const std::string& normalized) {
    std::vector<std::string> words;
    std::size_t start = 0;
    while (start < normalized.size()) {
        std::size_t end = normalized.find(' ', start);
        if (end == std::string::npos) {
            end = normalized.size();
        }
        if (end > start) {
            words.push_back(normalized.substr(start, end - start));
        }
        start = end + 1;
    }
    return words;
}

const Tables& GetTables() {
    static const Tables tables = [] {
        Tables t;
        for (int i = 0; i < ROW_COUNT; ++i) {
            const Row& row = ROWS[i];
            t.section_of[i] = SectionId::Count;
            for (int s = 0; s < SECTION_COUNT; ++s) {
                if (std::strcmp(SECTIONS[s].id, row.section) == 0) {
                    t.section_of[i] = static_cast<SectionId>(s);
                }
            }
            t.parent_of[i] = RowId::Count;
            for (int p = 0; *row.parent && p < ROW_COUNT; ++p) {
                if (std::strcmp(ROWS[p].id, row.parent) == 0) {
                    t.parent_of[i] = static_cast<RowId>(p);
                }
            }
            const Section& section = SECTIONS[static_cast<int>(t.section_of[i])];
            std::string names = std::string(row.label) + " " + (row.search_label ? row.search_label : "") + " " +
                                section.title + " " + TAB_NAMES[static_cast<int>(section.tab)] + " " + row.keywords;
            t.names[i] = Words(Normalize(names));
            t.labels[i] = Words(Normalize(row.search_label ? row.search_label : row.label));
            t.helps[i] = Words(Normalize(row.help));
        }
        return t;
    }();
    return tables;
}

bool IsStopWord(const std::string& word) {
    static constexpr const char* STOP_WORDS[] = {"a",  "an",  "and", "the", "of", "to",   "in",
                                                 "on", "for", "with", "or",  "my", "how",  "do",
                                                 "i",  "is",  "it",  "what", "where"};
    for (const char* stop : STOP_WORDS) {
        if (word == stop) {
            return true;
        }
    }
    return false;
}

bool StartsWith(const std::string& word, const std::string& start) {
    return word.size() >= start.size() && word.compare(0, start.size(), start) == 0;
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

/// A word of the query is the start of one of `words` (keys, textures: also without their s). One
/// that ends in a digit starts a word only where that word's number ends: 60 finds 60 and 60hz,
/// not 600; 16 (of 16:9) not 165; f1 not f11 or f12.
bool Matches(const std::string& token, const std::vector<std::string>& words) {
    const bool plural = token.size() >= 4 && token.back() == 's';
    const std::string single = plural ? token.substr(0, token.size() - 1) : token;
    const bool number = !token.empty() && IsDigit(token.back());
    for (const std::string& word : words) {
        if (number) {
            if (StartsWith(word, token) && (word.size() == token.size() || !IsDigit(word[token.size()]))) {
                return true;
            }
        } else if (StartsWith(word, token) || (plural && StartsWith(word, single))) {
            return true;
        }
    }
    return false;
}

/// How closely a row's label as listed (its words) names the query's words: 0 they are its words,
/// 1 it has each of them whole, 2 each the start of one of its words (Matches), 3 not every one.
int Naming(const std::vector<std::string>& tokens, const std::vector<std::string>& label) {
    if (label == tokens) {
        return 0;
    }
    const auto whole = [&label](const std::string& token) {
        return std::find(label.begin(), label.end(), token) != label.end();
    };
    if (std::all_of(tokens.begin(), tokens.end(), whole)) {
        return 1;
    }
    const auto starts = [&label](const std::string& token) { return Matches(token, label); };
    return std::all_of(tokens.begin(), tokens.end(), starts) ? 2 : 3;
}

} // namespace

const char* TabName(Tab tab) {
    return TAB_NAMES[static_cast<int>(tab)];
}

const Section& Get(SectionId section) {
    return SECTIONS[static_cast<int>(section)];
}

const Row& Get(RowId row) {
    return ROWS[static_cast<int>(row)];
}

SectionId SectionOf(RowId row) {
    return GetTables().section_of[static_cast<int>(row)];
}

Tab TabOf(SectionId section) {
    return SECTIONS[static_cast<int>(section)].tab;
}

Tab TabOf(RowId row) {
    return TabOf(SectionOf(row));
}

RowId Find(const std::string& id) {
    for (int i = 0; i < ROW_COUNT; ++i) {
        if (id == ROWS[i].id) {
            return static_cast<RowId>(i);
        }
    }
    return RowId::Count;
}

RowId ParentOf(RowId row) {
    return GetTables().parent_of[static_cast<int>(row)];
}

std::string ListedLabel(RowId row) {
    const Row& r = Get(row);
    std::string label = r.search_label ? r.search_label : r.label;
    label.erase(0, label.find_first_not_of(' '));
    return label;
}

std::string Normalize(const std::string& text) {
    std::string out, word;
    const auto flush = [&] {
        if (!word.empty() && !IsStopWord(word)) {
            out += (out.empty() ? "" : " ") + word;
        }
        word.clear();
    };
    for (const unsigned char c : text) {
        const char lower = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
            word += lower;
        } else {
            flush();
        }
    }
    flush();
    return out;
}

bool Searchable(const std::string& query) {
    for (const std::string& word : Words(Normalize(query))) {
        if (word.size() >= 2) {
            return true;
        }
    }
    return false;
}

std::vector<Result> Search(const std::string& query, const std::vector<std::string>& extra) {
    const std::vector<std::string> tokens = Words(Normalize(query));
    std::vector<Result> results;
    if (tokens.empty()) {
        return results;
    }
    const Tables& tables = GetTables();
    std::vector<std::vector<std::string>> names(ROW_COUNT);
    for (int i = 0; i < ROW_COUNT; ++i) {
        names[i] = tables.names[i];
        if (i < static_cast<int>(extra.size())) {
            const std::vector<std::string> more = Words(Normalize(extra[i]));
            names[i].insert(names[i].end(), more.begin(), more.end());
        }
    }
    for (int tier = 1; tier <= 2 && results.empty(); ++tier) {
        // by how closely their labels name the query (Naming): the setting named so first
        std::array<std::vector<Result>, 4> named;
        for (int i = 0; i < ROW_COUNT; ++i) {
            if (!*ROWS[i].keywords) {
                continue; // the search field and Back: nothing to go to
            }
            std::vector<std::string> words = names[i];
            if (tier == 2) {
                words.insert(words.end(), tables.helps[i].begin(), tables.helps[i].end());
            }
            if (std::all_of(tokens.begin(), tokens.end(),
                            [&words](const std::string& token) { return Matches(token, words); })) {
                named[Naming(tokens, tables.labels[i])].push_back({i, tier});
            }
        }
        for (const std::vector<Result>& group : named) {
            results.insert(results.end(), group.begin(), group.end());
        }
    }
    return results;
}

} // namespace App::Ui::Index
