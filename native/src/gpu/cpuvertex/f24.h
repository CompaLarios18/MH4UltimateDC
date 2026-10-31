// Derived from Azahar src/video_core/pica_types.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; the file says "Licensed under GPLv2 or any later version"): the PICA float
// type as the interpreter uses it, a 24-bit float approximated with an IEEE single (Pica::f24).
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
//
// The interpreter's number type (N2-36). Like Azahar's Pica::Float<16, 7> it is a plain float
// whose multiplication follows the PICA: 0 x infinity is +0 instead of NaN, while a NaN factor
// stays NaN. The 16-bit mantissa is not enforced (Azahar does not either: "Stored as a regular
// float, merely for convenience"). Decoding a raw 24-bit value is pica::Float24ToFloat in
// gpu/pica/state.h, which the command processor already applies to every uniform.
//
// One difference from Azahar: a default-constructed value is 0, where Azahar's `float value` is
// left uninitialised.
#pragma once

#include <array>
#include <cmath>

namespace mh4n::gpu::cpuvertex {

struct f24 {
    float value = 0.0f;

    static f24 FromFloat32(float val) {
        f24 ret;
        ret.value = val;
        return ret;
    }
    static f24 Zero() {
        return FromFloat32(0.f);
    }
    static f24 One() {
        return FromFloat32(1.f);
    }

    float ToFloat32() const {
        return value;
    }

    f24 operator*(const f24& flt) const {
        float result = value * flt.ToFloat32();
        // PICA gives 0 instead of NaN when multiplying by inf
        if (std::isnan(result))
            if (!std::isnan(value) && !std::isnan(flt.ToFloat32()))
                result = 0.f;
        return f24::FromFloat32(result);
    }
    f24 operator/(const f24& flt) const {
        return f24::FromFloat32(ToFloat32() / flt.ToFloat32());
    }
    f24 operator+(const f24& flt) const {
        return f24::FromFloat32(ToFloat32() + flt.ToFloat32());
    }
    f24 operator-(const f24& flt) const {
        return f24::FromFloat32(ToFloat32() - flt.ToFloat32());
    }
    f24 operator-() const {
        return f24::FromFloat32(-ToFloat32());
    }

    bool operator<(const f24& flt) const {
        return ToFloat32() < flt.ToFloat32();
    }
    bool operator>(const f24& flt) const {
        return ToFloat32() > flt.ToFloat32();
    }
    bool operator>=(const f24& flt) const {
        return ToFloat32() >= flt.ToFloat32();
    }
    bool operator<=(const f24& flt) const {
        return ToFloat32() <= flt.ToFloat32();
    }
    bool operator==(const f24& flt) const {
        return ToFloat32() == flt.ToFloat32();
    }
    bool operator!=(const f24& flt) const {
        return ToFloat32() != flt.ToFloat32();
    }
};

// Azahar's Common::Vec4<f24>: the shader registers are made of these. `c` is contiguous, so the
// interpreter can hand out a pointer to the first component, as Azahar's does.
struct Vec4 {
    f24 c[4];
};

// Pica::AttributeBuffer: the compacted outputs of a shader unit (or the inputs of the geometry
// shader), 16 vectors.
using AttributeBuffer = std::array<Vec4, 16>;

} // namespace mh4n::gpu::cpuvertex
