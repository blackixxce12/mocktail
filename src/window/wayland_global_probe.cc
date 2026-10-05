#include "window/wayland_global_probe.h"

#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mocktail {
namespace window {

namespace {

// libwayland-client's entry points, loaded at run time so the game does not
// link against it. Objects stay opaque: only pointers pass through.
struct WaylandClient {
  bool loaded = false;
  void* (*display_connect)(const char* name) = nullptr;
  void* (*display_connect_to_fd)(int fd) = nullptr;
  void (*display_disconnect)(void* display) = nullptr;
  int (*display_get_fd)(void* display) = nullptr;
  int (*display_flush)(void* display) = nullptr;
  int (*display_prepare_read)(void* display) = nullptr;
  void (*display_cancel_read)(void* display) = nullptr;
  int (*display_read_events)(void* display) = nullptr;
  int (*display_dispatch_pending)(void* display) = nullptr;
  void* (*proxy_marshal_flags)(void* proxy, std::uint32_t opcode,
                               const void* interface, std::uint32_t version,
                               std::uint32_t flags, ...) = nullptr;
  int (*proxy_add_listener)(void* proxy, void (**implementation)(void),
                            void* data) = nullptr;
  void (*proxy_destroy)(void* proxy) = nullptr;
  std::uint32_t (*proxy_get_version)(void* proxy) = nullptr;
  const void* registry_interface = nullptr;
  const void* callback_interface = nullptr;
};

template <typename Function>
bool Resolve(void* library, const char* name, Function* function) {
  *function = reinterpret_cast<Function>(dlsym(library, name));
  return *function != nullptr;
}

WaylandClient LoadWaylandClient() {
  WaylandClient client;
  void* library = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    return client;
  }
  client.registry_interface = dlsym(library, "wl_registry_interface");
  client.callback_interface = dlsym(library, "wl_callback_interface");
  client.loaded =
      Resolve(library, "wl_display_connect", &client.display_connect) &&
      Resolve(library, "wl_display_connect_to_fd",
              &client.display_connect_to_fd) &&
      Resolve(library, "wl_display_disconnect", &client.display_disconnect) &&
      Resolve(library, "wl_display_get_fd", &client.display_get_fd) &&
      Resolve(library, "wl_display_flush", &client.display_flush) &&
      Resolve(library, "wl_display_prepare_read",
              &client.display_prepare_read) &&
      Resolve(library, "wl_display_cancel_read",
              &client.display_cancel_read) &&
      Resolve(library, "wl_display_read_events",
              &client.display_read_events) &&
      Resolve(library, "wl_display_dispatch_pending",
              &client.display_dispatch_pending) &&
      Resolve(library, "wl_proxy_marshal_flags",
              &client.proxy_marshal_flags) &&
      Resolve(library, "wl_proxy_add_listener", &client.proxy_add_listener) &&
      Resolve(library, "wl_proxy_destroy", &client.proxy_destroy) &&
      Resolve(library, "wl_proxy_get_version", &client.proxy_get_version) &&
      client.registry_interface != nullptr &&
      client.callback_interface != nullptr;
  // The library stays loaded: SDL uses it next, and it never unloads cleanly.
  return client;
}

const WaylandClient& Client() {
  static const WaylandClient client = LoadWaylandClient();
  return client;
}

constexpr std::uint32_t kDisplaySync = 0;
constexpr std::uint32_t kDisplayGetRegistry = 1;

struct ProbeState {
  WaylandGlobals globals;
  bool done = false;
};

void OnGlobal(void* data, void* /*registry*/, std::uint32_t /*name*/,
              const char* interface, std::uint32_t /*version*/) {
  auto* state = static_cast<ProbeState*>(data);
  if (interface == nullptr) {
    return;
  }
  if (std::strcmp(interface, "wp_linux_drm_syncobj_manager_v1") == 0) {
    state->globals.drm_syncobj = true;
  } else if (std::strcmp(interface, "wp_pointer_warp_v1") == 0) {
    state->globals.pointer_warp = true;
  }
}

void OnGlobalRemove(void* /*data*/, void* /*registry*/,
                    std::uint32_t /*name*/) {}

void OnSyncDone(void* data, void* /*callback*/, std::uint32_t /*serial*/) {
  static_cast<ProbeState*>(data)->done = true;
}

// Laid out as libwayland expects a listener: one function per event, in
// protocol order.
struct RegistryListener {
  void (*global)(void*, void*, std::uint32_t, const char*, std::uint32_t);
  void (*global_remove)(void*, void*, std::uint32_t);
};
struct CallbackListener {
  void (*done)(void*, void*, std::uint32_t);
};
const RegistryListener kRegistryListener = {OnGlobal, OnGlobalRemove};
const CallbackListener kCallbackListener = {OnSyncDone};

template <typename Listener>
void (**Implementation(const Listener& listener))(void) {
  return reinterpret_cast<void (**)(void)>(const_cast<Listener*>(&listener));
}

// Reads events until the sync callback fires or the deadline passes, without
// ever blocking in libwayland.
void ReadUntilSynced(const WaylandClient& client, void* display,
                     std::chrono::milliseconds timeout, ProbeState* state) {
  using Clock = std::chrono::steady_clock;
  const Clock::time_point deadline = Clock::now() + timeout;
  const int fd = client.display_get_fd(display);
  while (!state->done) {
    if (client.display_prepare_read(display) != 0) {
      if (client.display_dispatch_pending(display) < 0) {
        return;
      }
      continue;
    }
    pollfd descriptor{fd, POLLIN, 0};
    if (client.display_flush(display) < 0) {
      if (errno != EAGAIN) {
        client.display_cancel_read(display);
        return;
      }
      descriptor.events |= POLLOUT;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        deadline - Clock::now());
    if (remaining.count() <= 0) {
      client.display_cancel_read(display);
      return;
    }
    const int ready = poll(&descriptor, 1,
                           remaining.count() > INT_MAX
                               ? INT_MAX
                               : static_cast<int>(remaining.count()));
    if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
      client.display_cancel_read(display);
      if ((ready < 0 && errno != EINTR) ||
          (ready > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) !=
                            0)) {
        return;
      }
      continue;
    }
    if (client.display_read_events(display) < 0 ||
        client.display_dispatch_pending(display) < 0) {
      return;
    }
  }
}

WaylandGlobals ProbeConnected(const WaylandClient& client, void* display,
                              std::chrono::milliseconds timeout) {
  ProbeState state;
  const std::uint32_t version = client.proxy_get_version(display);
  void* registry = client.proxy_marshal_flags(
      display, kDisplayGetRegistry, client.registry_interface, version, 0,
      static_cast<void*>(nullptr));
  void* callback =
      registry == nullptr
          ? nullptr
          : client.proxy_marshal_flags(display, kDisplaySync,
                                       client.callback_interface, version, 0,
                                       static_cast<void*>(nullptr));
  if (callback != nullptr &&
      client.proxy_add_listener(registry, Implementation(kRegistryListener),
                                &state) == 0 &&
      client.proxy_add_listener(callback, Implementation(kCallbackListener),
                                &state) == 0) {
    ReadUntilSynced(client, display, timeout, &state);
  }
  if (callback != nullptr) {
    client.proxy_destroy(callback);
  }
  if (registry != nullptr) {
    client.proxy_destroy(registry);
  }
  client.display_disconnect(display);
  state.globals.listed = state.done;
  return state.globals;
}

}  // namespace

bool WaylandClientAvailable() { return Client().loaded; }

WaylandGlobals ProbeWaylandGlobals(std::chrono::milliseconds timeout) {
  // libwayland would take this inherited socket over and unset it, leaving
  // SDL without its connection.
  const char* inherited_socket = std::getenv("WAYLAND_SOCKET");
  const char* display_name = std::getenv("WAYLAND_DISPLAY");
  if ((inherited_socket != nullptr && inherited_socket[0] != '\0') ||
      display_name == nullptr || display_name[0] == '\0') {
    return {};
  }
  const WaylandClient& client = Client();
  if (!client.loaded) {
    return {};
  }
  void* display = client.display_connect(nullptr);
  if (display == nullptr) {
    return {};
  }
  return ProbeConnected(client, display, timeout);
}

WaylandGlobals ProbeWaylandGlobalsOnSocket(int fd,
                                           std::chrono::milliseconds timeout) {
  const WaylandClient& client = Client();
  if (!client.loaded) {
    close(fd);
    return {};
  }
  // libwayland closes the socket itself when this fails.
  void* display = client.display_connect_to_fd(fd);
  if (display == nullptr) {
    return {};
  }
  return ProbeConnected(client, display, timeout);
}

}  // namespace window
}  // namespace mocktail
