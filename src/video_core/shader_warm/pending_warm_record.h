// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "common/common_types.h"

namespace VideoCommon::ShaderWarm {

/// Writes what a precache recorded (its marker, its progress) and says what it wrote, for the log;
/// empty when it wrote nothing (the failure is logged by the record).
using WarmRecord = std::function<std::string()>;

/// What a precache recorded, held until a whole save of the pipeline cache that holds its builds:
/// whichever save that is (the one queued after the precache, a timed flush, a game change, the
/// shutdown), as the prebuild that starts right after the precache can overtake the first. A new
/// title's load drops it. A save takes a ticket before it reads the cache and may write only a
/// record held by then: one whose read began before the precache's last builds holds none of
/// them. Safe to use from the loading and serialization threads.
class PendingWarmRecord {
public:
    using Ticket = u64;

    /// Holds `record`, replacing one held before.
    void Hold(WarmRecord record);

    /// Taken by a save before it reads the cache.
    [[nodiscard]] Ticket TicketBeforeRead() const;

    /// After that save: a whole one writes the record held when its ticket was taken and returns
    /// what the record wrote (empty when it wrote nothing). Empty, and the record kept, after a
    /// partial or failed save, one whose read began before the record was held, or when nothing
    /// is held.
    std::optional<std::string> AfterSave(bool whole, Ticket ticket);

    /// Drops what is held.
    void Drop();

    [[nodiscard]] bool Held() const;

private:
    mutable std::mutex mutex;
    WarmRecord record;
    /// Counts the records held; `held_at` is the held one's count
    u64 generation = 0;
    u64 held_at = 0;
};

} // namespace VideoCommon::ShaderWarm
