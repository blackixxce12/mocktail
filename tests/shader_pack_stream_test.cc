// Roblox's Vulkan shader pack loader (FStringGraphicsVulkanShaderMTDenyPattern
// decides whether it runs on one thread or several) opens the pack once:
// AAssetManager_open, AAsset_openFileDescriptor, fdopen, then fseek to the
// asset's start. With several threads, every worker reads its entries from
// that one FILE as fseek(start + entry offset) followed by fread(entry size),
// and holds a libc++ std::mutex around the pair: pthread_mutex_lock and
// pthread_mutex_unlock on a zeroed 40-byte Bionic mutex (2.736.1408 and
// 2.738.1397 both do; nothing else in the loader touches the FILE).
//
// The guest's FILE is the host glibc FILE that fdopen returned, and its
// fseek, fread and pthread_mutex_* calls bind to the Mocktail exports used
// here. These tests pin down the two properties the loader relies on:
// the guest's own lock keeps every pair together under contention, and the
// stream keeps the one shared position that stdio defines, as Bionic does.

#include "compat/bionic_abi_exports.h"
#include "compat/bionic_stdio_runtime.h"

#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

struct AAssetManager;
struct AAsset;

extern "C" {
AAssetManager* AAssetManager_fromJava(void* env, void* asset_manager);
AAsset* AAssetManager_open(AAssetManager* manager, const char* filename,
                           int mode);
void AAsset_close(AAsset* asset);
int AAsset_openFileDescriptor(AAsset* asset, off_t* out_start,
                              off_t* out_length);
}

namespace {

constexpr int kAssetModeBuffer = 3;
constexpr char kPackName[] = "shaders/shaders_vulkan_mobile.pack";
constexpr std::size_t kPackBytes = 3U * 1024U * 1024U;

unsigned char PackByte(std::size_t offset) {
  std::uint64_t value = offset * 0x9e3779b97f4a7c15ULL;
  value ^= value >> 29;
  return static_cast<unsigned char>(value);
}

bool MatchesPack(std::size_t offset, const std::vector<unsigned char>& data) {
  for (std::size_t index = 0; index < data.size(); ++index) {
    if (data[index] != PackByte(offset + index)) {
      return false;
    }
  }
  return true;
}

// Steps two threads through a fixed interleaving.
class Turnstile {
 public:
  void WaitFor(int step) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return step_ == step; });
  }
  void Advance() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++step_;
    }
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  int step_ = 0;
};

class ShaderPackStreamTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "mocktail-shader-pack-XXXXXX")
            .string();
    ASSERT_NE(nullptr, mkdtemp(pattern.data()));
    root_ = pattern;
    std::filesystem::create_directories(root_ / "shaders");
    std::ofstream pack(root_ / kPackName, std::ios::binary);
    for (std::size_t offset = 0; offset < kPackBytes; ++offset) {
      pack.put(static_cast<char>(PackByte(offset)));
    }
    pack.close();
    ASSERT_TRUE(pack.good());
    ASSERT_EQ(0, setenv("MOCKTAIL_ASSET_ROOT", root_.c_str(), 1));
    OpenPackLikeRoblox();
  }

  void TearDown() override {
    if (pack_ != nullptr) {
      EXPECT_EQ(0, mocktail_fclose(pack_));
    }
    unsetenv("MOCKTAIL_ASSET_ROOT");
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  // The loader's opener: a FILE over the asset's descriptor, positioned at
  // the asset's start; the AAsset itself is closed straight away.
  void OpenPackLikeRoblox() {
    AAssetManager* manager = AAssetManager_fromJava(nullptr, nullptr);
    ASSERT_NE(nullptr, manager);
    AAsset* asset = AAssetManager_open(manager, kPackName, kAssetModeBuffer);
    ASSERT_NE(nullptr, asset);
    off_t length = -1;
    const int fd = AAsset_openFileDescriptor(asset, &start_, &length);
    AAsset_close(asset);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(static_cast<off_t>(kPackBytes), length);
    pack_ = ::fdopen(fd, "rb");
    ASSERT_NE(nullptr, pack_);
    ASSERT_EQ(0, mocktail_fseek(pack_, static_cast<long>(start_), SEEK_SET));
  }

  long PackOffset(std::size_t offset) const {
    return static_cast<long>(start_) + static_cast<long>(offset);
  }

  std::filesystem::path root_;
  FILE* pack_ = nullptr;
  off_t start_ = 0;
};

TEST_F(ShaderPackStreamTest, GuestLockedSeekReadPairsShareOneStream) {
  constexpr std::size_t kEntryCount = 4096;
  constexpr unsigned kWorkerCount = 8;
  struct Entry {
    std::size_t offset = 0;
    std::size_t size = 0;
  };
  // Entry sizes up to three stdio buffers, so reads straddle refills.
  std::vector<Entry> entries;
  entries.reserve(kEntryCount);
  std::uint64_t state = 0x243f6a8885a308d3ULL;
  for (std::size_t index = 0; index < kEntryCount; ++index) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::size_t size =
        1 + static_cast<std::size_t>((state >> 33) % 12288);
    const std::size_t offset =
        static_cast<std::size_t>((state >> 11) % (kPackBytes - size));
    entries.push_back({offset, size});
  }

  // libc++'s constexpr std::mutex is the zero Bionic initializer.
  alignas(8) pthread_mutex_t guest_mutex{};
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> lock_failures{0};
  std::atomic<std::size_t> short_reads{0};
  std::atomic<std::size_t> wrong_entries{0};
  const auto worker = [&] {
    std::vector<unsigned char> buffer;
    for (;;) {
      const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
      if (index >= entries.size()) {
        return;
      }
      const Entry& entry = entries[index];
      buffer.assign(entry.size, 0);
      if (mocktail_pthread_mutex_lock(&guest_mutex) != 0) {
        lock_failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      const std::size_t read =
          mocktail_fseek(pack_, PackOffset(entry.offset), SEEK_SET) == 0
              ? mocktail_fread(buffer.data(), 1, entry.size, pack_)
              : 0;
      if (mocktail_pthread_mutex_unlock(&guest_mutex) != 0) {
        lock_failures.fetch_add(1, std::memory_order_relaxed);
      }
      if (read != entry.size) {
        short_reads.fetch_add(1, std::memory_order_relaxed);
      } else if (!MatchesPack(entry.offset, buffer)) {
        wrong_entries.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };
  std::vector<std::thread> workers;
  for (unsigned index = 0; index < kWorkerCount; ++index) {
    workers.emplace_back(worker);
  }
  for (std::thread& thread : workers) {
    thread.join();
  }

  EXPECT_EQ(0U, lock_failures.load());
  EXPECT_EQ(0U, short_reads.load());
  EXPECT_EQ(0U, wrong_entries.load())
      << "entries read under the guest's lock came back with other bytes";
}

// The stream has one position, whichever thread moves it: a pair that is
// not under the caller's lock can read another thread's entry, exactly as
// on Bionic. That is why the loader holds its lock, and why Mocktail must not
// give threads positions of their own: a FILE positioned on one thread and
// read on another would then read from the wrong place.
TEST_F(ShaderPackStreamTest, StreamKeepsOnePositionAcrossThreads) {
  constexpr std::size_t kEntrySize = 4096 + 173;
  Turnstile turnstile;
  std::vector<unsigned char> a_data(kEntrySize);
  std::vector<unsigned char> b_data(kEntrySize);
  std::size_t a_read = 0;
  std::size_t b_read = 0;
  // A: fseek(entry 3) | B: fseek(entry 7) | A: fread | B: fread
  std::thread a([&] {
    turnstile.WaitFor(0);
    EXPECT_EQ(0, mocktail_fseek(pack_, PackOffset(3 * kEntrySize), SEEK_SET));
    turnstile.Advance();
    turnstile.WaitFor(2);
    a_read = mocktail_fread(a_data.data(), 1, a_data.size(), pack_);
    turnstile.Advance();
  });
  std::thread b([&] {
    turnstile.WaitFor(1);
    EXPECT_EQ(0, mocktail_fseek(pack_, PackOffset(7 * kEntrySize), SEEK_SET));
    turnstile.Advance();
    turnstile.WaitFor(3);
    b_read = mocktail_fread(b_data.data(), 1, b_data.size(), pack_);
    turnstile.Advance();
  });
  a.join();
  b.join();

  ASSERT_EQ(kEntrySize, a_read);
  ASSERT_EQ(kEntrySize, b_read);
  EXPECT_TRUE(MatchesPack(7 * kEntrySize, a_data))
      << "A's fread must start where B's fseek left the stream";
  EXPECT_TRUE(MatchesPack(8 * kEntrySize, b_data))
      << "B's fread must continue where A's fread stopped";
  EXPECT_EQ(PackOffset(9 * kEntrySize), mocktail_ftell(pack_));
}

}  // namespace
