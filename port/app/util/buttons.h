// Derived from Azahar src/common/settings.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): the order of the 3DS's buttons and sticks (NativeButton::Values and
// NativeAnalog::Values), which the settings file, the app's keys and the runtime's input profile
// index by.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
//
// The 3DS's buttons and sticks as the app numbers them. The Azahar runtime checks the numbers
// against Azahar's own (runtime_azahar/azahar_input.cpp).
#pragma once

namespace App {

namespace Button {
enum Id : int {
    A,
    B,
    X,
    Y,
    Up,
    Down,
    Left,
    Right,
    L,
    R,
    Start,
    Select,
    Debug,
    Gpio14,
    ZL,
    ZR,
    Home,
    Power,
    Count,
};
} // namespace Button

namespace Analog {
enum Id : int {
    CirclePad,
    CStick,
    Count,
};
} // namespace Analog

} // namespace App
