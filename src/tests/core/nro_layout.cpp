// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/loader/nro.h"

namespace {

struct Segment {
    u32 offset;
    u32 size;
};

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

constexpr Segment Text{0, 0x1000};
constexpr Segment RoData{0x1000, 0x1000};
constexpr Segment Data{0x2000, 0x1000};

// An NRO's header for the given segments, followed by zeros up to `stored` bytes in all.
std::vector<u8> MakeNro(Segment text, Segment rodata, Segment data, u32 file_size, u32 bss_size,
                        std::size_t stored) {
    std::vector<u8> bytes(stored);
    Put32(bytes, 0x10, Common::MakeMagic('N', 'R', 'O', '0'));
    Put32(bytes, 0x18, file_size);
    const Segment segments[] = {text, rodata, data};
    for (std::size_t i = 0; i < 3; ++i) {
        Put32(bytes, 0x20 + i * 8, segments[i].offset);
        Put32(bytes, 0x24 + i * 8, segments[i].size);
    }
    Put32(bytes, 0x38, bss_size);
    return bytes;
}

std::vector<u8> MakeNro(u32 bss_size = 0x2000) {
    return MakeNro(Text, RoData, Data, 0x3000, bss_size, 0x3000);
}

void PutMod0(std::vector<u8>& bytes, u32 offset, u32 bss_start, u32 bss_end) {
    Put32(bytes, 0x4, offset);
    Put32(bytes, offset, Common::MakeMagic('M', 'O', 'D', '0'));
    Put32(bytes, offset + 0x8, bss_start);
    Put32(bytes, offset + 0xC, bss_end);
}

} // Anonymous namespace

TEST_CASE("NRO layout of a valid NRO", "[core][loader]") {
    // No module header: .bss from the NRO's header. The same segments as the loader always gave.
    const auto bytes = MakeNro();
    const auto layout = Loader::ValidateNroLayout(bytes);
    REQUIRE(layout);
    CHECK(layout->file_size == 0x3000);
    CHECK(layout->image_size == 0x5000);
    CHECK(layout->segments[0].offset == 0);
    CHECK(layout->segments[0].size == 0x1000);
    CHECK(layout->segments[1].offset == 0x1000);
    CHECK(layout->segments[1].size == 0x1000);
    // .data and .bss.
    CHECK(layout->segments[2].offset == 0x2000);
    CHECK(layout->segments[2].size == 0x3000);
}

TEST_CASE("NRO layout takes .bss from the module header", "[core][loader]") {
    auto bytes = MakeNro();
    PutMod0(bytes, 0x100, 0x2F00, 0x2F00 + 0x3800);
    const auto layout = Loader::ValidateNroLayout(bytes);
    REQUIRE(layout);
    CHECK(layout->image_size == 0x3000 + 0x4000);
    CHECK(layout->segments[2].size == 0x1000 + 0x4000);
}

TEST_CASE("NRO layout does not use a module header outside the file", "[core][loader]") {
    auto bytes = MakeNro();
    Put32(bytes, 0x4, 0x3000 - 0x10);
    const auto layout = Loader::ValidateNroLayout(bytes);
    REQUIRE(layout);
    CHECK(layout->image_size == 0x5000);
}

TEST_CASE("NRO layout of a file whose size is not page aligned", "[core][loader]") {
    // The file holds exactly its header's size: the image is page aligned, the copy is not.
    const auto bytes = MakeNro(Text, RoData, {0x2000, 0x800}, 0x2800, 0, 0x2800);
    const auto layout = Loader::ValidateNroLayout(bytes);
    REQUIRE(layout);
    CHECK(layout->file_size == 0x2800);
    CHECK(layout->image_size == 0x3000);
}

TEST_CASE("NRO layout rejects a damaged NRO", "[core][loader]") {
    const auto rejects = [](const std::vector<u8>& bytes) {
        CHECK_FALSE(Loader::ValidateNroLayout(bytes));
    };
    SECTION("a size larger than the file") {
        rejects(MakeNro(Text, RoData, Data, 0x10000, 0, 0x3000));
    }
    SECTION("a size smaller than its header") {
        rejects(MakeNro({0, 0}, {0, 0}, {0, 0}, 0x40, 0, 0x3000));
    }
    SECTION("a segment past the NRO's size") {
        rejects(MakeNro(Text, RoData, {0x2000, 0x1001}, 0x3000, 0, 0x4000));
    }
    SECTION(".text not at 0") {
        rejects(MakeNro({0x1000, 0x1000}, {0x2000, 0x800}, {0x2800, 0x800}, 0x3000, 0, 0x3000));
    }
    SECTION("a segment off a page") {
        rejects(MakeNro(Text, {0x1800, 0x800}, Data, 0x3000, 0, 0x3000));
    }
    SECTION("a segment that runs into the next") {
        rejects(MakeNro({0, 0x1800}, RoData, Data, 0x3000, 0, 0x3000));
    }
    SECTION("segments out of order") {
        rejects(MakeNro(Text, Data, RoData, 0x3000, 0, 0x3000));
    }
    SECTION("a module header whose .bss ends before it starts") {
        auto bytes = MakeNro();
        PutMod0(bytes, 0x100, 0x2000, 0);
        rejects(bytes);
    }
    SECTION("an image larger than a code set segment can describe") {
        rejects(MakeNro(0xFFFFF000));
    }
    SECTION("too short for a header, or not an NRO") {
        rejects(std::vector<u8>(0x40));
        auto bytes = MakeNro();
        Put32(bytes, 0x10, 0);
        rejects(bytes);
    }
}
