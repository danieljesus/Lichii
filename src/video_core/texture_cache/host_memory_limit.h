// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>

#include "common/common_types.h"
#include "common/memory_detect.h"

namespace VideoCommon {

/// Commit the texture cache leaves to the rest of the process and the system.
constexpr u64 HostCommitReserve = u64{2} << 30;

/// The cache is not shrunk below this for host memory: a scene needs about this much, and under
/// it the collector only deletes and decodes the same textures again.
constexpr u64 MinHostCriticalMemory = u64{3} << 30;

/// The most device memory the texture cache may hold before it collects in earnest, as host
/// memory allows it. Windows charges device memory to the system's commit limit (RAM plus the
/// page files) like any other memory, since each allocation has a backing store there. A 16 GB
/// card's budget alone let the cache fill 9.7 GiB on a PC with 16 GiB of RAM and a 16 GiB page
/// file, where the guest's memory already takes 14 GiB of commit: the driver then refused a
/// 2.6 MiB image in every heap, host memory included, and the session ended (Ocarina of Time,
/// RX 7600 XT). `device_usage` is the device memory in use now and `commit_room` the commit the
/// system can still hand out: their sum is what the cache would hold with all of it.
constexpr u64 HostCriticalMemory(u64 device_usage, u64 commit_room) {
    const u64 limit = commit_room > (std::numeric_limits<u64>::max)() - device_usage
                          ? (std::numeric_limits<u64>::max)()
                          : device_usage + commit_room;
    return (std::max)(limit - (std::min)(limit, HostCommitReserve), MinHostCriticalMemory);
}

/// Images the cache keeps under critical memory pressure however hard it collects: below that a
/// scene only deletes and decodes the same textures again (#739).
constexpr u64 MinPressureImageMemory = u64{1} << 30;

/// How long a lower memory pressure has to hold before the cap follows it down.
constexpr std::chrono::seconds PressureSettle{10};

/// A cap on the texture cache's critical threshold from the system's memory pressure, where the
/// host reports one (macOS). A device's budget alone is not the bound on unified memory: Metal's
/// working set is most of the RAM, which the guest's memory and everything else also take.
/// - Normal: no cap; the device's budget applies as before.
/// - Warning: the cache does not grow. The cap is the device memory in use when the warning came,
///   and stays there while it lasts; never under MinHostCriticalMemory.
/// - Critical: the cache collects in earnest. At each check the cap takes an eighth of the images
///   off what is in use, down to MinPressureImageMemory of images above everything else.
/// The cap only falls while the pressure lasts. A higher pressure applies at once; a lower one
/// only after it held for PressureSettle, the cap holding meanwhile, so that what collecting frees
/// does not lift it at once and the cache refill.
class HostPressureLimit {
public:
    using Clock = std::chrono::steady_clock;

    /// `device_usage` is the device memory in use, `image_bytes` the cache's images among it.
    [[nodiscard]] std::optional<u64> Update(Common::MemoryPressure reported, Clock::time_point now,
                                            u64 device_usage, u64 image_bytes) {
        using Common::MemoryPressure;
        if (reported == MemoryPressure::Unknown) {
            reported = MemoryPressure::Normal;
        }
        if (reported > level) {
            level = reported;
            lower_since.reset();
        } else if (reported < level) {
            if (!lower_since) {
                lower_since = now;
            }
            if (now - *lower_since < PressureSettle) {
                // Settling: the cap holds where it is, neither lifted nor lowered.
                return cap;
            }
            level = reported;
            lower_since.reset();
        } else {
            lower_since.reset();
        }
        switch (level) {
        case MemoryPressure::Warning:
            // Latched when the warning came: following the usage down as the cache frees images
            // would ratchet it to the floor.
            if (!cap) {
                cap = (std::max)(device_usage, MinHostCriticalMemory);
            }
            break;
        case MemoryPressure::Critical: {
            const u64 images = (std::min)(image_bytes, device_usage);
            const u64 other = device_usage - images;
            const u64 kept = (std::max)(images - images / 8, MinPressureImageMemory);
            Lower(other + kept);
            // Never under what everything else takes plus the images a scene needs.
            cap = (std::max)(*cap, other + MinPressureImageMemory);
            break;
        }
        default:
            cap.reset();
            break;
        }
        return cap;
    }

    [[nodiscard]] Common::MemoryPressure Level() const {
        return level;
    }

private:
    void Lower(u64 value) {
        cap = cap ? (std::min)(*cap, value) : value;
    }

    Common::MemoryPressure level{Common::MemoryPressure::Normal};
    std::optional<Clock::time_point> lower_since;
    std::optional<u64> cap;
};

} // namespace VideoCommon
