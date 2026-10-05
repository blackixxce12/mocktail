#include "mocktail/graphics/etc2_decode_totals.h"

#include <algorithm>
#include <cstdio>

namespace mocktail::graphics {

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kFirstMilestone = 64ULL * kMiB;

double Mebibytes(std::uint64_t bytes) {
  return static_cast<double>(bytes) / static_cast<double>(kMiB);
}

unsigned long long Milliseconds(std::uint64_t ns) {
  return static_cast<unsigned long long>((ns + 500000ULL) / 1000000ULL);
}

}  // namespace

void AddEtc2SubmitDecode(Etc2DecodeTotals* totals, std::uint64_t uploads,
                         std::uint64_t compressed_bytes,
                         std::uint64_t host_bytes, std::uint64_t decode_ns) {
  if (totals == nullptr || uploads == 0) {
    return;
  }
  totals->uploads += uploads;
  ++totals->submits;
  totals->compressed_bytes += compressed_bytes;
  totals->host_bytes += host_bytes;
  totals->decode_ns += decode_ns;
  totals->longest_submit_ns = std::max(totals->longest_submit_ns, decode_ns);
}

std::string FormatEtc2DecodeTotals(const Etc2DecodeTotals& totals) {
  char ratio[32] = "";
  if (totals.compressed_bytes != 0) {
    std::snprintf(ratio, sizeof(ratio), " (%.1fx)",
                  static_cast<double>(totals.host_bytes) /
                      static_cast<double>(totals.compressed_bytes));
  }
  char line[256];
  std::snprintf(line, sizeof(line),
                "images=%llu (upscaled %llu) uploads=%llu in %llu submits "
                "compressed=%.1f MiB host=%.1f MiB%s decode=%llu ms "
                "longest=%llu ms",
                static_cast<unsigned long long>(totals.images),
                static_cast<unsigned long long>(totals.upscaled_images),
                static_cast<unsigned long long>(totals.uploads),
                static_cast<unsigned long long>(totals.submits),
                Mebibytes(totals.compressed_bytes),
                Mebibytes(totals.host_bytes), ratio,
                Milliseconds(totals.decode_ns),
                Milliseconds(totals.longest_submit_ns));
  return line;
}

bool Etc2DecodeTotalsMilestone(std::uint64_t before, std::uint64_t after) {
  for (std::uint64_t milestone = kFirstMilestone;
       milestone != 0 && milestone <= after; milestone <<= 1) {
    if (before < milestone) {
      return true;
    }
  }
  return false;
}

}  // namespace mocktail::graphics
