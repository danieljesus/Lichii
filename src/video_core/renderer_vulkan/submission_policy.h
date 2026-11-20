// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <optional>
#include <string_view>

#include "common/common_types.h"

namespace Vulkan::SubmissionPolicy {

inline constexpr u32 DefaultNativeBudget = 128;

enum class Action {
    None,
    Dispatch,
    Flush,
};

inline std::optional<u32> ParseBudget(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    u32 budget{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), budget);
    if (error != std::errc{} || end != value.data() + value.size() || budget == 0) {
        return std::nullopt;
    }
    return budget;
}

constexpr Action Advance(u32& counter, bool native, bool android,
                         u32 native_budget = DefaultNativeBudget, u32 legacy_budget = 4096) {
    ++counter;
    if (native && !android) {
        if (counter >= native_budget) {
            counter = 0;
            return Action::Flush;
        }
        return counter % 32 == 0 ? Action::Dispatch : Action::None;
    }
    const u32 check_mask = android ? 3 : 31;
    if ((counter & check_mask) != check_mask) {
        return Action::None;
    }
    if (counter < (android ? 512U : legacy_budget)) {
        return Action::Dispatch;
    }
    counter = 0;
    return Action::Flush;
}

/// Whether a flush the legacy budget asked for in the middle of a render pass, and that waits for
/// the pass to end, happens before this draw. It waits for a draw that starts with no pass active
/// (something outside a pass ended it) or with its render targets changed (it most likely begins
/// another pass), so the flush does not split a pass itself; at most `budget` draws, so a long
/// pass is still submitted.
constexpr bool FlushAtPassBoundary(bool render_pass_active, bool render_targets_dirty,
                                   u32 deferred_draws, u32 budget) {
    return !render_pass_active || render_targets_dirty || deferred_draws >= budget;
}

} // namespace Vulkan::SubmissionPolicy
