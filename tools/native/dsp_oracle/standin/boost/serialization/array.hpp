// Stand-in of the DSP oracle (tools/native/dsp_oracle/dsp_oracle.py) for <boost/serialization/array.hpp>:
// Azahar's headers include it for save states; the oracle serializes nothing, so it only declares the
// name the audio headers befriend.
#pragma once

namespace boost::serialization {
class access;
}
