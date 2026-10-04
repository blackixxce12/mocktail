#ifndef MOCKTAIL_RUNTIME_PROCESS_RELAUNCH_H_
#define MOCKTAIL_RUNTIME_PROCESS_RELAUNCH_H_

#include <signal.h>

#include <filesystem>
#include <string>
#include <vector>

#include "runtime/command_line.h"

namespace mocktail {
namespace runtime {

// Process-wide request to start Mocktail again once the runtime has shut
// down. The main loop sets it; main() acts on it after a clean shutdown.
void RequestProcessRelaunch();
bool ProcessRelaunchRequested();
void ResetProcessRelaunchRequestForTesting();

// argv[0..] for the relaunched process, built before argv is scrubbed. A
// website launch request is left out: that join belonged to this run, and a
// raw website URI may carry a gameinfo ticket that must not be reused.
// --launcher is left out and --play added, so the restart goes straight
// back to Roblox instead of showing the settings window again.
bool BuildProcessRelaunchArguments(const CommandLineOptions& options, int argc,
                                   const char* const argv[],
                                   std::vector<std::string>* arguments,
                                   std::string* error);

// How this process was started, captured first thing in main() so that a
// restart begins like opening Mocktail again from the same place: the
// installed executable (after an upgrade, the new one), the working
// directory, the environment before Mocktail and the game export their own
// settings, the signal mask and the signals that were ignored.
struct ProcessStartState {
  std::string executable;
  std::string working_directory;
  std::vector<std::string> environment;
  sigset_t signal_mask{};
  sigset_t ignored_signals{};
};

ProcessStartState CaptureProcessStartState();

// Replaces this process image with |start.executable| running |arguments| in
// |start|'s working directory and environment, set up to start from the
// session the website sign-in saved. The credential sink wrote it to
// roblox.cookie under |auth_root|, this run's auth root, and
// MOCKTAIL_AUTH_ROOT is set to that root (an empty path keeps |start|'s).
// MOCKTAIL_ROBLOX_COOKIES and MOCKTAIL_COOKIE_FILE are left out, since either
// would load the session that was signed out instead; without the pinned
// root, a run they overrode would then restart in the account store's
// selected slot rather than with the new session. Every descriptor above
// stderr is made close-on-exec first, so nothing this run opened (database
// locks, sockets, pipes to helpers) reaches the new process, and the signal
// mask and which signals are ignored return to |start|'s. Returns only on
// failure, with the working directory and signal state restored.
bool ExecProcessRelaunch(const ProcessStartState& start,
                         const std::vector<std::string>& arguments,
                         const std::filesystem::path& auth_root,
                         std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_PROCESS_RELAUNCH_H_
