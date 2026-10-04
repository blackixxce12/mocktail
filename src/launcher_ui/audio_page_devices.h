#ifndef MOCKTAIL_LAUNCHER_UI_AUDIO_PAGE_DEVICES_H_
#define MOCKTAIL_LAUNCHER_UI_AUDIO_PAGE_DEVICES_H_

#include <gio/gio.h>

#include <functional>
#include <string>
#include <vector>

namespace mocktail::launcher_ui {

// The sound devices as the game sees them: SDL 3's device names, which
// audio.output_device and audio.input_device must match exactly
// (sdl_audio_sink.cc ListSdlPlaybackDevices, sdl_audio_capture.cc
// ListSdlRecordingDevices). Duplicate names are kept.
struct AudioDeviceList {
  bool ok = false;
  // SDL's message when the audio subsystem could not start.
  std::string error;
  // The SDL audio driver: "pipewire", "pulseaudio", "alsa", ...
  std::string driver;
  std::vector<std::string> playback;
  std::vector<std::string> recording;
};

// Starts SDL's audio subsystem on a worker thread, lists the devices and
// shuts SDL down again; one listing runs at a time. `done` runs on the main
// context, unless `cancellable` was cancelled first.
void ListAudioDevicesAsync(GCancellable* cancellable,
                           std::function<void(AudioDeviceList)> done);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_AUDIO_PAGE_DEVICES_H_
