#include "window/wayland_global_probe.h"

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace mocktail {
namespace window {
namespace {

using std::chrono::milliseconds;

// The compositor's half of the wire protocol, as much of it as the probe
// uses: it reads wl_display.get_registry and wl_display.sync, then announces
// `globals` on the new registry and completes the sync callback.
class FakeCompositor final {
 public:
  enum class Behavior { kAnswer, kSilent, kHangUp };

  FakeCompositor(int fd, std::vector<std::string> globals, Behavior behavior)
      : fd_(fd), globals_(std::move(globals)), behavior_(behavior) {
    thread_ = std::thread([this] { Serve(); });
  }
  ~FakeCompositor() {
    if (thread_.joinable()) {
      thread_.join();
    }
    close(fd_);
  }
  FakeCompositor(const FakeCompositor&) = delete;
  FakeCompositor& operator=(const FakeCompositor&) = delete;

  // The requests arrived as libwayland marshals them.
  bool saw_requests() const { return registry_id_ != 0 && callback_id_ != 0; }

 private:
  bool ReadExactly(void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
      pollfd descriptor{fd_, POLLIN, 0};
      if (poll(&descriptor, 1, 2000) <= 0) {
        return false;
      }
      const ssize_t received = read(fd_, bytes + done, size - done);
      if (received <= 0) {
        return false;
      }
      done += static_cast<std::size_t>(received);
    }
    return true;
  }

  void Put(std::vector<std::uint32_t>* message, std::uint32_t word) {
    message->push_back(word);
  }

  void PutString(std::vector<std::uint32_t>* message, const std::string& text) {
    const std::uint32_t length = static_cast<std::uint32_t>(text.size() + 1);
    message->push_back(length);
    std::vector<std::uint32_t> words((length + 3) / 4, 0);
    std::memcpy(words.data(), text.c_str(), length);
    message->insert(message->end(), words.begin(), words.end());
  }

  void Send(std::uint32_t object, std::uint32_t opcode,
            const std::vector<std::uint32_t>& arguments) {
    std::vector<std::uint32_t> message = {object, 0};
    message.insert(message.end(), arguments.begin(), arguments.end());
    const std::uint32_t size =
        static_cast<std::uint32_t>(message.size() * sizeof(std::uint32_t));
    message[1] = (size << 16) | opcode;
    ASSERT_EQ(write(fd_, message.data(), size), static_cast<ssize_t>(size));
  }

  void Serve() {
    if (behavior_ == Behavior::kHangUp) {
      shutdown(fd_, SHUT_RDWR);
      return;
    }
    // wl_display.get_registry (opcode 1) and wl_display.sync (opcode 0),
    // each carrying the new object's id.
    for (int request = 0; request < 2; ++request) {
      std::uint32_t words[3] = {};
      if (!ReadExactly(words, sizeof(words)) || words[0] != 1 ||
          (words[1] >> 16) != sizeof(words)) {
        return;
      }
      ((words[1] & 0xffff) == 1 ? registry_id_ : callback_id_) = words[2];
    }
    if (behavior_ == Behavior::kSilent) {
      // Keep the connection open past the probe's timeout.
      pollfd descriptor{fd_, POLLIN, 0};
      poll(&descriptor, 1, 2000);
      return;
    }
    std::uint32_t name = 1;
    for (const std::string& global : globals_) {
      std::vector<std::uint32_t> arguments;
      Put(&arguments, name++);
      PutString(&arguments, global);
      Put(&arguments, 1);
      Send(registry_id_, 0, arguments);  // wl_registry.global
    }
    Send(callback_id_, 0, {7});    // wl_callback.done
    Send(1, 1, {callback_id_});    // wl_display.delete_id
    // Wait for the client to hang up.
    std::uint8_t ignored = 0;
    (void)ReadExactly(&ignored, 1);
  }

  int fd_ = -1;
  std::vector<std::string> globals_;
  Behavior behavior_;
  std::uint32_t registry_id_ = 0;
  std::uint32_t callback_id_ = 0;
  std::thread thread_;
};

// A connected socket pair; the probe gets one end, the fake the other.
std::pair<int, int> SocketPair() {
  int fds[2] = {-1, -1};
  EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
  return {fds[0], fds[1]};
}

class WaylandGlobalProbeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!WaylandClientAvailable()) {
      GTEST_SKIP() << "libwayland-client.so.0 is not installed";
    }
  }
};

TEST_F(WaylandGlobalProbeTest, SeesExplicitSyncAndPointerWarp) {
  const auto [client, server] = SocketPair();
  FakeCompositor compositor(server,
                            {"wl_compositor", "wp_linux_drm_syncobj_manager_v1",
                             "xdg_wm_base", "wp_pointer_warp_v1"},
                            FakeCompositor::Behavior::kAnswer);
  const WaylandGlobals globals =
      ProbeWaylandGlobalsOnSocket(client, milliseconds(2000));
  EXPECT_TRUE(globals.listed);
  EXPECT_TRUE(globals.drm_syncobj);
  EXPECT_TRUE(globals.pointer_warp);
}

TEST_F(WaylandGlobalProbeTest, ReportsACompositorWithoutExplicitSync) {
  const auto [client, server] = SocketPair();
  FakeCompositor compositor(server,
                            {"wl_compositor", "wl_shm", "xdg_wm_base",
                             "wp_linux_drm_syncobj_manager_v2"},
                            FakeCompositor::Behavior::kAnswer);
  const WaylandGlobals globals =
      ProbeWaylandGlobalsOnSocket(client, milliseconds(2000));
  EXPECT_TRUE(globals.listed);
  EXPECT_FALSE(globals.drm_syncobj);
  EXPECT_FALSE(globals.pointer_warp);
}

TEST_F(WaylandGlobalProbeTest, TellsHyprlandByItsOwnGlobals) {
  // As Hyprland lists them (sandbox-bench/tools/globals/hyprland.txt).
  {
    const auto [client, server] = SocketPair();
    FakeCompositor compositor(
        server,
        {"wl_compositor", "hyprland_lock_notifier_v1",
         "hyprland_surface_manager_v1", "xdg_wm_base"},
        FakeCompositor::Behavior::kAnswer);
    const WaylandGlobals globals =
        ProbeWaylandGlobalsOnSocket(client, milliseconds(2000));
    EXPECT_TRUE(globals.listed);
    EXPECT_TRUE(globals.hyprland);
    EXPECT_FALSE(globals.drm_syncobj);
  }

  // KWin and GNOME offer explicit sync but nothing of Hyprland's; a name
  // that only contains "hyprland" does not count either.
  const auto [client, server] = SocketPair();
  FakeCompositor compositor(
      server,
      {"wl_compositor", "wp_linux_drm_syncobj_manager_v1",
       "org_kde_plasma_shell", "zwp_hyprland_like_v1", "hyprland"},
      FakeCompositor::Behavior::kAnswer);
  const WaylandGlobals globals =
      ProbeWaylandGlobalsOnSocket(client, milliseconds(2000));
  EXPECT_TRUE(globals.listed);
  EXPECT_TRUE(globals.drm_syncobj);
  EXPECT_FALSE(globals.hyprland);
}

TEST_F(WaylandGlobalProbeTest, GivesUpOnASilentCompositorAtTheTimeout) {
  const auto [client, server] = SocketPair();
  FakeCompositor compositor(server, {}, FakeCompositor::Behavior::kSilent);
  const auto start = std::chrono::steady_clock::now();
  const WaylandGlobals globals =
      ProbeWaylandGlobalsOnSocket(client, milliseconds(50));
  const auto waited = std::chrono::steady_clock::now() - start;
  EXPECT_FALSE(globals.listed);
  EXPECT_FALSE(globals.drm_syncobj);
  EXPECT_LT(waited, milliseconds(1000));
}

TEST_F(WaylandGlobalProbeTest, FailsCleanlyWhenTheCompositorHangsUp) {
  const auto [client, server] = SocketPair();
  FakeCompositor compositor(server, {}, FakeCompositor::Behavior::kHangUp);
  const WaylandGlobals globals =
      ProbeWaylandGlobalsOnSocket(client, milliseconds(2000));
  EXPECT_FALSE(globals.listed);
}

class ProbeEnvironmentTest : public WaylandGlobalProbeTest {
 protected:
  void SetUp() override {
    WaylandGlobalProbeTest::SetUp();
    if (IsSkipped()) {
      return;
    }
    for (const char* name : {"WAYLAND_DISPLAY", "WAYLAND_SOCKET"}) {
      const char* value = std::getenv(name);
      saved_.emplace_back(name, value != nullptr
                                    ? std::optional<std::string>(value)
                                    : std::nullopt);
      unsetenv(name);
    }
    char pattern[] = "/tmp/mocktail_wayland_probe_XXXXXX";
    ASSERT_NE(mkdtemp(pattern), nullptr);
    directory_ = pattern;
  }
  void TearDown() override {
    for (const auto& [name, value] : saved_) {
      if (value) {
        setenv(name.c_str(), value->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
    std::error_code error;
    if (!directory_.empty()) {
      std::filesystem::remove_all(directory_, error);
    }
  }

  std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
  std::filesystem::path directory_;
};

TEST_F(ProbeEnvironmentTest, ConnectsThroughWaylandDisplay) {
  const std::filesystem::path path = directory_ / "wayland-test";
  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(listener, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  ASSERT_LT(path.string().size(), sizeof(address.sun_path));
  std::strcpy(address.sun_path, path.c_str());
  ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)),
            0);
  ASSERT_EQ(listen(listener, 1), 0);
  std::optional<FakeCompositor> compositor;
  std::thread accept_thread([&] {
    const int connection = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (connection >= 0) {
      compositor.emplace(connection,
                         std::vector<std::string>{
                             "wl_compositor", "wp_linux_drm_syncobj_manager_v1"},
                         FakeCompositor::Behavior::kAnswer);
    }
  });
  // An absolute WAYLAND_DISPLAY names the socket itself.
  ASSERT_EQ(setenv("WAYLAND_DISPLAY", path.c_str(), 1), 0);
  const WaylandGlobals globals = ProbeWaylandGlobals(milliseconds(2000));
  accept_thread.join();
  close(listener);
  EXPECT_TRUE(globals.listed);
  EXPECT_TRUE(globals.drm_syncobj);
  ASSERT_TRUE(compositor.has_value());
  EXPECT_TRUE(compositor->saw_requests());
}

TEST_F(ProbeEnvironmentTest, LeavesAnInheritedWaylandSocketToSdl) {
  ASSERT_EQ(setenv("WAYLAND_DISPLAY", "wayland-0", 1), 0);
  ASSERT_EQ(setenv("WAYLAND_SOCKET", "9", 1), 0);
  EXPECT_FALSE(ProbeWaylandGlobals(milliseconds(50)).listed);
  ASSERT_NE(std::getenv("WAYLAND_SOCKET"), nullptr);
  EXPECT_STREQ(std::getenv("WAYLAND_SOCKET"), "9");
}

TEST_F(ProbeEnvironmentTest, NeedsAWaylandDisplay) {
  EXPECT_FALSE(ProbeWaylandGlobals(milliseconds(50)).listed);
}

}  // namespace
}  // namespace window
}  // namespace mocktail
