// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
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

TEST_CASE("Memory pressure caps the texture cache only while it lasts",
          "[video_core][texture_cache][host_memory_limit]") {
    using Common::MemoryPressure;
    using VideoCommon::HostPressureLimit;
    using Clock = HostPressureLimit::Clock;
    const Clock::time_point t0{std::chrono::seconds{1000}};
    HostPressureLimit limit;
    // Normal, or a host that does not say: Metal's budget applies as before.
    REQUIRE_FALSE(limit.Update(MemoryPressure::Normal, t0, 6 * GiB, 4 * GiB).has_value());
    REQUIRE_FALSE(limit.Update(MemoryPressure::Unknown, t0, 6 * GiB, 4 * GiB).has_value());
    // Warning: the cache does not grow past what is in use when it came...
    REQUIRE(limit.Update(MemoryPressure::Warning, t0, 6 * GiB, 4 * GiB) == 6 * GiB);
    REQUIRE(limit.Update(MemoryPressure::Warning, t0, 7 * GiB, 4 * GiB) == 6 * GiB);
    // ...and does not follow it down as the cache frees images (that would ratchet to the floor).
    REQUIRE(limit.Update(MemoryPressure::Warning, t0, 5 * GiB, 3 * GiB) == 6 * GiB);
    // Normal again: the cap holds until that lasted PressureSettle.
    REQUIRE(limit.Update(MemoryPressure::Normal, t0, 4 * GiB, 3 * GiB) == 6 * GiB);
    REQUIRE(limit.Update(MemoryPressure::Normal, t0 + VideoCommon::PressureSettle -
                                                     std::chrono::seconds{1},
                         4 * GiB, 3 * GiB) == 6 * GiB);
    REQUIRE_FALSE(limit.Update(MemoryPressure::Normal, t0 + VideoCommon::PressureSettle, 4 * GiB,
                               3 * GiB)
                      .has_value());
}

TEST_CASE("Critical memory pressure collects an eighth of the images at a time",
          "[video_core][texture_cache][host_memory_limit]") {
    using Common::MemoryPressure;
    using VideoCommon::HostPressureLimit;
    const HostPressureLimit::Clock::time_point t0{std::chrono::seconds{1000}};
    HostPressureLimit limit;
    // 8 GiB in use, 4 GiB of it images: an eighth of them goes.
    REQUIRE(limit.Update(MemoryPressure::Critical, t0, 8 * GiB, 4 * GiB) == 7 * GiB + 512 * MiB);
    // At the next check, after collecting, another eighth.
    REQUIRE(limit.Update(MemoryPressure::Critical, t0, 7 * GiB + 512 * MiB, 3 * GiB + 512 * MiB) ==
            7 * GiB + 64 * MiB);
    // Never under everything else plus the images a scene needs.
    REQUIRE(limit.Update(MemoryPressure::Critical, t0, 5 * GiB, 1 * GiB) ==
            4 * GiB + VideoCommon::MinPressureImageMemory);
    // A warning while critical settles keeps the lower cap.
    REQUIRE(limit.Update(MemoryPressure::Warning, t0, 6 * GiB, 2 * GiB) ==
            4 * GiB + VideoCommon::MinPressureImageMemory);
    REQUIRE(limit.Level() == MemoryPressure::Critical);
}

TEST_CASE("The host's memory pressure is read where the host says it",
          "[video_core][texture_cache][host_memory_limit]") {
#ifdef __APPLE__
    REQUIRE(Common::GetMemoryPressure() != Common::MemoryPressure::Unknown);
#else
    REQUIRE(Common::GetMemoryPressure() == Common::MemoryPressure::Unknown);
#endif
}

TEST_CASE("A memory pressure warning leaves the cache what a scene needs",
          "[video_core][texture_cache][host_memory_limit]") {
    using Common::MemoryPressure;
    const VideoCommon::HostPressureLimit::Clock::time_point t0{std::chrono::seconds{1000}};
    VideoCommon::HostPressureLimit limit;
    // 2.9 GiB in use when the warning comes: the cap is the floor, not that.
    REQUIRE(limit.Update(MemoryPressure::Warning, t0, 2 * GiB + 900 * MiB, 1 * GiB) ==
            VideoCommon::MinHostCriticalMemory);
}
