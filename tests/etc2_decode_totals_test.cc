#include "mocktail/graphics/etc2_decode_totals.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace mocktail::graphics {
namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

TEST(Etc2DecodeTotalsTest, AddsSubmitsAndKeepsTheLongest) {
  Etc2DecodeTotals totals;
  totals.images = 12;
  totals.upscaled_images = 3;
  AddEtc2SubmitDecode(&totals, 4, 2 * kMiB, 8 * kMiB, 3000000);
  AddEtc2SubmitDecode(&totals, 1, kMiB, 8 * kMiB, 41400000);
  AddEtc2SubmitDecode(&totals, 0, kMiB, kMiB, 99000000);  // nothing decoded
  AddEtc2SubmitDecode(nullptr, 1, 1, 1, 1);

  EXPECT_EQ(totals.uploads, 5U);
  EXPECT_EQ(totals.submits, 2U);
  EXPECT_EQ(totals.compressed_bytes, 3 * kMiB);
  EXPECT_EQ(totals.host_bytes, 16 * kMiB);
  EXPECT_EQ(totals.decode_ns, 44400000U);
  EXPECT_EQ(totals.longest_submit_ns, 41400000U);
  EXPECT_EQ(FormatEtc2DecodeTotals(totals),
            "images=12 (upscaled 3) uploads=5 in 2 submits "
            "compressed=3.0 MiB host=16.0 MiB (5.3x) decode=44 ms "
            "longest=41 ms");
}

TEST(Etc2DecodeTotalsTest, FormatsAnEmptySessionWithoutARatio) {
  Etc2DecodeTotals totals;
  totals.images = 4;
  totals.upscaled_images = 4;
  EXPECT_EQ(FormatEtc2DecodeTotals(totals),
            "images=4 (upscaled 4) uploads=0 in 0 submits "
            "compressed=0.0 MiB host=0.0 MiB decode=0 ms longest=0 ms");
}

TEST(Etc2DecodeTotalsTest, ReportsAtSixtyFourMebibytesAndEachDoubling) {
  EXPECT_FALSE(Etc2DecodeTotalsMilestone(0, 63 * kMiB));
  EXPECT_TRUE(Etc2DecodeTotalsMilestone(63 * kMiB, 64 * kMiB));
  EXPECT_FALSE(Etc2DecodeTotalsMilestone(64 * kMiB, 127 * kMiB));
  EXPECT_TRUE(Etc2DecodeTotalsMilestone(100 * kMiB, 300 * kMiB));
  EXPECT_FALSE(Etc2DecodeTotalsMilestone(300 * kMiB, 500 * kMiB));
  EXPECT_TRUE(Etc2DecodeTotalsMilestone(500 * kMiB, 512 * kMiB));
  EXPECT_TRUE(Etc2DecodeTotalsMilestone(0, UINT64_MAX));
  EXPECT_FALSE(Etc2DecodeTotalsMilestone(UINT64_MAX - 1, UINT64_MAX));
}

TEST(Etc2DecodeTotalsTest, GivesBackScratchLargerThanTheKeepSize) {
  std::vector<std::uint8_t> kept(kEtc2ScratchKeepBytes);
  ReleaseOversizedEtc2Scratch(&kept);
  EXPECT_EQ(kept.size(), kEtc2ScratchKeepBytes);

  std::vector<std::uint8_t> burst(kEtc2ScratchKeepBytes + 1);
  ReleaseOversizedEtc2Scratch(&burst);
  EXPECT_TRUE(burst.empty());
  EXPECT_EQ(burst.capacity(), 0U);

  ReleaseOversizedEtc2Scratch(nullptr);
}

}  // namespace
}  // namespace mocktail::graphics
