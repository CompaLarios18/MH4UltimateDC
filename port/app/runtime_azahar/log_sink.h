// The Azahar runtime's logger: the app's own logging (util/log.h) forwarded to Azahar's, as
// class Frontend, so that the log file, the console and the --log filter treat the app's
// messages as before.
#pragma once

namespace App::Azahar {

/// Makes util/log.h's messages go to Azahar's logger. Call it before the app logs.
void InstallLogSink();

} // namespace App::Azahar
