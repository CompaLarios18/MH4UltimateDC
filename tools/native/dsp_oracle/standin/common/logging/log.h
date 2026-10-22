// Stand-in of the DSP oracle (tools/native/dsp_oracle/dsp_oracle.py) for common/logging/log.h:
// logging prints nothing (the oracle's output is the table). The arguments are not evaluated.
#pragma once

#define LOG_TRACE(log_class, ...) (void(0))
#define LOG_DEBUG(log_class, ...) (void(0))
#define LOG_INFO(log_class, ...) (void(0))
#define LOG_WARNING(log_class, ...) (void(0))
#define LOG_ERROR(log_class, ...) (void(0))
#define LOG_CRITICAL(log_class, ...) (void(0))
