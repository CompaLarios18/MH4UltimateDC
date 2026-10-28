#include "stop_message.h"

namespace App::StopMessage {

std::string Stopped(unsigned status, const std::string& details, double seconds, const std::string& log) {
    if (status == FATAL_ERROR && seconds < START_SECONDS) {
        return "The game stopped with a fatal error of its own as it started.\n"
               "That can mean it couldn't read its own files: a mod, a damaged dump, or a wrongly decrypted "
               "game file.\n"
               "\n"
               "What to try:\n"
               "- If mods are on, turn them off in the launcher's Mods and try again.\n"
               "- Dump the game again with GodMode9 on your 3DS, and choose the new .3ds, .cci or .cxi in "
               "Setup.\n"
               "- Install the update again (USA v1.3.0, Europe v1.1.0) from a .cia that GodMode9 made "
               "(Setup, step 2).\n"
               "With launcher = off in mh4u-app.ini, start the app with --launcher to reach Mods and Setup.\n"
               "\n"
               "If it still stops, please report it with the log, which has the error's numbers (its \"Fatal "
               "error\" lines).\n"
               "It's complete once this message is closed:\n" +
               log;
    }
    if (status == FATAL_ERROR) {
        // by now the game file passed the launcher's check and the game ran a while: the port's own
        // bugs are as likely, and a report costs less than a new dump
        return "The game stopped with a fatal error of its own.\n"
               "This can be a bug of the port: please report it with the log, which has the error's numbers.\n"
               "If it happens at the same place every time, a damaged dump can also be the cause:\n"
               "dump the game again with GodMode9 on your 3DS.\n"
               "\n"
               "The log is complete once this message is closed:\n" +
               log;
    }
    if (details.empty()) {
        return "The game stopped (status " + std::to_string(status) + ").\n"
               "The log may say why; it's complete once this message is closed:\n" +
               log;
    }
    return "The game stopped: " + details + "\n\nThe log:\n" + log;
}

std::string DidntStart(unsigned status, const std::string& details, const std::string& log) {
    if (details.empty()) {
        return "The game didn't start (status " + std::to_string(status) + ").\n"
               "The log may say why:\n" +
               log;
    }
    return "The game didn't start: " + details + "\n\nThe log:\n" + log;
}

std::string CodeModDoesntFit(const std::string& log) {
    return "The game didn't start: a code mod (code.ips or code.bps) doesn't fit its code.\n"
           "It's made for another version of the game (is the v1.3.0 update, or Europe's v1.1.0,\n"
           "installed?), or it's damaged.\n"
           "Turn it off in the launcher's Mods (with launcher = off in mh4u-app.ini, start the app with "
           "--launcher).\n"
           "The log may say which one it is:\n" +
           log;
}

} // namespace App::StopMessage
