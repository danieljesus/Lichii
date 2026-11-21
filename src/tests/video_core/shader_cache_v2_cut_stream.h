// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <ios>
#include <span>
#include <streambuf>

#include "common/common_types.h"

namespace ShaderCacheV2Test {

/// A stream over `bytes` that serves only the first `limit` of them, then either ends, as a read
/// error does in libc++, or fails, as one does in libstdc++ (badbit).
class CutStream final : public std::streambuf {
public:
    CutStream(std::span<const u8> bytes_, std::size_t limit_, bool fail_)
        : bytes{bytes_}, limit{limit_}, fail{fail_} {}

protected:
    int_type underflow() override {
        if (position >= limit) {
            if (fail) {
                throw std::ios_base::failure("read error");
            }
            return traits_type::eof();
        }
        current = static_cast<char>(bytes[position++]);
        setg(&current, &current, &current + 1);
        return traits_type::to_int_type(current);
    }

private:
    std::span<const u8> bytes;
    std::size_t limit;
    bool fail;
    std::size_t position{};
    char current{};
};

} // namespace ShaderCacheV2Test
