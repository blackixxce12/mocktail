#include "launcher_ui/setting_kinds.h"

#include <algorithm>
#include <string_view>

namespace mocktail::launcher_ui {
namespace {

constexpr std::string_view kBooleanKeys[] = {
    "runtime.headless",
    "performance.multithreaded_rendering",
    // runtime_config_file.cc reads it with ParseBoolean, like the others.
    "engine.nvidia_shader_mt",
    "window.high_dpi",
    "network.use_system_proxy",
    "integrations.fleasion.enabled",
    "integrations.discord_rpc.enabled",
    "integrations.discord_rpc.show_place_name",
    "integrations.discord_rpc.show_elapsed_time",
    "integrations.discord_rpc.join.enabled",
    "integrations.discord_rpc.join.public_servers_only",
    "launcher.show_on_start",
    "updates.automatic",
    "updates.launch_after_update",
};

constexpr std::string_view kTextKeys[] = {
    "audio.output_device",
    "audio.input_device",
    "window.title",
    "network.ca_bundle",
    "integrations.fleasion.ca_certificate",
    "integrations.discord_rpc.join.button_label",
    "integrations.discord_rpc.text.browsing",
    "integrations.discord_rpc.text.joining",
    "integrations.discord_rpc.text.playing",
    "integrations.discord_rpc.text.state",
    "integrations.discord_rpc.text.unknown_place",
    // Digits only, but an identifier: quoted so YAML never reads a number.
    "integrations.discord_rpc.application_id",
};

template <std::size_t N>
bool Contains(const std::string_view (&keys)[N], std::string_view key) {
  return std::find(std::begin(keys), std::end(keys), key) != std::end(keys);
}

bool IsInteger(std::string_view value) {
  if (!value.empty() && value.front() == '-') value.remove_prefix(1);
  return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
    return c >= '0' && c <= '9';
  });
}

}  // namespace

bool IsTextKey(std::string_view key) { return Contains(kTextKeys, key); }

launcher::ScalarKind ScalarKindFor(std::string_view key,
                                   std::string_view value) {
  if (Contains(kBooleanKeys, key)) return launcher::ScalarKind::kBool;
  if (IsTextKey(key)) return launcher::ScalarKind::kString;
  if (IsInteger(value)) return launcher::ScalarKind::kInteger;
  // A host name is a token unless it needs quoting (an IPv6 address).
  if (key == "network.proxy_host" &&
      value.find_first_of(":#[]{},&*!|>'\"%@`") != std::string_view::npos) {
    return launcher::ScalarKind::kString;
  }
  return launcher::ScalarKind::kEnum;
}

}  // namespace mocktail::launcher_ui
