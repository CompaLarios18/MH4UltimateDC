// Written for this package from the design: nothing is copied from Azahar here. See emitter.h.
#include "gpu/fragment/emitter.h"

#include <cstring>

namespace mh4n::gpu::fragment {

namespace {

constexpr char kLane[4] = {'x', 'y', 'z', 'w'};

std::string CfgRead(size_t word) {
    return fmt::format("cfg[{}].{}", word / 4, kLane[word % 4]);
}

}  // namespace

Emitter::Emitter(Mode mode, const FragmentKey& applied_key, const DeviceProfile& profile, ShaderPass pass)
    : mode_(mode), profile_(profile), pass_(pass) {
    std::memcpy(words_, &applied_key, sizeof(words_));
}

Value Emitter::Field(size_t word, u32 first, u32 count) const {
    if (specialised()) {
        return Value::Known(fragment::Bits(words_[word], first, count));
    }
    Value v;
    std::string expr = CfgRead(word);
    if (first != 0) {
        expr = fmt::format("({} >> {}u)", expr, first);
    }
    if (count < 32) {
        expr = fmt::format("({} & {}u)", expr, (1u << count) - 1u);
    }
    v.glsl = std::move(expr);
    return v;
}

Value Emitter::Slice(const Value& v, u32 first, u32 count) const {
    if (v.known) {
        return Value::Known(fragment::Bits(v.literal, first, count));
    }
    Value out;
    std::string expr = v.glsl;
    if (first != 0) {
        expr = fmt::format("({} >> {}u)", expr, first);
    }
    if (count < 32) {
        expr = fmt::format("({} & {}u)", expr, (1u << count) - 1u);
    }
    out.glsl = std::move(expr);
    return out;
}

Value Emitter::Select(const Cond& c, const Value& b, const Value& a) const {
    if (c.known) {
        return c.value ? b : a;
    }
    Value v;
    v.glsl = fmt::format("(({}) ? {} : {})", c.glsl, b.glsl, a.glsl);
    return v;
}

Cond Emitter::Eq(const Value& v, u32 n) const {
    if (v.known) {
        return Cond::Known(v.literal == n);
    }
    return Cond{false, false, fmt::format("({} == {}u)", v.glsl, n)};
}

Cond Emitter::Ne(const Value& v, u32 n) const {
    if (v.known) {
        return Cond::Known(v.literal != n);
    }
    return Cond{false, false, fmt::format("({} != {}u)", v.glsl, n)};
}

Cond Emitter::Eq(const Value& a, const Value& b) const {
    if (a.known && b.known) {
        return Cond::Known(a.literal == b.literal);
    }
    return Cond{false, false, fmt::format("({} == {})", a.glsl, b.glsl)};
}

Cond Emitter::Not(const Cond& c) const {
    if (c.known) {
        return Cond::Known(!c.value);
    }
    return Cond{false, false, fmt::format("(!{})", c.glsl)};
}

Cond Emitter::And(const Cond& a, const Cond& b) const {
    if (a.known && !a.value) return a;
    if (b.known && !b.value) return b;
    if (a.known) return b;
    if (b.known) return a;
    return Cond{false, false, fmt::format("({} && {})", a.glsl, b.glsl)};
}

Cond Emitter::Or(const Cond& a, const Cond& b) const {
    if (a.known && a.value) return a;
    if (b.known && b.value) return b;
    if (a.known) return b;
    if (b.known) return a;
    return Cond{false, false, fmt::format("({} || {})", a.glsl, b.glsl)};
}

void Emitter::If(const Cond& c, const Body& then, const Body& otherwise) {
    if (c.known) {
        const Body& chosen = c.value ? then : otherwise;
        if (chosen) chosen();
        return;
    }
    out_ += fmt::format("if ({}) {{\n", c.glsl);
    if (then) then();
    if (otherwise) {
        out_ += "} else {\n";
        otherwise();
    }
    out_ += "}\n";
}

void Emitter::Switch(const Value& v, std::initializer_list<std::pair<u32, Body>> cases, const Body& otherwise) {
    if (v.known) {
        for (const auto& [value, body] : cases) {
            if (value == v.literal) {
                if (body) body();
                return;
            }
        }
        if (otherwise) otherwise();
        return;
    }
    out_ += fmt::format("switch ({}) {{\n", v.glsl);
    for (const auto& [value, body] : cases) {
        out_ += fmt::format("case {}u: {{\n", value);
        if (body) body();
        out_ += "} break;\n";
    }
    out_ += "default: {\n";
    if (otherwise) otherwise();
    out_ += "} break;\n}\n";
}

void Emitter::ForEach(const Value& count, u32 max, std::string_view variable,
                      const std::function<void(const std::string& index)>& body) {
    if (count.known) {
        const u32 n = count.literal < max ? count.literal : max;
        for (u32 i = 0; i < n; ++i) {
            out_ += "{\n";
            body(fmt::format("{}u", i));
            out_ += "}\n";
        }
        return;
    }
    out_ += fmt::format("for (uint {0} = 0u; {0} < min({1}, {2}u); ++{0}) {{\n", variable, count.glsl, max);
    body(std::string(variable));
    out_ += "}\n";
}

}  // namespace mh4n::gpu::fragment
