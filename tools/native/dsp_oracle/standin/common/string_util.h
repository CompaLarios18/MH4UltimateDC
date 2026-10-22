// Stand-in of the DSP oracle (tools/native/dsp_oracle/dsp_oracle.py) for common/string_util.h: empty.
// Azahar's effects.cpp includes it for Common::BytesToHex, whose only use is an argument of a LOG_DEBUG
// that the logging stand-in discards without compiling it.
#pragma once
