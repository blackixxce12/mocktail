#include "launcher_ui/audio_page_devices.h"

#include <SDL3/SDL.h>
#include <glib.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

namespace mocktail::launcher_ui {
namespace {

std::mutex& SdlMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<std::string> Names(SDL_AudioDeviceID* ids, int count) {
  std::vector<std::string> names;
  for (int index = 0; index < count; ++index) {
    const char* name = SDL_GetAudioDeviceName(ids[index]);
    if (name != nullptr && name[0] != '\0') names.emplace_back(name);
  }
  // A stable order for the list; SDL's follows device discovery.
  std::stable_sort(names.begin(), names.end(),
                   [](const std::string& a, const std::string& b) {
                     return g_utf8_collate(a.c_str(), b.c_str()) < 0;
                   });
  return names;
}

AudioDeviceList ListNow() {
  std::lock_guard<std::mutex> lock(SdlMutex());
  AudioDeviceList list;
  // SDL's events subsystem (which audio starts) would otherwise turn
  // SIGINT/SIGTERM into SDL quit events nobody reads.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    list.error = SDL_GetError();
    SDL_Quit();
    return list;
  }
  const char* driver = SDL_GetCurrentAudioDriver();
  list.driver = driver != nullptr ? driver : "";
  int count = 0;
  if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count)) {
    list.playback = Names(ids, count);
    SDL_free(ids);
  }
  count = 0;
  if (SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count)) {
    list.recording = Names(ids, count);
    SDL_free(ids);
  }
  list.ok = true;
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  SDL_Quit();
  return list;
}

struct Request {
  std::function<void(AudioDeviceList)> done;
};

void OnListed(GObject*, GAsyncResult* result, gpointer data) {
  std::unique_ptr<Request> request(static_cast<Request*>(data));
  GError* error = nullptr;
  auto* list = static_cast<AudioDeviceList*>(
      g_task_propagate_pointer(G_TASK(result), &error));
  if (list == nullptr) {
    // Cancelled: whoever asked may be gone.
    g_clear_error(&error);
    return;
  }
  AudioDeviceList owned = std::move(*list);
  delete list;
  request->done(std::move(owned));
}

}  // namespace

void ListAudioDevicesAsync(GCancellable* cancellable,
                           std::function<void(AudioDeviceList)> done) {
  auto* request = new Request{std::move(done)};
  GTask* task = g_task_new(nullptr, cancellable, OnListed, request);
  g_task_run_in_thread(
      task, [](GTask* running, gpointer, gpointer, GCancellable*) {
        g_task_return_pointer(
            running, new AudioDeviceList(ListNow()),
            [](gpointer list) { delete static_cast<AudioDeviceList*>(list); });
      });
  g_object_unref(task);
}

}  // namespace mocktail::launcher_ui
