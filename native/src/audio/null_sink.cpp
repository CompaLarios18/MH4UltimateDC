// The null AudioSink (N1-18): registered as "null", the runtime's default (--audio). It takes the
// DSP's frames and drops them.
#include <memory>

#include "mh4n/host.h"
#include "mh4n/registry.h"

namespace mh4n {

namespace {

class NullSink final : public AudioSink {
public:
    void Queue(const s16*, std::size_t) override {}
};

MH4N_REGISTER(AudioSinkFactory, "null",
              [](Runtime&) -> std::unique_ptr<AudioSink> { return std::make_unique<NullSink>(); });

}  // namespace

}  // namespace mh4n
