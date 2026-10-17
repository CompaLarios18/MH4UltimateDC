// A Backend that remembers every draw call and a copy of the state it saw (N2-04), for tests and
// for looking at what a command list draws.
#pragma once

#include <memory>
#include <vector>

#include "gpu/pica/backend.h"

namespace mh4n::gpu::pica {

struct RecordedDraw {
    DrawCall call;                        // call.state points at *state
    std::shared_ptr<const PicaState> state; // the state as it was when the draw was issued
};

class RecordingBackend final : public Backend {
public:
    void Draw(const DrawCall& draw) override {
        RecordedDraw recorded;
        recorded.state = std::make_shared<const PicaState>(*draw.state);
        recorded.call = draw;
        recorded.call.state = recorded.state.get();
        draws.push_back(std::move(recorded));
    }

    std::vector<RecordedDraw> draws;
};

} // namespace mh4n::gpu::pica
