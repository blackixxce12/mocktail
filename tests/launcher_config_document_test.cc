#include "launcher/config_document.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/runtime_config_bootstrap.h"
#include "runtime/runtime_config_file.h"

#ifndef MOCKTAIL_TEST_SOURCE_DIR
#error "MOCKTAIL_TEST_SOURCE_DIR must point at the Mocktail source tree"
#endif

namespace mocktail::launcher {
namespace {

class EmptyEnvironment final : public runtime::Environment {
 public:
  std::optional<std::string> Get(std::string_view) const override {
    return std::nullopt;
  }
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_config_document_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, std::string_view bytes,
               mode_t mode = 0600) {
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
  }
  ASSERT_EQ(chmod(path.c_str(), mode), 0);
}

mode_t FileMode(const std::filesystem::path& path) {
  struct stat status = {};
  return lstat(path.c_str(), &status) == 0 ? status.st_mode & 0777 : 0;
}

std::string Fixture() {
  return ReadFile(std::filesystem::path(MOCKTAIL_TEST_SOURCE_DIR) /
                  "tests/fixtures/launcher/user_config.yaml");
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin < text.size()) {
    std::size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      end = text.size();
    }
    lines.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  return lines;
}

struct LineDiff {
  std::vector<std::string> removed;
  std::vector<std::string> added;
};

// Longest-common-subsequence line diff: what an edit removed and added.
LineDiff Diff(const std::string& before, const std::string& after) {
  const std::vector<std::string> left = SplitLines(before);
  const std::vector<std::string> right = SplitLines(after);
  std::vector<std::vector<int>> table(left.size() + 1,
                                      std::vector<int>(right.size() + 1, 0));
  for (std::size_t i = left.size(); i-- > 0;) {
    for (std::size_t j = right.size(); j-- > 0;) {
      table[i][j] = left[i] == right[j]
                        ? table[i + 1][j + 1] + 1
                        : std::max(table[i + 1][j], table[i][j + 1]);
    }
  }
  LineDiff diff;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left.size() && j < right.size()) {
    if (left[i] == right[j]) {
      ++i;
      ++j;
    } else if (table[i + 1][j] >= table[i][j + 1]) {
      diff.removed.push_back(left[i++]);
    } else {
      diff.added.push_back(right[j++]);
    }
  }
  while (i < left.size()) {
    diff.removed.push_back(left[i++]);
  }
  while (j < right.size()) {
    diff.added.push_back(right[j++]);
  }
  return diff;
}

using Lines = std::vector<std::string>;

runtime::RuntimeConfigLoadResult LoadWithRealLoader(const std::string& bytes) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.path() / "config.yaml";
  {
    std::ofstream output(file, std::ios::binary);
    output << bytes;
  }
  return runtime::LoadRuntimeConfig(EmptyEnvironment(), file);
}

bool ContainsCyrillic(const std::string& line) {
  for (const char character : line) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte == 0xD0U || byte == 0xD1U) {
      return true;
    }
  }
  return false;
}

std::size_t LineNumberOf(const std::string& text, std::string_view line) {
  const std::vector<std::string> lines = SplitLines(text);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (lines[index] == line) {
      return index + 1;
    }
  }
  return 0;
}

TEST(LauncherConfigDocumentTest, FixtureIsAcceptedAndReadsCustomValues) {
  const ConfigDocument document = ConfigDocument::FromBytes(Fixture());
  std::string error;
  ASSERT_TRUE(document.Validate(&error)) << error;

  EXPECT_EQ(document.Get("version"), "1");
  EXPECT_EQ(document.Get("device"), "pc-windows-11");
  EXPECT_EQ(document.Get("graphics.backend"), "direct-vulkan");
  EXPECT_EQ(document.Get("graphics.frame_rate_limit"), "165");
  EXPECT_EQ(document.Get("graphics.vsync"), "off");
  EXPECT_EQ(document.Get("performance.multithreaded_rendering"), "true");
  EXPECT_EQ(document.Get("performance.gamemode"), "on");
  EXPECT_EQ(document.Get("window.width"), "1600");
  EXPECT_EQ(document.Get("window.height"), "900");
  EXPECT_EQ(document.Get("window.high_dpi"), "true");
  EXPECT_EQ(document.Get("integrations.discord_rpc.join.enabled"), "true");
  EXPECT_EQ(document.Get("updates.source"), "apk-pure");

  // Commented examples and absent blocks read as absent.
  EXPECT_EQ(document.Get("network.proxy_port"), std::nullopt);
  EXPECT_EQ(document.Get("integrations.discord_rpc.join.button_label"),
            std::nullopt);
  EXPECT_EQ(document.Get("integrations.fleasion.enabled"), std::nullopt);
  EXPECT_EQ(document.Get("runtime.roblox_library"), std::nullopt);
  EXPECT_EQ(document.Get("graphics"), std::nullopt);
  EXPECT_TRUE(document.IsMapping("graphics"));
  EXPECT_TRUE(document.IsMapping("integrations.discord_rpc.join"));
  EXPECT_FALSE(document.IsMapping("device"));
  EXPECT_FALSE(document.IsMapping("integrations.fleasion"));
}

TEST(LauncherConfigDocumentTest, ReplacesOnlyTheEditedLines) {
  const std::string original = Fixture();
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Set("window.width", "1920", ScalarKind::kInteger,
                           &error))
      << error;
  ASSERT_TRUE(document.Set("window.height", "1080", ScalarKind::kInteger,
                           &error))
      << error;
  ASSERT_TRUE(document.Set("graphics.vsync", "on", ScalarKind::kEnum, &error))
      << error;
  ASSERT_TRUE(document.Set("graphics.frame_rate_limit", "display",
                           ScalarKind::kEnum, &error))
      << error;
  ASSERT_TRUE(document.Set("graphics.backend", "opengl", ScalarKind::kEnum,
                           &error))
      << error;
  ASSERT_TRUE(document.Set("integrations.discord_rpc.join.enabled", "false",
                           ScalarKind::kBool, &error))
      << error;
  ASSERT_TRUE(document.Set("performance.memory_limit_mb", "6144",
                           ScalarKind::kInteger, &error))
      << error;

  const LineDiff diff = Diff(original, document.bytes());
  EXPECT_EQ(diff.removed,
            (Lines{"  backend: direct-vulkan", "  frame_rate_limit: 165",
                   "  vsync: off", "  memory_limit_mb: 0",
                   "      enabled: true", "  width: 1600", "  height: 900"}));
  EXPECT_EQ(diff.added,
            (Lines{"  backend: opengl", "  frame_rate_limit: display",
                   "  vsync: on", "  memory_limit_mb: 6144",
                   "      enabled: false", "  width: 1920", "  height: 1080"}));
  // Same number of lines, every Cyrillic comment byte-identical in place.
  const std::vector<std::string> before_lines = SplitLines(original);
  const std::vector<std::string> after_lines = SplitLines(document.bytes());
  ASSERT_EQ(before_lines.size(), after_lines.size());
  std::size_t cyrillic = 0;
  for (std::size_t index = 0; index < before_lines.size(); ++index) {
    if (ContainsCyrillic(before_lines[index])) {
      ++cyrillic;
      EXPECT_EQ(after_lines[index], before_lines[index]);
    }
  }
  EXPECT_GE(cyrillic, 15U);

  ASSERT_TRUE(document.Validate(&error)) << error;
  const runtime::RuntimeConfigLoadResult loaded =
      LoadWithRealLoader(document.bytes());
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.window().width, 1920);
  EXPECT_EQ(loaded.config.window().height, 1080);
  EXPECT_TRUE(loaded.config.window().high_dpi);
  EXPECT_EQ(loaded.config.vsync_mode(), "on");
  EXPECT_EQ(loaded.config.frame_rate().mode,
            runtime::FrameRateLimitMode::kDisplay);
  EXPECT_EQ(loaded.config.graphics_backend(),
            runtime::GraphicsBackend::kSystem);
  EXPECT_FALSE(loaded.config.discord_rpc().join_enabled);
  EXPECT_EQ(loaded.config.performance().memory_limit_mb, 6144U);
}

TEST(LauncherConfigDocumentTest, SettingTheSameValueKeepsTheBytes) {
  const std::string original = Fixture();
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Set("graphics.frame_rate_limit", "165",
                           ScalarKind::kInteger, &error))
      << error;
  ASSERT_TRUE(document.Set("graphics.vsync", "off", ScalarKind::kEnum, &error))
      << error;
  // Free text is quoted only when it actually changes.
  ASSERT_TRUE(document.Set("window.title", "Roblox", ScalarKind::kString,
                           &error))
      << error;
  ASSERT_TRUE(document.Set("audio.output_device", "default",
                           ScalarKind::kString, &error))
      << error;
  ASSERT_TRUE(document.Set("window.width", "1600", ScalarKind::kInteger,
                           &error))
      << error;
  EXPECT_EQ(document.bytes(), original);
  ASSERT_TRUE(document.Set("window.title", "Roblox 2", ScalarKind::kString,
                           &error))
      << error;
  EXPECT_NE(document.bytes().find("  title: \"Roblox 2\"\n"),
            std::string::npos);
}

TEST(LauncherConfigDocumentTest,
     KeepsTrailingCommentsAndHandlesCharacterColumns) {
  // libyaml columns count characters: a Cyrillic title before the edited
  // value on the same line must not shift the splice.
  ConfigDocument document = ConfigDocument::FromBytes(
      "version: 1\n"
      "graphics:\n"
      "  vsync: off  # без синхронизации\n"
      "window: {title: \"Привет, мир\", width: 1280, height: 720}\n");
  std::string error;
  ASSERT_TRUE(document.Set("graphics.vsync", "on", ScalarKind::kEnum, &error))
      << error;
  ASSERT_TRUE(document.Set("window.width", "1600", ScalarKind::kInteger,
                           &error))
      << error;
  ASSERT_TRUE(document.Set("window.title", "Ёлка \"2\"", ScalarKind::kString,
                           &error))
      << error;
  EXPECT_EQ(document.bytes(),
            "version: 1\n"
            "graphics:\n"
            "  vsync: on  # без синхронизации\n"
            "window: {title: \"Ёлка \\\"2\\\"\", width: 1600, height: 720}\n");
  EXPECT_EQ(document.Get("window.title"), "Ёлка \"2\"");
  ASSERT_TRUE(document.Validate(&error)) << error;

  // Adding a key to a flow mapping is left to the user.
  EXPECT_FALSE(document.Set("window.high_dpi", "true", ScalarKind::kBool,
                            &error));
  EXPECT_NE(error.find("flow style"), std::string::npos) << error;
}

TEST(LauncherConfigDocumentTest, QuotesFreeTextSoItRoundTrips) {
  ConfigDocument document = ConfigDocument::FromBytes(Fixture());
  std::string error;
  const std::vector<std::string> titles = {
      "Roblox \"Beta\" \\ тест 🎮",
      "{place_name}",
      "# not a comment: and not a key",
      "  leading and trailing  ",
      "off",
      "tab\there",
      "",
  };
  for (const std::string& title : titles) {
    ASSERT_TRUE(document.Set("window.title", title, ScalarKind::kString,
                             &error))
        << title << ": " << error;
    EXPECT_EQ(document.Get("window.title"), title);
  }
  ASSERT_TRUE(document.Set("window.title", "Roblox \"Beta\" — тест 🎮",
                           ScalarKind::kString, &error))
      << error;
  const runtime::RuntimeConfigLoadResult loaded =
      LoadWithRealLoader(document.bytes());
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.window().title, "Roblox \"Beta\" — тест 🎮");

  // Characters libyaml does not accept raw are escaped and still read back.
  const std::string unusual = std::string("a\x01" "b\x7f" "c") +
                              "\xC2\x85" "d\xE2\x80\xA8" "e\xEF\xBB\xBF" "f" +
                              std::string(1, '\0') + "g\r\n";
  ASSERT_TRUE(document.Set("audio.output_device", unusual,
                           ScalarKind::kString, &error))
      << error;
  EXPECT_EQ(document.Get("audio.output_device"), unusual);
  EXPECT_EQ(document.Get("window.title"), "Roblox \"Beta\" — тест 🎮");

  const std::string before = document.bytes();
  EXPECT_FALSE(document.Set("window.title", "bad \xC3\x28 utf8",
                            ScalarKind::kString, &error));
  EXPECT_EQ(document.bytes(), before);
}

TEST(LauncherConfigDocumentTest, RejectsValuesThatDoNotMatchTheirKind) {
  const std::string original = Fixture();
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  EXPECT_FALSE(document.Set("window.high_dpi", "yes", ScalarKind::kBool,
                            &error));
  EXPECT_FALSE(document.Set("window.high_dpi", "True", ScalarKind::kBool,
                            &error));
  EXPECT_FALSE(document.Set("window.width", "1.5", ScalarKind::kInteger,
                            &error));
  EXPECT_FALSE(document.Set("window.width", "+5", ScalarKind::kInteger,
                            &error));
  EXPECT_FALSE(document.Set("window.width", "007", ScalarKind::kInteger,
                            &error));
  EXPECT_FALSE(document.Set("window.width", "", ScalarKind::kInteger, &error));
  EXPECT_FALSE(document.Set("graphics.vsync", "on # x", ScalarKind::kEnum,
                            &error));
  EXPECT_FALSE(document.Set("graphics.vsync", "-on", ScalarKind::kEnum,
                            &error));
  EXPECT_FALSE(document.Set("graphics.vsync", "", ScalarKind::kEnum, &error));
  EXPECT_FALSE(document.Set("graphics..vsync", "on", ScalarKind::kEnum,
                            &error));
  EXPECT_FALSE(document.Set("graphics.vsync key", "on", ScalarKind::kEnum,
                            &error));
  EXPECT_EQ(document.bytes(), original);

  ASSERT_TRUE(document.Set("graphics.frame_rate_limit", "-1",
                           ScalarKind::kInteger, &error))
      << error;
  EXPECT_EQ(document.Get("graphics.frame_rate_limit"), "-1");
}

TEST(LauncherConfigDocumentTest, UncommentsExamplesInsideTheirOwnSection) {
  // The real template has proxy_port twice: commented under network and
  // active under integrations.fleasion.
  const std::string original(runtime::DefaultRuntimeConfigYaml());
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Set("network.proxy_host", "127.0.0.1",
                           ScalarKind::kString, &error))
      << error;
  ASSERT_TRUE(document.Set("network.proxy_port", "8080", ScalarKind::kInteger,
                           &error))
      << error;
  LineDiff diff = Diff(original, document.bytes());
  EXPECT_EQ(diff.removed,
            (Lines{"  # proxy_host: 127.0.0.1", "  # proxy_port: 8080"}));
  EXPECT_EQ(diff.added,
            (Lines{"  proxy_host: \"127.0.0.1\"", "  proxy_port: 8080"}));
  EXPECT_EQ(LineNumberOf(document.bytes(), "  proxy_port: 8080"),
            LineNumberOf(original, "  # proxy_port: 8080"));
  EXPECT_EQ(document.Get("integrations.fleasion.proxy_port"), "58443");

  const std::string with_proxy = document.bytes();
  ASSERT_TRUE(document.Set("integrations.fleasion.proxy_port", "60000",
                           ScalarKind::kInteger, &error))
      << error;
  diff = Diff(with_proxy, document.bytes());
  EXPECT_EQ(diff.removed, (Lines{"    proxy_port: 58443"}));
  EXPECT_EQ(diff.added, (Lines{"    proxy_port: 60000"}));
  EXPECT_EQ(document.Get("network.proxy_port"), "8080");

  const runtime::RuntimeConfigLoadResult loaded =
      LoadWithRealLoader(document.bytes());
  ASSERT_TRUE(loaded) << loaded.error;
  ASSERT_TRUE(loaded.config.network_proxy().has_value());
  EXPECT_EQ(loaded.config.network_proxy()->port, 8080);
  EXPECT_EQ(loaded.config.fleasion_proxy_port(), 60000);
}

TEST(LauncherConfigDocumentTest, EveryTemplateExampleIsUncommentedInPlace) {
  const std::string original(runtime::DefaultRuntimeConfigYaml());
  const std::vector<std::string> examples = {
      "runtime.roblox_library",
      "graphics.frame_rate_limit",
      "graphics.vsync",
      "integrations.fleasion.ca_certificate",
      "integrations.discord_rpc.join.button_label",
      "integrations.discord_rpc.text.browsing",
      "integrations.discord_rpc.text.joining",
      "integrations.discord_rpc.text.playing",
      "integrations.discord_rpc.text.state",
      "integrations.discord_rpc.text.unknown_place",
      "integrations.discord_rpc.application_id",
      "network.proxy_host",
      "network.proxy_port",
      "network.ca_bundle",
  };
  for (const std::string& path : examples) {
    ConfigDocument document = ConfigDocument::FromBytes(original);
    std::string error;
    ASSERT_EQ(document.Get(path), std::nullopt) << path;
    ASSERT_TRUE(document.Set(path, "probe", ScalarKind::kString, &error))
        << path << ": " << error;
    EXPECT_EQ(document.Get(path), "probe") << path;
    const LineDiff diff = Diff(original, document.bytes());
    const std::size_t expected = path.find(".text.") == std::string::npos
                                     ? 1U
                                     : 2U;  // the text: header as well
    EXPECT_EQ(diff.removed.size(), expected) << path;
    EXPECT_EQ(diff.added.size(), expected) << path;
    EXPECT_EQ(SplitLines(document.bytes()).size(),
              SplitLines(original).size())
        << path;
    for (const std::string& removed : diff.removed) {
      EXPECT_NE(removed.find("# "), std::string::npos) << path;
    }
  }
}

TEST(LauncherConfigDocumentTest, EverySettingRoundTripsThroughUnset) {
  const std::string original = Fixture();
  const std::vector<std::string> settings = {
      "version",
      "device",
      "runtime.headless",
      "appearance.theme",
      "graphics.backend",
      "graphics.frame_rate_limit",
      "graphics.vsync",
      "performance.multithreaded_rendering",
      "performance.physics_worker_mode",
      "performance.memory_limit_mb",
      "performance.gamemode",
      "audio.output_device",
      "audio.input_device",
      "integrations.discord_rpc.enabled",
      "integrations.discord_rpc.show_place_name",
      "integrations.discord_rpc.show_elapsed_time",
      "integrations.discord_rpc.join.enabled",
      "integrations.discord_rpc.join.public_servers_only",
      "window.width",
      "window.height",
      "window.title",
      "window.high_dpi",
      "network.use_system_proxy",
      "updates.automatic",
      "updates.source",
      "updates.launch_after_update",
  };
  for (const std::string& path : settings) {
    ConfigDocument document = ConfigDocument::FromBytes(original);
    std::string error;
    ASSERT_TRUE(document.Get(path).has_value()) << path;
    ASSERT_TRUE(document.Set(path, "probe value", ScalarKind::kString, &error))
        << path << ": " << error;
    const LineDiff diff = Diff(original, document.bytes());
    EXPECT_EQ(diff.removed.size(), 1U) << path;
    EXPECT_EQ(diff.added.size(), 1U) << path;
    const std::string edited = document.bytes();

    ASSERT_TRUE(document.Unset(path, &error)) << path << ": " << error;
    EXPECT_EQ(document.Get(path), std::nullopt) << path;
    ASSERT_TRUE(document.Set(path, "probe value", ScalarKind::kString, &error))
        << path << ": " << error;
    EXPECT_EQ(document.bytes(), edited) << path;
  }
}

TEST(LauncherConfigDocumentTest, CommentedKeyOfAnotherSectionIsNotUsed) {
  // fleasion has a commented proxy_port, network has none: setting the
  // network port inserts a line into network instead.
  const std::string original =
      "version: 1\n"
      "integrations:\n"
      "  fleasion:\n"
      "    enabled: false\n"
      "    # proxy_port: 1234\n"
      "network:\n"
      "  use_system_proxy: false\n"
      "  proxy_host: localhost\n"
      "updates:\n"
      "  automatic: true\n";
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Set("network.proxy_port", "3128", ScalarKind::kInteger,
                           &error))
      << error;
  EXPECT_EQ(document.bytes(),
            "version: 1\n"
            "integrations:\n"
            "  fleasion:\n"
            "    enabled: false\n"
            "    # proxy_port: 1234\n"
            "network:\n"
            "  use_system_proxy: false\n"
            "  proxy_host: localhost\n"
            "  proxy_port: 3128\n"
            "updates:\n"
            "  automatic: true\n");
  ASSERT_TRUE(document.Validate(&error)) << error;
}

TEST(LauncherConfigDocumentTest, UncommentsNestedDiscordExamples) {
  const std::string original = Fixture();
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  // The template's own example value has a space, so it qualifies even
  // though it is not a single token.
  ASSERT_TRUE(document.Set("integrations.discord_rpc.join.button_label",
                           "Зайти", ScalarKind::kString, &error))
      << error;
  ASSERT_TRUE(document.Set("integrations.discord_rpc.text.playing",
                           "{place_name} — играю", ScalarKind::kString,
                           &error))
      << error;
  ASSERT_TRUE(document.Set("integrations.discord_rpc.application_id",
                           "123456789012345678", ScalarKind::kInteger, &error))
      << error;
  const LineDiff diff = Diff(original, document.bytes());
  EXPECT_EQ(diff.removed,
            (Lines{"      # button_label: Join Server", "    # text:",
                   "    #   playing: \"{place_name}\"",
                   "    # application_id: 123456789012345678"}));
  EXPECT_EQ(diff.added,
            (Lines{"      button_label: \"Зайти\"", "    text:",
                   "      playing: \"{place_name} — играю\"",
                   "    application_id: 123456789012345678"}));
  EXPECT_EQ(document.Get("integrations.discord_rpc.text.playing"),
            "{place_name} — играю");
  EXPECT_EQ(document.Get("integrations.discord_rpc.text.browsing"),
            std::nullopt);

  const runtime::RuntimeConfigLoadResult loaded =
      LoadWithRealLoader(document.bytes());
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.discord_rpc().join_button_label, "Зайти");
  EXPECT_EQ(loaded.config.discord_rpc().text.playing,
            "{place_name} — играю");

  // A second text key goes into the now active text: block.
  ASSERT_TRUE(document.Set("integrations.discord_rpc.text.state", "Играю",
                           ScalarKind::kString, &error))
      << error;
  EXPECT_NE(document.bytes().find("      state: \"Играю\"\n"),
            std::string::npos);
  ASSERT_TRUE(document.Validate(&error)) << error;

  // Removing both text keys comments the emptied text: header out again.
  ASSERT_TRUE(document.Unset("integrations.discord_rpc.text.state", &error))
      << error;
  ASSERT_TRUE(document.Unset("integrations.discord_rpc.text.playing", &error))
      << error;
  EXPECT_FALSE(document.IsMapping("integrations.discord_rpc.text"));
  EXPECT_NE(document.bytes().find("    # text:\n"), std::string::npos);
  ASSERT_TRUE(document.Validate(&error)) << error;
}

TEST(LauncherConfigDocumentTest, LeavesProseCommentsAlone) {
  const std::string original =
      "version: 1\n"
      "graphics:\n"
      "  backend: direct-vulkan\n"
      "  # vsync: выключено — так меньше задержка\n"
      "  # frame_rate_limit: 144 hz panel, see notes\n"
      "updates:\n"
      "  automatic: true\n";
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Set("graphics.vsync", "on", ScalarKind::kEnum, &error))
      << error;
  ASSERT_TRUE(document.Set("graphics.frame_rate_limit", "60",
                           ScalarKind::kInteger, &error))
      << error;
  EXPECT_EQ(document.bytes(),
            "version: 1\n"
            "graphics:\n"
            "  backend: direct-vulkan\n"
            "  vsync: on\n"
            "  frame_rate_limit: 60\n"
            "  # vsync: выключено — так меньше задержка\n"
            "  # frame_rate_limit: 144 hz panel, see notes\n"
            "updates:\n"
            "  automatic: true\n");

  // A one-token commented value is an example and is reused; its inline
  // note stays.
  ConfigDocument example = ConfigDocument::FromBytes(
      "graphics:\n"
      "  backend: direct-vulkan\n"
      "  # vsync: off  # пробовал, рвётся картинка\n");
  ASSERT_TRUE(example.Set("graphics.vsync", "on", ScalarKind::kEnum, &error))
      << error;
  EXPECT_EQ(example.bytes(),
            "graphics:\n"
            "  backend: direct-vulkan\n"
            "  vsync: on  # пробовал, рвётся картинка\n");
}

TEST(LauncherConfigDocumentTest, InsertsWithTheSiblingsIndentation) {
  const std::string original =
      "version: 1\n"
      "graphics:\n"
      "    backend: direct-vulkan\n"
      "    # A note that belongs to graphics.\n"
      "\n"
      "integrations:\n"
      "    discord_rpc:\n"
      "        enabled: false\n"
      "        join:\n"
      "            enabled: true\n"
      "            # button_label: Join Server\n"
      "        # application_id: 123456789012345678\n"
      "window:\n"
      "    width: 1280\n";
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Set("graphics.vsync", "off", ScalarKind::kEnum, &error))
      << error;
  ASSERT_TRUE(document.Set("integrations.discord_rpc.show_place_name",
                           "false", ScalarKind::kBool, &error))
      << error;
  ASSERT_TRUE(document.Set("integrations.discord_rpc.text.state", "x",
                           ScalarKind::kString, &error))
      << error;
  ASSERT_TRUE(document.Set("window.high_dpi", "true", ScalarKind::kBool,
                           &error))
      << error;
  EXPECT_EQ(document.bytes(),
            "version: 1\n"
            "graphics:\n"
            "    backend: direct-vulkan\n"
            "    vsync: off\n"
            "    # A note that belongs to graphics.\n"
            "\n"
            "integrations:\n"
            "    discord_rpc:\n"
            "        enabled: false\n"
            "        join:\n"
            "            enabled: true\n"
            "            # button_label: Join Server\n"
            "        show_place_name: false\n"
            "        text:\n"
            "            state: \"x\"\n"
            "        # application_id: 123456789012345678\n"
            "window:\n"
            "    width: 1280\n"
            "    high_dpi: true\n");
  // The nested example under join still belongs to join.
  ASSERT_TRUE(document.Set("integrations.discord_rpc.join.button_label", "Go",
                           ScalarKind::kString, &error))
      << error;
  EXPECT_NE(document.bytes().find("            button_label: \"Go\"\n"),
            std::string::npos);
  ASSERT_TRUE(document.Validate(&error)) << error;
}

TEST(LauncherConfigDocumentTest, CopiesAMissingNestedBlockFromTheTemplate) {
  // The fixture predates integrations.fleasion.
  const std::string original = Fixture();
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Set("integrations.fleasion.enabled", "true",
                           ScalarKind::kBool, &error))
      << error;
  const LineDiff diff = Diff(original, document.bytes());
  EXPECT_TRUE(diff.removed.empty());
  EXPECT_EQ(diff.added,
            (Lines{"  fleasion:",
                   "    # Boolean (default: false): trust Fleasion's CA "
                   "without editing Roblox files.",
                   "    enabled: true",
                   "    # String: match Fleasion's routing mode: env or "
                   "hosts.",
                   "    proxy_mode: env",
                   "    # Integer: Fleasion's local HTTP proxy port in env "
                   "mode.",
                   "    proxy_port: 58443",
                   "    # Optional absolute public CA path. Default: "
                   "$XDG_CONFIG_HOME/Fleasion/proxy_ca/ca.crt",
                   "    # or ~/.config/Fleasion/proxy_ca/ca.crt. Never select "
                   "ca.key.",
                   "    # ca_certificate: "
                   "/home/user/.config/Fleasion/proxy_ca/ca.crt"}));
  // Template order: fleasion comes before discord_rpc.
  EXPECT_LT(document.bytes().find("  fleasion:\n"),
            document.bytes().find("  discord_rpc:\n"));
  EXPECT_EQ(document.Get("integrations.fleasion.proxy_mode"), "env");
  const runtime::RuntimeConfigLoadResult loaded =
      LoadWithRealLoader(document.bytes());
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.config.fleasion_enabled());

  // A key the template only shows commented is uncommented in the copy.
  ASSERT_TRUE(document.Set("integrations.fleasion.ca_certificate",
                           "/srv/ca.crt", ScalarKind::kString, &error))
      << error;
  EXPECT_NE(document.bytes().find("    ca_certificate: \"/srv/ca.crt\"\n"),
            std::string::npos);
  ASSERT_TRUE(document.Validate(&error)) << error;
}

// A hand-written first-run template with the shipped template's section
// order but only keys the loader knows today, so these tests do not change
// whenever the shipped template gains sections.
std::string FixtureTemplate() {
  return ReadFile(std::filesystem::path(MOCKTAIL_TEST_SOURCE_DIR) /
                  "tests/fixtures/launcher/template_config.yaml");
}

// text without [from, to): drops sections to make an older-looking file.
std::string WithoutRange(std::string text, std::string_view from,
                         std::string_view to) {
  const std::size_t begin = text.find(from);
  const std::size_t end = to.empty() ? text.size() : text.find(to, begin);
  if (begin == std::string::npos || end == std::string::npos) {
    ADD_FAILURE() << "fixture text not found: " << from;
    return text;
  }
  text.erase(begin, end - begin);
  return text;
}

TEST(LauncherConfigDocumentTest, AppendsAMissingSectionAtItsTemplatePosition) {
  const std::string original =
      WithoutRange(Fixture(), "audio:\n", "integrations:\n");
  ConfigDocument document = ConfigDocument::FromBytes(original);
  document.SetTemplate(FixtureTemplate());
  std::string error;
  ASSERT_EQ(document.Get("audio.output_device"), std::nullopt);
  ASSERT_TRUE(document.Set("audio.output_device", "Встроенный звук",
                           ScalarKind::kString, &error))
      << error;
  const LineDiff diff = Diff(original, document.bytes());
  EXPECT_TRUE(diff.removed.empty());
  EXPECT_EQ(diff.added,
            (Lines{"# Sound devices.", "audio:",
                   "  # String (default: default): output device name "
                   "printed during startup.",
                   "  output_device: \"Встроенный звук\"",
                   "  # String (default: default): input device name, "
                   "id:<number>, or disabled.",
                   "  # input_device: disabled", ""}));
  // Template order: after performance, before integrations.
  EXPECT_NE(document.bytes().find("  gamemode: on\n"
                                  "\n"
                                  "# Sound devices.\n"
                                  "audio:\n"),
            std::string::npos)
      << document.bytes();
  EXPECT_NE(document.bytes().find("  # input_device: disabled\n"
                                  "\n"
                                  "integrations:\n"),
            std::string::npos)
      << document.bytes();

  // The copied section's commented example is used for the next key.
  ASSERT_TRUE(document.Set("audio.input_device", "disabled", ScalarKind::kEnum,
                           &error))
      << error;
  EXPECT_NE(document.bytes().find("  input_device: disabled\n"
                                  "\n"
                                  "integrations:\n"),
            std::string::npos)
      << document.bytes();
  ASSERT_TRUE(document.Validate(&error)) << error;
  const runtime::RuntimeConfigLoadResult loaded =
      LoadWithRealLoader(document.bytes());
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.audio_output_device(), "Встроенный звук");
  EXPECT_EQ(loaded.config.audio_input_device(), "disabled");
}

TEST(LauncherConfigDocumentTest, KeepsTemplateOrderForSeveralNewSections) {
  const std::string original =
      WithoutRange(Fixture(), "performance:\n", "integrations:\n");
  ConfigDocument document = ConfigDocument::FromBytes(original);
  document.SetTemplate(FixtureTemplate());
  std::string error;
  // The later section first: the earlier one must still go above it.
  ASSERT_TRUE(document.Set("audio.input_device", "disabled", ScalarKind::kEnum,
                           &error))
      << error;
  ASSERT_TRUE(document.Set("performance.gamemode", "on", ScalarKind::kEnum,
                           &error))
      << error;
  EXPECT_NE(
      document.bytes().find(
          "  vsync: off\n"
          "\n"
          "performance:\n"
          "  # Boolean (default: false): size Roblox queues from every "
          "physical core.\n"
          "  multithreaded_rendering: false\n"
          "  # String (default: auto): request Feral GameMode: auto, on, or "
          "off.\n"
          "  gamemode: on\n"
          "\n"
          "# Sound devices.\n"
          "audio:\n"
          "  # String (default: default): output device name printed during "
          "startup.\n"
          "  output_device: default\n"
          "  # String (default: default): input device name, id:<number>, or "
          "disabled.\n"
          "  input_device: disabled\n"
          "\n"
          "integrations:\n"),
      std::string::npos)
      << document.bytes();
  EXPECT_TRUE(Diff(original, document.bytes()).removed.empty());
  ASSERT_TRUE(document.Validate(&error)) << error;
  const runtime::RuntimeConfigLoadResult loaded =
      LoadWithRealLoader(document.bytes());
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.performance().game_mode, runtime::GameModePolicy::kOn);
  EXPECT_EQ(loaded.config.audio_input_device(), "disabled");
}

TEST(LauncherConfigDocumentTest, AppendsATrailingTemplateSectionAtTheEnd) {
  // The fixture ends with updates: and the note above it.
  const std::string original =
      WithoutRange(Fixture(), "\n# Обновления", std::string_view());
  ConfigDocument document = ConfigDocument::FromBytes(original);
  document.SetTemplate(FixtureTemplate());
  std::string error;
  ASSERT_TRUE(document.Set("updates.automatic", "false", ScalarKind::kBool,
                           &error))
      << error;
  EXPECT_EQ(document.bytes(),
            original +
                "\n"
                "# Roblox updates.\n"
                "updates:\n"
                "  # Boolean (default: true): install verified Roblox updates "
                "automatically.\n"
                "  automatic: false\n"
                "  # String (default: apk-pure): APK provider. Supported "
                "values: auto, apk-pure.\n"
                "  # source: auto\n");
  ASSERT_TRUE(document.Validate(&error)) << error;

  // A section the template does not know is still written, plainly.
  ConfigDocument plain = ConfigDocument::FromBytes("version: 1\nupdates:\n"
                                                   "  automatic: true");
  ASSERT_TRUE(plain.Set("input.touch_enabled", "true", ScalarKind::kBool,
                        &error))
      << error;
  EXPECT_EQ(plain.bytes(),
            "version: 1\nupdates:\n  automatic: true\n\ninput:\n"
            "  touch_enabled: true\n");
}

TEST(LauncherConfigDocumentTest, CopiesAMissingSectionFromTheShippedTemplate) {
  // By default sections come from DefaultRuntimeConfigYaml(). Only the shape
  // is checked here, so the template's wording can change freely.
  const std::string original =
      WithoutRange(Fixture(), "network:\n", "# Обновления");
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Set("network.use_system_proxy", "true",
                           ScalarKind::kBool, &error))
      << error;
  const LineDiff diff = Diff(original, document.bytes());
  EXPECT_TRUE(diff.removed.empty());
  const std::vector<std::string> shipped =
      SplitLines(std::string(runtime::DefaultRuntimeConfigYaml()));
  bool header = false;
  bool value = false;
  for (const std::string& line : diff.added) {
    if (line == "network:") {
      header = true;
    }
    if (line == "  use_system_proxy: true") {
      value = true;
      continue;
    }
    EXPECT_NE(std::find(shipped.begin(), shipped.end(), line), shipped.end())
        << "not a template line: " << line;
  }
  EXPECT_TRUE(header);
  EXPECT_TRUE(value);
  EXPECT_GT(diff.added.size(), 3U);  // the template's comments came along
  // Template order: network sits before updates and the note above it.
  EXPECT_LT(document.bytes().find("\nnetwork:\n"),
            document.bytes().find("# Обновления"));
  EXPECT_GT(document.bytes().find("\nnetwork:\n"),
            document.bytes().find("  high_dpi: true\n"));
  ASSERT_TRUE(document.Validate(&error)) << error;
  const runtime::RuntimeConfigLoadResult loaded =
      LoadWithRealLoader(document.bytes());
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.config.use_system_proxy());
}

// The shipped template's top-level `name:` section, from its header to the
// blank line that ends it.
std::string ShippedTemplateSection(std::string_view name) {
  const std::string shipped(runtime::DefaultRuntimeConfigYaml());
  const std::string header = "\n" + std::string(name) + ":\n";
  const std::size_t begin = shipped.find(header);
  const std::size_t end =
      begin == std::string::npos ? begin : shipped.find("\n\n", begin + 1);
  if (end == std::string::npos) {
    ADD_FAILURE() << "shipped template has no section " << name;
    return std::string();
  }
  return shipped.substr(begin + 1, end - begin);
}

std::string ReplacedOnce(std::string text, std::string_view from,
                         std::string_view to) {
  const std::size_t at = text.find(from);
  if (at == std::string::npos) {
    ADD_FAILURE() << "text not found: " << from;
    return text;
  }
  return text.replace(at, from.size(), to);
}

TEST(LauncherConfigDocumentTest, AddsTheLauncherSectionsToAnOlderConfig) {
  // The fixture was written before display:, account:, engine: and
  // launcher: existed. The settings window sets two of them, and the new
  // sections come from the shipped template.
  const std::string original = Fixture();
  for (const char* section :
       {"\ndisplay:", "\naccount:", "\nengine:", "\nlauncher:"}) {
    ASSERT_EQ(original.find(section), std::string::npos) << section;
  }
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Set("display.server", "wayland", ScalarKind::kEnum,
                           &error))
      << error;
  ASSERT_TRUE(document.Set("account.sign_in", "browser", ScalarKind::kEnum,
                           &error))
      << error;

  // Each new section is the template's, comments included, with only the
  // edited value changed.
  const std::string& bytes = document.bytes();
  const std::string display = ReplacedOnce(ShippedTemplateSection("display"),
                                           "  server: auto\n",
                                           "  server: wayland\n");
  const std::string account = ReplacedOnce(ShippedTemplateSection("account"),
                                           "  sign_in: native\n",
                                           "  sign_in: browser\n");
  EXPECT_NE(display.find("\n  # "), std::string::npos) << display;
  EXPECT_NE(account.find("\n  # "), std::string::npos) << account;
  EXPECT_NE(bytes.find("\n\n" + display + "\n"), std::string::npos) << bytes;
  EXPECT_NE(bytes.find("\n\n" + account + "\n"), std::string::npos) << bytes;

  // Nothing of the user's file is lost, and the two sections it did not
  // need stay out.
  const LineDiff diff = Diff(original, bytes);
  EXPECT_TRUE(diff.removed.empty());
  const std::vector<std::string> shipped =
      SplitLines(std::string(runtime::DefaultRuntimeConfigYaml()));
  for (const std::string& line : diff.added) {
    if (line == "  server: wayland" || line == "  sign_in: browser") {
      continue;
    }
    EXPECT_NE(std::find(shipped.begin(), shipped.end(), line), shipped.end())
        << "not a template line: " << line;
  }
  EXPECT_EQ(bytes.find("\nengine:"), std::string::npos);
  EXPECT_EQ(bytes.find("\nlauncher:"), std::string::npos);

  // Template order: window, display, account, then network.
  const std::size_t window = bytes.find("\nwindow:\n");
  const std::size_t display_at = bytes.find("\ndisplay:\n");
  const std::size_t account_at = bytes.find("\naccount:\n");
  const std::size_t network = bytes.find("\nnetwork:\n");
  ASSERT_NE(window, std::string::npos);
  ASSERT_NE(network, std::string::npos);
  EXPECT_LT(window, display_at);
  EXPECT_LT(display_at, account_at);
  EXPECT_LT(account_at, network);

  // The runtime's own loader accepts the result and reads both settings,
  // and the user's values elsewhere are untouched.
  ASSERT_TRUE(document.Validate(&error)) << error;
  const runtime::RuntimeConfigLoadResult loaded = LoadWithRealLoader(bytes);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.display().server, runtime::DisplayServer::kWayland);
  EXPECT_TRUE(loaded.config.display().server_valid);
  EXPECT_EQ(loaded.config.display().start_mode,
            runtime::WindowStartMode::kRemember);
  EXPECT_EQ(loaded.config.account().sign_in, runtime::SignInMethod::kBrowser);
  EXPECT_EQ(loaded.config.engine().graphics_quality,
            runtime::GraphicsQuality{});
  EXPECT_TRUE(loaded.config.launcher().show_on_start);
  const runtime::RuntimeConfigLoadResult before = LoadWithRealLoader(original);
  ASSERT_TRUE(before) << before.error;
  EXPECT_EQ(before.config.display().server, runtime::DisplayServer::kAuto);
  EXPECT_EQ(before.config.account().sign_in, runtime::SignInMethod::kNative);
  EXPECT_EQ(loaded.config.window().width, before.config.window().width);
  EXPECT_EQ(loaded.config.window().height, before.config.window().height);
  EXPECT_EQ(loaded.config.window().high_dpi, before.config.window().high_dpi);
  EXPECT_EQ(loaded.config.performance().game_mode,
            before.config.performance().game_mode);
}

// A config.yaml written before engine.gpu and engine.nvidia_shader_mt has
// an engine: section with graphics_quality alone. The new keys go in below
// it, indented like it, and the runtime's loader reads them.
TEST(LauncherConfigDocumentTest, AddsTheNewEngineKeysToAnOlderEngineSection) {
  const std::string original =
      Fixture() + "engine:\n  # Mine.\n  graphics_quality: 5\n";
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(
      document.Set("engine.gpu", "integrated", ScalarKind::kEnum, &error))
      << error;
  ASSERT_TRUE(document.Set("engine.nvidia_shader_mt", "false",
                           ScalarKind::kBool, &error))
      << error;
  const std::string& bytes = document.bytes();
  EXPECT_NE(bytes.find("\nengine:\n  # Mine.\n  graphics_quality: 5\n"
                       "  gpu: integrated\n  nvidia_shader_mt: false\n"),
            std::string::npos)
      << bytes;
  const LineDiff diff = Diff(original, bytes);
  EXPECT_TRUE(diff.removed.empty());
  EXPECT_EQ(diff.added.size(), 2U);

  ASSERT_TRUE(document.Validate(&error)) << error;
  const runtime::RuntimeConfigLoadResult loaded = LoadWithRealLoader(bytes);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(
      loaded.config.engine().graphics_quality,
      (runtime::GraphicsQuality{runtime::GraphicsQualityMode::kLevel, 5}));
  EXPECT_EQ(loaded.config.engine().gpu, runtime::GpuPreference::kIntegrated);
  EXPECT_FALSE(loaded.config.engine().nvidia_shader_mt);

  // The first-run template has both, and they are edited where they are.
  const std::string shipped(runtime::DefaultRuntimeConfigYaml());
  ConfigDocument fresh = ConfigDocument::FromBytes(shipped);
  EXPECT_EQ(fresh.Get("engine.gpu"), "auto");
  EXPECT_EQ(fresh.Get("engine.nvidia_shader_mt"), "true");
  ASSERT_TRUE(fresh.Set("engine.gpu", "discrete", ScalarKind::kEnum, &error))
      << error;
  ASSERT_TRUE(
      fresh.Set("engine.nvidia_shader_mt", "false", ScalarKind::kBool, &error))
      << error;
  const LineDiff fresh_diff = Diff(shipped, fresh.bytes());
  EXPECT_EQ(
      fresh_diff.removed,
      (std::vector<std::string>{"  gpu: auto", "  nvidia_shader_mt: true"}));
  EXPECT_EQ(fresh_diff.added,
            (std::vector<std::string>{"  gpu: discrete",
                                      "  nvidia_shader_mt: false"}));
}

TEST(LauncherConfigDocumentTest, NeverLeavesAnEmptySectionHeader) {
  const std::string original = Fixture();
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  // theme is the only value of appearance: the header goes with it.
  ASSERT_TRUE(document.Unset("appearance.theme", &error)) << error;
  LineDiff diff = Diff(original, document.bytes());
  EXPECT_EQ(diff.removed, (Lines{"appearance:", "  theme: roblox"}));
  EXPECT_EQ(diff.added, (Lines{"# appearance:", "  # theme: roblox"}));
  EXPECT_EQ(document.Get("appearance.theme"), std::nullopt);
  ASSERT_TRUE(document.Validate(&error)) << error;

  // Setting it again brings both lines back where they were.
  ASSERT_TRUE(document.Set("appearance.theme", "dark", ScalarKind::kEnum,
                           &error))
      << error;
  diff = Diff(original, document.bytes());
  EXPECT_EQ(diff.removed, (Lines{"  theme: roblox"}));
  EXPECT_EQ(diff.added, (Lines{"  theme: dark"}));

  // An existing empty header (fatal for the loader) gets its value.
  ConfigDocument empty_header = ConfigDocument::FromBytes(
      "version: 1\n"
      "window:\n"
      "graphics:\n"
      "  backend: opengl\n");
  EXPECT_FALSE(empty_header.Validate(&error));
  EXPECT_NE(error.find("window must be a mapping (line 2)"), std::string::npos)
      << error;
  ASSERT_TRUE(empty_header.Set("window.width", "1600", ScalarKind::kInteger,
                               &error))
      << error;
  EXPECT_EQ(empty_header.bytes(),
            "version: 1\n"
            "window:\n"
            "  width: 1600\n"
            "graphics:\n"
            "  backend: opengl\n");
  ASSERT_TRUE(empty_header.Validate(&error)) << error;

  // Removing the last value of the whole file is refused.
  ConfigDocument single = ConfigDocument::FromBytes("version: 1\n");
  EXPECT_FALSE(single.Unset("version", &error));
  EXPECT_EQ(single.bytes(), "version: 1\n");
}

TEST(LauncherConfigDocumentTest, UnsetCommentsTheLineOut) {
  const std::string original = Fixture();
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Unset("graphics.frame_rate_limit", &error)) << error;
  const LineDiff diff = Diff(original, document.bytes());
  EXPECT_EQ(diff.removed, (Lines{"  frame_rate_limit: 165"}));
  EXPECT_EQ(diff.added, (Lines{"  # frame_rate_limit: 165"}));
  EXPECT_EQ(document.Get("graphics.frame_rate_limit"), std::nullopt);
  const runtime::RuntimeConfigLoadResult loaded =
      LoadWithRealLoader(document.bytes());
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.frame_rate().mode,
            runtime::FrameRateLimitMode::kUnmanaged);

  // Absent keys are already unset; sections are not single values.
  const std::string unchanged = document.bytes();
  EXPECT_TRUE(document.Unset("network.proxy_port", &error)) << error;
  EXPECT_TRUE(document.Unset("integrations.fleasion.enabled", &error));
  EXPECT_FALSE(document.Unset("graphics", &error));
  EXPECT_EQ(document.bytes(), unchanged);

  // And it can be set again from its own commented line.
  ASSERT_TRUE(document.Set("graphics.frame_rate_limit", "120",
                           ScalarKind::kInteger, &error))
      << error;
  EXPECT_EQ(LineNumberOf(document.bytes(), "  frame_rate_limit: 120"),
            LineNumberOf(original, "  frame_rate_limit: 165"));
}

TEST(LauncherConfigDocumentTest, RefusesToReplaceADeviceMappingUnlessForced) {
  std::string text = Fixture();
  const std::string scalar_line = "device: pc-windows-11\n";
  text.erase(text.find(scalar_line), scalar_line.size());
  const std::string detailed = "# device:\n#   type: mobile\n";
  text.replace(text.find(detailed), detailed.size(),
               "device:\n  type: mobile\n  # A note inside the mapping.\n");
  const std::string original = text;
  ConfigDocument document = ConfigDocument::FromBytes(original);
  std::string error;
  ASSERT_TRUE(document.Validate(&error)) << error;
  EXPECT_TRUE(document.IsMapping("device"));
  EXPECT_EQ(document.Get("device.type"), "mobile");

  EXPECT_FALSE(document.Set("device", "pc-windows-11", ScalarKind::kEnum,
                            &error));
  EXPECT_NE(error.find("mapping"), std::string::npos) << error;
  EXPECT_EQ(document.bytes(), original);

  // The detailed fields themselves stay editable.
  ASSERT_TRUE(document.Set("device.type", "console", ScalarKind::kEnum,
                           &error))
      << error;
  ASSERT_TRUE(document.Set("device", "pc-windows-11", ScalarKind::kEnum,
                           &error, true))
      << error;
  const LineDiff diff = Diff(original, document.bytes());
  EXPECT_EQ(diff.removed, (Lines{"device:", "  type: mobile"}));
  EXPECT_EQ(diff.added,
            (Lines{"device: pc-windows-11", "#   type: console"}));
  EXPECT_FALSE(document.IsMapping("device"));
  EXPECT_EQ(document.Get("device"), "pc-windows-11");
  ASSERT_TRUE(document.Validate(&error)) << error;
}

TEST(LauncherConfigDocumentTest, RefusesAmbiguousOrUnsupportedEdits) {
  std::string error;
  ConfigDocument duplicate = ConfigDocument::FromBytes(
      "graphics:\n  vsync: on\n  vsync: off\n");
  EXPECT_FALSE(duplicate.Set("graphics.vsync", "auto", ScalarKind::kEnum,
                             &error));
  EXPECT_NE(error.find("more than once"), std::string::npos) << error;

  ConfigDocument multiline = ConfigDocument::FromBytes(
      "window:\n  title: |\n    Roblox\n    Player\n");
  EXPECT_FALSE(multiline.Set("window.title", "x", ScalarKind::kString,
                             &error));
  EXPECT_NE(error.find("several lines"), std::string::npos) << error;

  ConfigDocument scalar_parent = ConfigDocument::FromBytes(
      "device: pc\nwindow:\n  width: 1280\n");
  EXPECT_FALSE(scalar_parent.Set("device.type", "mobile", ScalarKind::kEnum,
                                 &error));

  ConfigDocument broken = ConfigDocument::FromBytes(
      "window:\n  width: 1280\n   height: [\n");
  EXPECT_FALSE(broken.Set("window.width", "1600", ScalarKind::kInteger,
                          &error));
  EXPECT_NE(error.find("line"), std::string::npos) << error;
  EXPECT_EQ(broken.Get("window.width"), std::nullopt);

  ConfigDocument not_a_mapping = ConfigDocument::FromBytes("# only notes\n");
  EXPECT_FALSE(not_a_mapping.Set("window.width", "1600",
                                 ScalarKind::kInteger, &error));
}

TEST(LauncherConfigDocumentTest, KeepsWindowsLineEndings) {
  ConfigDocument document = ConfigDocument::FromBytes(
      "version: 1\r\n"
      "graphics:\r\n"
      "  backend: direct-vulkan\r\n"
      "  # vsync: off\r\n"
      "window:\r\n"
      "  width: 1280\r\n");
  std::string error;
  ASSERT_TRUE(document.Set("graphics.vsync", "on", ScalarKind::kEnum, &error))
      << error;
  ASSERT_TRUE(document.Set("window.height", "720", ScalarKind::kInteger,
                           &error))
      << error;
  ASSERT_TRUE(document.Set("window.width", "1600", ScalarKind::kInteger,
                           &error))
      << error;
  EXPECT_EQ(document.bytes(),
            "version: 1\r\n"
            "graphics:\r\n"
            "  backend: direct-vulkan\r\n"
            "  vsync: on\r\n"
            "window:\r\n"
            "  width: 1600\r\n"
            "  height: 720\r\n");
}

TEST(LauncherConfigDocumentTest, ValidateReportsLoaderErrorsWithLines) {
  std::string error;
  ConfigDocument bad_value = ConfigDocument::FromBytes(
      "version: 1\n"
      "# Заметка\n"
      "graphics:\n"
      "  backend: direct-vulkan\n"
      "  vsync: false\n");
  EXPECT_FALSE(bad_value.Validate(&error));
  EXPECT_EQ(error, "graphics.vsync must be auto, on, or off (line 5)");

  ConfigDocument unknown_key = ConfigDocument::FromBytes(
      "version: 1\nwindow:\n  width: 1280\n  video_driver: wayland\n");
  EXPECT_FALSE(unknown_key.Validate(&error));
  EXPECT_EQ(error,
            "unknown runtime configuration key: window.video_driver (line 4)");

  ConfigDocument unknown_update = ConfigDocument::FromBytes(
      "version: 1\nupdates:\n  automatic: true\n  channel: beta\n");
  EXPECT_FALSE(unknown_update.Validate(&error));
  EXPECT_EQ(error, "unknown updates key: channel (line 4)");

  ConfigDocument syntax = ConfigDocument::FromBytes(
      "version: 1\n# Комментарий\nwindow:\n  width: 1280\n height: 720\n");
  EXPECT_FALSE(syntax.Validate(&error));
  EXPECT_EQ(error.rfind("invalid YAML at line 5", 0), 0U) << error;

  ConfigDocument semantic = ConfigDocument::FromBytes(
      "version: 1\nnetwork:\n  proxy_host: 127.0.0.1\n");
  EXPECT_FALSE(semantic.Validate(&error));
  EXPECT_NE(error.find("network"), std::string::npos) << error;
  EXPECT_EQ(error.find("/tmp"), std::string::npos) << error;

  // Unknown top-level sections are ignored by this loader, as at startup.
  ConfigDocument future = ConfigDocument::FromBytes(
      "version: 1\nsome_future_section:\n  some_key: true\n");
  EXPECT_TRUE(future.Validate(&error)) << error;
}

class LauncherConfigDocumentFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_FALSE(temporary_.path().empty());
    file_ = temporary_.path() / "config.yaml";
  }

  TemporaryDirectory temporary_;
  std::filesystem::path file_;
};

TEST_F(LauncherConfigDocumentFileTest, SavesPrivatelyAndKeepsOneBackup) {
  const std::string original = Fixture();
  WriteFile(file_, original, 0644);
  ConfigDocument document;
  std::string error;
  ASSERT_TRUE(ConfigDocument::Load(file_, &document, &error)) << error;
  EXPECT_TRUE(document.identity().exists);
  EXPECT_FALSE(document.HasUnsavedChanges());
  ASSERT_TRUE(document.Set("graphics.vsync", "on", ScalarKind::kEnum, &error))
      << error;
  EXPECT_TRUE(document.HasUnsavedChanges());

  const mode_t previous_umask = umask(0);
  const bool saved = document.Save(file_, &error);
  umask(previous_umask);
  ASSERT_TRUE(saved) << error;
  EXPECT_FALSE(document.HasUnsavedChanges());
  EXPECT_EQ(ReadFile(file_), document.bytes());
  EXPECT_EQ(FileMode(file_), 0600);
  const std::filesystem::path backup = ConfigDocument::BackupPath(file_);
  EXPECT_EQ(backup.filename(), "config.yaml.launcher-backup");
  EXPECT_EQ(ReadFile(backup), original);
  EXPECT_EQ(FileMode(backup), 0600);

  // The next save neither needs a reload nor touches the backup.
  ASSERT_TRUE(document.Set("window.width", "1920", ScalarKind::kInteger,
                           &error))
      << error;
  ASSERT_TRUE(document.Save(file_, &error)) << error;
  EXPECT_EQ(ReadFile(file_), document.bytes());
  EXPECT_EQ(ReadFile(backup), original);

  // No temporary files are left behind.
  std::size_t entries = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(temporary_.path())) {
    (void)entry;
    ++entries;
  }
  EXPECT_EQ(entries, 2U);

  // Restoring the backup goes through the same checked save.
  ConfigDocument restore;
  ASSERT_TRUE(ConfigDocument::Load(file_, &restore, &error)) << error;
  restore.ReplaceAll(ReadFile(backup));
  ASSERT_TRUE(restore.Save(file_, &error)) << error;
  EXPECT_EQ(ReadFile(file_), original);
}

TEST_F(LauncherConfigDocumentFileTest, RefusesFilesChangedSinceLoading) {
  const std::string original = Fixture();
  WriteFile(file_, original);
  ConfigDocument document;
  std::string error;
  ASSERT_TRUE(ConfigDocument::Load(file_, &document, &error)) << error;
  ASSERT_TRUE(document.Set("graphics.vsync", "on", ScalarKind::kEnum, &error))
      << error;

  // A hand edit of the same size, made right after loading.
  std::string edited = original;
  edited.replace(edited.find("gamemode: on"), 12, "gamemode: no");
  WriteFile(file_, edited);
  EXPECT_FALSE(document.Save(file_, &error));
  EXPECT_NE(error.find("changed on disk"), std::string::npos) << error;
  EXPECT_EQ(ReadFile(file_), edited);
  EXPECT_FALSE(std::filesystem::exists(ConfigDocument::BackupPath(file_)));

  // A replaced file (new inode) is refused too.
  ConfigDocument second;
  ASSERT_TRUE(ConfigDocument::Load(file_, &second, &error)) << error;
  const std::filesystem::path replacement = temporary_.path() / "other.yaml";
  WriteFile(replacement, edited);
  ASSERT_EQ(rename(replacement.c_str(), file_.c_str()), 0);
  EXPECT_FALSE(second.Save(file_, &error));

  // A file that appears after a missing-file load is not clobbered.
  const std::filesystem::path missing = temporary_.path() / "new.yaml";
  ConfigDocument fresh;
  ASSERT_TRUE(ConfigDocument::Load(missing, &fresh, &error)) << error;
  WriteFile(missing, "version: 1\n");
  EXPECT_FALSE(fresh.Save(missing, &error));
  EXPECT_EQ(ReadFile(missing), "version: 1\n");
}

TEST_F(LauncherConfigDocumentFileTest, RefusesSymlinks) {
  const std::filesystem::path target = temporary_.path() / "real.yaml";
  WriteFile(target, Fixture());
  ASSERT_EQ(symlink(target.c_str(), file_.c_str()), 0);
  ConfigDocument document;
  std::string error;
  EXPECT_FALSE(ConfigDocument::Load(file_, &document, &error));
  EXPECT_NE(error.find("symlink"), std::string::npos) << error;

  // A regular file swapped for a symlink after loading is not followed.
  ASSERT_EQ(unlink(file_.c_str()), 0);
  WriteFile(file_, Fixture());
  ASSERT_TRUE(ConfigDocument::Load(file_, &document, &error)) << error;
  ASSERT_TRUE(document.Set("graphics.vsync", "on", ScalarKind::kEnum, &error))
      << error;
  ASSERT_EQ(unlink(file_.c_str()), 0);
  ASSERT_EQ(symlink(target.c_str(), file_.c_str()), 0);
  EXPECT_FALSE(document.Save(file_, &error));
  EXPECT_NE(error.find("symlink"), std::string::npos) << error;
  EXPECT_EQ(ReadFile(target), Fixture());
  EXPECT_FALSE(std::filesystem::exists(ConfigDocument::BackupPath(file_)));
}

TEST_F(LauncherConfigDocumentFileTest, RefusesToSaveAnInvalidConfiguration) {
  WriteFile(file_, Fixture());
  ConfigDocument document;
  std::string error;
  ASSERT_TRUE(ConfigDocument::Load(file_, &document, &error)) << error;
  ASSERT_TRUE(document.Set("graphics.backend", "metal", ScalarKind::kEnum,
                           &error))
      << error;
  EXPECT_FALSE(document.Save(file_, &error));
  EXPECT_NE(error.find("graphics.backend"), std::string::npos) << error;
  EXPECT_EQ(ReadFile(file_), Fixture());
}

TEST_F(LauncherConfigDocumentFileTest, CreatesAMissingFileFromTheTemplate) {
  ConfigDocument document;
  std::string error;
  ASSERT_TRUE(ConfigDocument::Load(file_, &document, &error)) << error;
  EXPECT_FALSE(document.identity().exists);
  EXPECT_EQ(document.bytes(), runtime::DefaultRuntimeConfigYaml());
  EXPECT_TRUE(document.HasUnsavedChanges());
  ASSERT_TRUE(document.Set("window.width", "1600", ScalarKind::kInteger,
                           &error))
      << error;
  ASSERT_TRUE(document.Save(file_, &error)) << error;
  EXPECT_EQ(ReadFile(file_), document.bytes());
  EXPECT_EQ(FileMode(file_), 0600);
  EXPECT_FALSE(std::filesystem::exists(ConfigDocument::BackupPath(file_)));
  EXPECT_TRUE(document.identity().exists);
}

TEST_F(LauncherConfigDocumentFileTest, RefusesOversizedAndNonRegularFiles) {
  ConfigDocument document;
  std::string error;
  WriteFile(file_, std::string(1024U * 1024U + 1U, '#'));
  EXPECT_FALSE(ConfigDocument::Load(file_, &document, &error));
  ASSERT_EQ(unlink(file_.c_str()), 0);
  ASSERT_TRUE(std::filesystem::create_directory(file_));
  EXPECT_FALSE(ConfigDocument::Load(file_, &document, &error));
}

}  // namespace
}  // namespace mocktail::launcher
