// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/program_metadata.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace {

using namespace FileSys;

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

// A 0x480-byte NPDM: ACID at 0x100, ACI0 at 0x400, and the kernel capability descriptors at
// ACI0 + kac_offset, the bytes from 0x460 on being 0x41.
VirtualFile MakeNpdm(u32 kac_size, u32 kac_offset = 0x60) {
    std::vector<u8> bytes(0x480);
    std::memcpy(bytes.data(), "META", 4);
    Put32(bytes, 0x70, 0x400); // aci_offset
    Put32(bytes, 0x74, 0x40);
    Put32(bytes, 0x78, 0x100); // acid_offset
    Put32(bytes, 0x7C, 0x240);
    std::memcpy(bytes.data() + 0x300, "ACID", 4);
    Put32(bytes, 0x320, 0x240); // fac_offset
    Put32(bytes, 0x324, 0x2C);
    std::memcpy(bytes.data() + 0x400, "ACI0", 4);
    Put32(bytes, 0x420, 0x40); // fah_offset
    Put32(bytes, 0x424, 0x1C);
    Put32(bytes, 0x430, kac_offset);
    Put32(bytes, 0x434, kac_size);
    std::memset(bytes.data() + 0x460, 0x41, 0x20);
    return std::make_shared<VectorVfsFile>(std::move(bytes), "main.npdm");
}

} // Anonymous namespace

TEST_CASE("NPDM reads its kernel capabilities", "[core][file_sys]") {
    ProgramMetadata npdm;
    REQUIRE(npdm.Load(MakeNpdm(8)) == Loader::ResultStatus::Success);
    REQUIRE(npdm.GetKernelCapabilities() == std::vector<u32>{0x41414141, 0x41414141});
}

TEST_CASE("NPDM reads only whole kernel capability words", "[core][file_sys]") {
    // Five bytes used to be read into a one-word vector, one byte past its end.
    ProgramMetadata npdm;
    REQUIRE(npdm.Load(MakeNpdm(5)) == Loader::ResultStatus::Success);
    REQUIRE(npdm.GetKernelCapabilities() == std::vector<u32>{0x41414141});
}

TEST_CASE("NPDM rejects kernel capabilities larger than the file", "[core][file_sys]") {
    // Rejected before the descriptors are allocated: this used to allocate 3.75 GiB.
    ProgramMetadata npdm;
    REQUIRE(npdm.Load(MakeNpdm(0xF0000000)) ==
            Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors);
}

TEST_CASE("NPDM rejects a kernel capability offset that wraps past 4 GiB", "[core][file_sys]") {
    // 0x400 + 0xFFFFFC60 wrapped to 0x60 in 32 bits, which read the NPDM header as capabilities.
    ProgramMetadata npdm;
    REQUIRE(npdm.Load(MakeNpdm(8, 0xFFFFFC60)) ==
            Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors);
}
