// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <utility>

#include "video_core/shader_warm/pending_warm_record.h"

namespace VideoCommon::ShaderWarm {

void PendingWarmRecord::Hold(WarmRecord record_) {
    std::scoped_lock lock{mutex};
    record = std::move(record_);
    held_at = ++generation;
}

PendingWarmRecord::Ticket PendingWarmRecord::TicketBeforeRead() const {
    std::scoped_lock lock{mutex};
    return generation;
}

std::optional<std::string> PendingWarmRecord::AfterSave(bool whole, Ticket ticket) {
    WarmRecord taken;
    {
        std::scoped_lock lock{mutex};
        if (!whole || !record || held_at > ticket) {
            return std::nullopt;
        }
        taken = std::exchange(record, {});
    }
    return taken();
}

void PendingWarmRecord::Drop() {
    std::scoped_lock lock{mutex};
    record = {};
}

bool PendingWarmRecord::Held() const {
    std::scoped_lock lock{mutex};
    return static_cast<bool>(record);
}

} // namespace VideoCommon::ShaderWarm
