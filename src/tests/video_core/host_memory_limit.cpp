// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "common/memory_detect.h"
#include "video_core/texture_cache/host_memory_limit.h"

namespace {

constexpr u64 MiB = u64{1} << 20;
constexpr u64 GiB = u64{1} << 30;

} // namespace

TEST_CASE("Host memory with room to spare does not bound the texture cache",
          "[video_core][texture_cache][host_memory_limit]") {
    // 32 GiB of RAM and a 16 GB card: far above the 9.7 GiB its device budget allows.
    REQUIRE(VideoCommon::HostCriticalMemory(6 * GiB, 20 * GiB) == 24 * GiB);
    REQUIRE(VideoCommon::HostCriticalMemory(6 * GiB, 20 * GiB) > 9723 * MiB);
}

TEST_CASE("The texture cache collects before the host runs out of commit",
          "[video_core][texture_cache][host_memory_limit]") {
    // The RX 7600 XT session: thresholds at 8955 / 9723 MiB from the device budget alone, on
    // a host with a 31.9 GiB commit limit that the guest's memory takes 14 GiB of.
    const u64 critical = VideoCommon::HostCriticalMemory(7 * GiB, 1 * GiB);
    REQUIRE(critical == 6 * GiB);
    REQUIRE(critical < 7 * GiB);
    // Freeing device memory gives the commit back, which leaves the threshold where it was.
    REQUIRE(VideoCommon::HostCriticalMemory(5 * GiB, 3 * GiB) == critical);
    // Commit taken by something else lowers it.
    REQUIRE(VideoCommon::HostCriticalMemory(7 * GiB, 512 * MiB) < critical);
}

TEST_CASE("The texture cache keeps what a scene needs when the host is out of commit",
          "[video_core][texture_cache][host_memory_limit]") {
    REQUIRE(VideoCommon::HostCriticalMemory(4 * GiB, 0) == VideoCommon::MinHostCriticalMemory);
    REQUIRE(VideoCommon::HostCriticalMemory(0, 0) == VideoCommon::MinHostCriticalMemory);
    REQUIRE(VideoCommon::HostCriticalMemory(1 * GiB, 1 * GiB) ==
            VideoCommon::MinHostCriticalMemory);
}

TEST_CASE("The host memory bound does not wrap", "[video_core][texture_cache][host_memory_limit]") {
    constexpr u64 max = (std::numeric_limits<u64>::max)();
    REQUIRE(VideoCommon::HostCriticalMemory(8 * GiB, max) == max - VideoCommon::HostCommitReserve);
    REQUIRE(VideoCommon::HostCriticalMemory(max, max) == max - VideoCommon::HostCommitReserve);
}

TEST_CASE("The system's commit room is reported where there is a commit limit",
          "[video_core][texture_cache][host_memory_limit]") {
    const std::optional<u64> room = Common::GetSystemCommitRoom();
#ifdef _WIN32
    REQUIRE(room.has_value());
    const Common::ProcessMemoryUsage usage = Common::GetProcessMemoryUsage();
    REQUIRE(*room <= usage.system_commit_limit);
#else
    REQUIRE_FALSE(room.has_value());
#endif
}

TEST_CASE("A room that follows free memory falls at once and rises slowly",
          "[video_core][texture_cache][host_memory_limit]") {
    using VideoCommon::HostRoomRise;
    using VideoCommon::SmoothHostRoom;
    REQUIRE(SmoothHostRoom(std::nullopt, 5 * GiB) == 5 * GiB);
    REQUIRE(SmoothHostRoom(5 * GiB, 2 * GiB) == 2 * GiB);
    // Evicting frees memory: the room comes back a step at a time, not at once.
    REQUIRE(SmoothHostRoom(2 * GiB, 5 * GiB) == 2 * GiB + HostRoomRise);
    REQUIRE(SmoothHostRoom(2 * GiB, 2 * GiB + 100 * MiB) == 2 * GiB + 100 * MiB);
    REQUIRE(SmoothHostRoom(3 * GiB, 3 * GiB) == 3 * GiB);
}

TEST_CASE("macOS's room is free and file-backed pages", "[video_core][texture_cache][host_memory_limit]") {
    // 16 KiB pages: 100000 free (speculative among them) and 50000 file-backed.
    constexpr Common::HostPageCounts counts{.free = 100000, .external = 50000, .page_size = 16384};
    STATIC_REQUIRE(counts.Room() == u64{150000} * 16384);
}

TEST_CASE("The host's room is reported where the host can tell it",
          "[video_core][texture_cache][host_memory_limit]") {
    const std::optional<Common::HostMemoryRoom> room = Common::GetHostMemoryRoom();
#ifdef _WIN32
    REQUIRE(room.has_value());
    REQUIRE_FALSE(room->instantaneous);
    const Common::ProcessMemoryUsage usage = Common::GetProcessMemoryUsage();
    REQUIRE(room->bytes <= usage.system_commit_limit);
#elif defined(__APPLE__)
    const std::optional<Common::HostPageCounts> before = Common::GetHostPageCounts();
    const std::optional<Common::HostMemoryRoom> now = Common::GetHostMemoryRoom();
    const std::optional<Common::HostPageCounts> after = Common::GetHostPageCounts();
    REQUIRE(before.has_value());
    REQUIRE(now.has_value());
    REQUIRE(after.has_value());
    REQUIRE(now->instantaneous);
    REQUIRE(now->bytes > 0);
    REQUIRE(now->bytes <= Common::GetMemInfo().TotalPhysicalMemory);
    // Free and file-backed pages, no more: read just before and after, it is within them.
    REQUIRE(now->bytes <= (std::max)(before->Room(), after->Room()) + 64 * MiB);
    REQUIRE(before->page_size >= 4096);
    // The low-memory hints keep their old figure on macOS: HostMemory::HintCold has nothing to
    // do there.
    REQUIRE(Common::GetAvailablePhysicalMemory() == 0);
    REQUIRE_FALSE(Common::IsHostLowOnMemory());
#else
    REQUIRE_FALSE(room.has_value());
#endif
}
