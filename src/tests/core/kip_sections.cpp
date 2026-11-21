// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace {

using namespace FileSys;

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

} // Anonymous namespace

TEST_CASE("KIP does not allocate a .bss the file holds no data for", "[core][file_sys]") {
    constexpr std::string_view text = "text";
    constexpr u32 bss_size = 0x10000000;
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    Put32(bytes, 0x24, static_cast<u32>(text.size())); // .text, stored
    Put32(bytes, 0x28, static_cast<u32>(text.size()));
    Put32(bytes, 0x50, 0x1000); // .bss offset
    Put32(bytes, 0x54, bss_size);
    bytes.insert(bytes.end(), text.begin(), text.end());

    const KIP kip{std::make_shared<VectorVfsFile>(std::move(bytes), "test.kip")};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    CHECK(kip.GetTextSection().size() == text.size());
    CHECK(kip.GetBSSOffset() == 0x1000);
    CHECK(kip.GetBSSSize() == bss_size);
    // This used to be a 256 MiB vector of zeros.
    CHECK(kip.GetSectionDecompressed(3).empty());
}

namespace {

constexpr u32 DamagedSize = 0xABABAB94;

// A KIP whose .text is stored raw, with `section` given no stored bytes and `memory_size` bytes in
// memory, and `flags` in the header (bits 0 to 2 mark .text, .rodata and .data as compressed).
std::vector<u8> KipWithEmptySection(std::size_t section, u32 memory_size, u8 flags) {
    constexpr std::string_view text = "text";
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    bytes[0x1F] = flags;
    if (section != 0) {
        Put32(bytes, 0x24, static_cast<u32>(text.size()));
        Put32(bytes, 0x28, static_cast<u32>(text.size()));
    }
    Put32(bytes, 0x20 + section * 0x10 + 0x4, memory_size);
    Put32(bytes, 0x20 + section * 0x10 + 0x8, 0);
    if (section != 0) {
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
    return bytes;
}

} // Anonymous namespace

TEST_CASE("KIP rejects .text, .rodata or .data with no stored bytes before allocating them",
          "[core][file_sys]") {
    for (std::size_t section = 0; section < 3; ++section) {
        for (const u8 flags : {u8{0}, static_cast<u8>(1U << section)}) {
            INFO("section " << section << " flags " << int{flags});
            // This used to allocate 0xABABAB94 bytes (2.9 GB) or throw std::bad_alloc.
            const KIP kip{std::make_shared<VectorVfsFile>(
                KipWithEmptySection(section, DamagedSize, flags), "test.kip")};
            CHECK(kip.GetStatus() == Loader::ResultStatus::ErrorBadKIPHeader);
            CHECK(kip.GetSectionDecompressed(static_cast<u8>(section)).empty());
        }
    }
}

TEST_CASE("KIP keeps an empty section that is empty in memory too", "[core][file_sys]") {
    for (std::size_t section = 1; section < 3; ++section) {
        INFO("section " << section);
        const KIP kip{std::make_shared<VectorVfsFile>(KipWithEmptySection(section, 0, 0),
                                                      "test.kip")};
        REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
        CHECK(kip.GetTextSection().size() == 4);
        CHECK(kip.GetSectionDecompressed(static_cast<u8>(section)).empty());
    }
}

TEST_CASE("KIP bounds a BLZ section's output by its stored bytes", "[core][file_sys]") {
    // Twelve stored bytes, all footer, whose added size asks for nearly 4 GiB: the decompressor
    // allows at most nine times the stored bytes plus 144, so it fails before resizing.
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    bytes[0x1F] = 0x1;
    Put32(bytes, 0x24, DamagedSize);
    Put32(bytes, 0x28, 0xC);
    std::vector<u8> footer(0xC);
    Put32(footer, 0, 0xC);
    Put32(footer, 4, 0xC);
    Put32(footer, 8, 0xFFFFFFF0);
    bytes.insert(bytes.end(), footer.begin(), footer.end());
    const KIP kip{std::make_shared<VectorVfsFile>(std::move(bytes), "test.kip")};
    CHECK(kip.GetStatus() == Loader::ResultStatus::ErrorBLZDecompressionFailed);
}
