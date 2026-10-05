#ifndef MOCKTAIL_GRAPHICS_ETC2_DECODE_TOTALS_H_
#define MOCKTAIL_GRAPHICS_ETC2_DECODE_TOTALS_H_

#include <cstdint>
#include <string>

namespace mocktail::graphics {

// What ETC2/EAC emulation did in one game session. Roblox budgets these
// textures at their compressed size, while the host images hold them
// decoded (4x for RGBA and EAC, 8x for RGB, more for upscaled small ones),
// and every decode runs on the CPU inside the vkQueueSubmit that uploads.
struct Etc2DecodeTotals {
  std::uint64_t images = 0;           // emulated images created
  std::uint64_t upscaled_images = 0;  // of them, small textures upscaled
  std::uint64_t uploads = 0;          // uploads decoded
  std::uint64_t submits = 0;          // submits that decoded something
  std::uint64_t compressed_bytes = 0;
  std::uint64_t host_bytes = 0;       // written to the host images
  std::uint64_t decode_ns = 0;        // submit time spent on decoding
  std::uint64_t longest_submit_ns = 0;
};

// Adds one submit's decoding.
void AddEtc2SubmitDecode(Etc2DecodeTotals* totals, std::uint64_t uploads,
                         std::uint64_t compressed_bytes,
                         std::uint64_t host_bytes, std::uint64_t decode_ns);

// "images=12 (upscaled 3) uploads=480 in 37 submits compressed=21.3 MiB
// host=118.0 MiB (5.5x) decode=812 ms longest=41 ms"
std::string FormatEtc2DecodeTotals(const Etc2DecodeTotals& totals);

// True when host bytes growing from `before` to `after` pass 64 MiB or a
// doubling of it, so a session that never destroys its device still logs
// its totals a few times.
bool Etc2DecodeTotalsMilestone(std::uint64_t before, std::uint64_t after);

}  // namespace mocktail::graphics

#endif  // MOCKTAIL_GRAPHICS_ETC2_DECODE_TOTALS_H_
