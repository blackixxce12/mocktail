#include "launcher/fast_flags_document.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "services/client_settings_service.h"

namespace mocktail::launcher {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_launcher_fflags_XXXXXX";
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

void WriteFile(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << bytes;
}

mode_t FileMode(const std::filesystem::path& path) {
  struct stat status = {};
  return lstat(path.c_str(), &status) == 0 ? status.st_mode & 0777 : 0;
}

std::vector<std::string> Names(const FastFlagsDocument& document) {
  std::vector<std::string> names;
  for (const FastFlagEntry& entry : document.entries()) {
    names.push_back(entry.name);
  }
  return names;
}

class LauncherFastFlagsDocumentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_FALSE(temporary_.path().empty());
    file_ = temporary_.path() / "fflags.json";
    // The FRM quality level follows this variable, as at startup.
    unsetenv("MOCKTAIL_GRAPHICS_QUALITY");
  }

  TemporaryDirectory temporary_;
  std::filesystem::path file_;
};

TEST_F(LauncherFastFlagsDocumentTest, KeepsFileOrderAndValueTypes) {
  WriteFile(file_,
            "{\n"
            "  // Roblox tolerates comments here, as does Mocktail.\n"
            "  \"FFlagDebugDisplayFPS\": true,\n"
            "  \"DFIntConnectionMTUSize\": 900,\n"
            "  \"FStringPartTexturePackTable2022\": \"{\\\"x\\\": 1}\",\n"
            "  \"FIntNegative\": -5,\n"
            "  \"FIntHuge\": 18446744073709551615\n"
            "}\n");
  FastFlagsDocument document;
  std::string error;
  ASSERT_TRUE(FastFlagsDocument::Load(file_, &document, &error)) << error;
  EXPECT_FALSE(document.HasUnsavedChanges());
  EXPECT_EQ(Names(document),
            (std::vector<std::string>{"FFlagDebugDisplayFPS",
                                      "DFIntConnectionMTUSize",
                                      "FStringPartTexturePackTable2022",
                                      "FIntNegative", "FIntHuge"}));
  const FastFlagEntry* fps = document.Find("FFlagDebugDisplayFPS");
  ASSERT_NE(fps, nullptr);
  EXPECT_EQ(fps->kind, FastFlagValueKind::kBoolean);
  EXPECT_EQ(fps->value, "true");
  EXPECT_EQ(fps->RobloxValue(), "True");
  EXPECT_EQ(document.Find("DFIntConnectionMTUSize")->kind,
            FastFlagValueKind::kInteger);
  EXPECT_EQ(document.Find("FIntNegative")->value, "-5");
  EXPECT_EQ(document.Find("FIntHuge")->value, "18446744073709551615");
  EXPECT_EQ(document.Find("FStringPartTexturePackTable2022")->value,
            "{\"x\": 1}");
  EXPECT_EQ(document.Find("Missing"), nullptr);

  // Changing an entry keeps its place; new entries go last.
  ASSERT_TRUE(document.Set("DFIntConnectionMTUSize",
                           FastFlagValueKind::kInteger, "1400", &error))
      << error;
  ASSERT_TRUE(document.Set("FFlagNew", FastFlagValueKind::kBoolean, "False",
                           &error))
      << error;
  EXPECT_TRUE(document.Remove("FFlagDebugDisplayFPS"));
  EXPECT_FALSE(document.Remove("FFlagDebugDisplayFPS"));
  EXPECT_TRUE(document.HasUnsavedChanges());
  EXPECT_EQ(Names(document),
            (std::vector<std::string>{"DFIntConnectionMTUSize",
                                      "FStringPartTexturePackTable2022",
                                      "FIntNegative", "FIntHuge",
                                      "FFlagNew"}));
  EXPECT_EQ(document.Serialize(),
            "{\n"
            "  \"DFIntConnectionMTUSize\": 1400,\n"
            "  \"FStringPartTexturePackTable2022\": \"{\\\"x\\\": 1}\",\n"
            "  \"FIntNegative\": -5,\n"
            "  \"FIntHuge\": 18446744073709551615,\n"
            "  \"FFlagNew\": false\n"
            "}\n");
}

TEST_F(LauncherFastFlagsDocumentTest, SavesWhatMocktailLoads) {
  FastFlagsDocument document;
  std::string error;
  ASSERT_TRUE(FastFlagsDocument::Load(file_, &document, &error)) << error;
  EXPECT_TRUE(document.entries().empty());
  EXPECT_FALSE(document.identity().exists);
  ASSERT_TRUE(document.Set("FFlagDebugDisplayFPS", FastFlagValueKind::kBoolean,
                           "true", &error));
  ASSERT_TRUE(document.Set("DFIntConnectionMTUSize",
                           FastFlagValueKind::kInteger, "900", &error));
  ASSERT_TRUE(document.Set("FStringName", FastFlagValueKind::kString,
                           "Привет \"мир\"\n", &error));
  const mode_t previous_umask = umask(0);
  const bool saved = document.Save(file_, &error);
  umask(previous_umask);
  ASSERT_TRUE(saved) << error;
  EXPECT_FALSE(document.HasUnsavedChanges());
  EXPECT_EQ(FileMode(file_), 0600);

  const services::FflagsMergeResult merged =
      services::LoadAndMergeFflagsFile(file_, "{}");
  ASSERT_TRUE(merged.error.empty()) << merged.error;
  EXPECT_TRUE(merged.loaded);
  EXPECT_EQ(merged.count, 3U);
  EXPECT_NE(merged.json.find("\"FFlagDebugDisplayFPS\":\"True\""),
            std::string::npos)
      << merged.json;
  EXPECT_NE(merged.json.find("\"DFIntConnectionMTUSize\":\"900\""),
            std::string::npos)
      << merged.json;

  FastFlagsDocument reloaded;
  ASSERT_TRUE(FastFlagsDocument::Load(file_, &reloaded, &error)) << error;
  EXPECT_EQ(Names(reloaded), Names(document));
  EXPECT_EQ(reloaded.Find("FStringName")->value, "Привет \"мир\"\n");

  // A second save needs no reload.
  ASSERT_TRUE(document.Remove("FStringName"));
  ASSERT_TRUE(document.Save(file_, &error)) << error;
  EXPECT_EQ(ReadFile(file_), document.Serialize());
}

TEST_F(LauncherFastFlagsDocumentTest, RejectsWhatTheRuntimeRejects) {
  FastFlagsDocument document;
  std::string error;
  for (const std::string& bytes :
       {std::string("[1, 2]"), std::string("{\"FIntX\": 1.5}"),
        std::string("{\"FIntX\": null}"), std::string("{\"FIntX\": {}}"),
        std::string("{\"\": 1}"), std::string("{not json"), std::string("")}) {
    WriteFile(file_, bytes);
    EXPECT_FALSE(FastFlagsDocument::Load(file_, &document, &error)) << bytes;
    EXPECT_FALSE(services::LoadAndMergeFflagsFile(file_, "{}").error.empty())
        << bytes;
  }
  WriteFile(file_, "{\"FStringBig\": \"" +
                       std::string(FastFlagsDocument::kMaximumBytes, 'x') +
                       "\"}");
  EXPECT_FALSE(FastFlagsDocument::Load(file_, &document, &error));
}

TEST_F(LauncherFastFlagsDocumentTest, ValidatesNamesAndValues) {
  FastFlagsDocument document;
  std::string error;
  EXPECT_FALSE(document.Set("", FastFlagValueKind::kBoolean, "true", &error));
  EXPECT_FALSE(document.Set("FFlag Name", FastFlagValueKind::kBoolean, "true",
                            &error));
  EXPECT_FALSE(document.Set("FFlag.Name", FastFlagValueKind::kBoolean, "true",
                            &error));
  EXPECT_FALSE(document.Set("FFlagX", FastFlagValueKind::kBoolean, "yes",
                            &error));
  EXPECT_FALSE(document.Set("FIntX", FastFlagValueKind::kInteger, "12a",
                            &error));
  EXPECT_FALSE(document.Set("FIntX", FastFlagValueKind::kInteger, "",
                            &error));
  EXPECT_FALSE(document.Set("FIntX", FastFlagValueKind::kInteger,
                            "18446744073709551616", &error));
  EXPECT_FALSE(document.Set("FStringX", FastFlagValueKind::kString,
                            "bad \xC3\x28", &error));
  EXPECT_TRUE(document.entries().empty());

  ASSERT_TRUE(document.Set("FIntX", FastFlagValueKind::kInteger,
                           "-9223372036854775808", &error))
      << error;
  ASSERT_TRUE(document.Set("FFlagX", FastFlagValueKind::kBoolean, "TRUE",
                           &error))
      << error;
  EXPECT_EQ(document.Find("FFlagX")->value, "true");

  // A value that would push the file past 64 KiB is not saved.
  ASSERT_TRUE(document.Set("FStringBig", FastFlagValueKind::kString,
                           std::string(FastFlagsDocument::kMaximumBytes, 'x'),
                           &error));
  EXPECT_FALSE(document.Save(file_, &error));
  EXPECT_FALSE(std::filesystem::exists(file_));
}

TEST_F(LauncherFastFlagsDocumentTest, ReportsFlagsMocktailManages) {
  FastFlagsDocument document;
  std::string error;
  ASSERT_TRUE(document.Set("DFIntTaskSchedulerTargetFps",
                           FastFlagValueKind::kInteger, "60", &error));
  ASSERT_TRUE(document.Set("FIntDebugFRMQualityLevelOverride",
                           FastFlagValueKind::kInteger, "5", &error));
  ASSERT_TRUE(document.Set("FFlagUseCrashpad", FastFlagValueKind::kBoolean,
                           "true", &error));
  ASSERT_TRUE(document.Set("FFlagDebugDisplayFPS", FastFlagValueKind::kBoolean,
                           "true", &error));

  // The reporter's settings: 165 fps, multithreaded rendering, throughput.
  const runtime::FrameRatePolicy fixed_165 =
      runtime::ParseFrameRatePolicy("165");
  const runtime::PerformancePolicy preset =
      runtime::ParsePerformancePolicy("true", "0", "on", "throughput");
  std::vector<FastFlagConflict> conflicts =
      document.FindManagedConflicts(fixed_165, preset);
  ASSERT_EQ(conflicts.size(), 3U);
  EXPECT_EQ(conflicts[0].name, "DFIntTaskSchedulerTargetFps");
  EXPECT_EQ(conflicts[0].file_value, "60");
  EXPECT_EQ(conflicts[0].managed_value, "165");
  EXPECT_EQ(conflicts[0].effect, FastFlagConflictEffect::kBlocksStart);
  EXPECT_EQ(conflicts[1].name, "FIntDebugFRMQualityLevelOverride");
  EXPECT_EQ(conflicts[1].managed_value, "3");
  EXPECT_EQ(conflicts[1].effect, FastFlagConflictEffect::kBlocksStart);
  EXPECT_EQ(conflicts[2].name, "FFlagUseCrashpad");
  EXPECT_EQ(conflicts[2].file_value, "True");
  EXPECT_EQ(conflicts[2].managed_value, "False");
  EXPECT_EQ(conflicts[2].effect, FastFlagConflictEffect::kOverridden);

  // With Roblox owning the frame rate and no preset, only the always-on
  // crash-report policy remains.
  conflicts = document.FindManagedConflicts(
      runtime::ParseFrameRatePolicy("-1"),
      runtime::ParsePerformancePolicy("false", "0", "auto", "auto"));
  ASSERT_EQ(conflicts.size(), 1U);
  EXPECT_EQ(conflicts[0].name, "FFlagUseCrashpad");

  // Agreeing values are no conflict.
  ASSERT_TRUE(document.Set("DFIntTaskSchedulerTargetFps",
                           FastFlagValueKind::kString, "165", &error));
  conflicts = document.FindManagedConflicts(fixed_165, preset);
  for (const FastFlagConflict& conflict : conflicts) {
    EXPECT_NE(conflict.name, "DFIntTaskSchedulerTargetFps");
  }

  EXPECT_TRUE(FastFlagsDocument::IsManagedFlag("DFIntTaskSchedulerTargetFps"));
  EXPECT_TRUE(
      FastFlagsDocument::IsManagedFlag("FIntDebugFRMQualityLevelOverride"));
  EXPECT_TRUE(FastFlagsDocument::IsManagedFlag("FIntTaskSchedulerThreadMin"));
  EXPECT_TRUE(FastFlagsDocument::IsManagedFlag(
      "DFIntSimMidPhaseContactPipelineBatchSize"));
  EXPECT_TRUE(FastFlagsDocument::IsManagedFlag("FFlagUseCrashpad"));
  EXPECT_FALSE(FastFlagsDocument::IsManagedFlag("FFlagDebugDisplayFPS"));
}

TEST_F(LauncherFastFlagsDocumentTest, RefusesConcurrentChangesAndSymlinks) {
  WriteFile(file_, "{\"FFlagA\": true}");
  FastFlagsDocument document;
  std::string error;
  ASSERT_TRUE(FastFlagsDocument::Load(file_, &document, &error)) << error;
  ASSERT_TRUE(document.Set("FFlagB", FastFlagValueKind::kBoolean, "false",
                           &error));
  WriteFile(file_, "{\"FFlagC\": true}");
  EXPECT_FALSE(document.Save(file_, &error));
  EXPECT_NE(error.find("changed on disk"), std::string::npos) << error;
  EXPECT_EQ(ReadFile(file_), "{\"FFlagC\": true}");

  const std::filesystem::path target = temporary_.path() / "elsewhere.json";
  WriteFile(target, "{}");
  const std::filesystem::path link = temporary_.path() / "link.json";
  ASSERT_EQ(symlink(target.c_str(), link.c_str()), 0);
  EXPECT_FALSE(FastFlagsDocument::Load(link, &document, &error));

  // A file created after a missing-file load is not replaced.
  const std::filesystem::path late = temporary_.path() / "late.json";
  FastFlagsDocument empty;
  ASSERT_TRUE(FastFlagsDocument::Load(late, &empty, &error)) << error;
  ASSERT_TRUE(empty.Set("FFlagA", FastFlagValueKind::kBoolean, "true", &error));
  WriteFile(late, "{}");
  EXPECT_FALSE(empty.Save(late, &error));
  EXPECT_EQ(ReadFile(late), "{}");
}

}  // namespace
}  // namespace mocktail::launcher
