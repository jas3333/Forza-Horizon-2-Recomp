/**
 * @file        guest_cache_vfs_test.cpp
 * @brief       Unit tests for writable Xbox cache device mappings
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>

namespace fs = std::filesystem;

namespace {

struct TempDirectory {
  TempDirectory()
      : path(fs::temp_directory_path() /
             ("rex_guest_cache_" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    fs::create_directories(path);
  }

  ~TempDirectory() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }

  fs::path path;
};

bool MountWritableCache(rex::filesystem::VirtualFileSystem& vfs, const fs::path& root,
                        std::string_view link, std::string_view mount) {
  auto device =
      std::make_unique<rex::filesystem::HostPathDevice>(mount, root, false /* writable */);
  return device->Initialize() && vfs.RegisterDevice(std::move(device)) &&
         vfs.RegisterSymbolicLink(link, mount);
}

}  // namespace

TEST_CASE("Guest cache devices resolve to separate writable host roots", "[system][vfs]") {
  TempDirectory temp;
  rex::filesystem::VirtualFileSystem vfs;

  REQUIRE(MountWritableCache(vfs, temp.path / "cache0", "cache0:", "\\CACHE0"));
  REQUIRE(MountWritableCache(vfs, temp.path / "cache1", "cache1:", "\\CACHE1"));
  REQUIRE(MountWritableCache(vfs, temp.path / "cache", "cache:", "\\CACHE"));

  REQUIRE(vfs.ResolvePath("cache:\\") != nullptr);
  REQUIRE(vfs.ResolvePath("cache0:\\") != nullptr);
  REQUIRE(vfs.ResolvePath("cache1:\\") != nullptr);

  REQUIRE(vfs.CreatePath("cache:\\replay_stream", rex::filesystem::kFileAttributeNormal) !=
          nullptr);
  CHECK(fs::exists(temp.path / "cache" / "replay_stream"));
  CHECK_FALSE(fs::exists(temp.path / "cache0" / "replay_stream"));
  CHECK_FALSE(fs::exists(temp.path / "cache1" / "replay_stream"));
}
