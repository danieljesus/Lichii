// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <openssl/evp.h>

#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/nca.h"

namespace {

using namespace FileSys;

/// A file whose reads stop returning data at `cutoff` (a damaged .ncz view, or an I/O error), and
/// which hands out at most `chunk` bytes per read.
class ShortReadFile final : public VectorVfsFile {
public:
    ShortReadFile(std::vector<u8> data, std::string name, std::size_t cutoff_, std::size_t chunk_)
        : VectorVfsFile(std::move(data), std::move(name)), cutoff{cutoff_}, chunk{chunk_} {}

    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        if (offset >= cutoff) {
            return 0;
        }
        return VectorVfsFile::Read(data, std::min({length, chunk, cutoff - offset}), offset);
    }

private:
    std::size_t cutoff;
    std::size_t chunk;
};

std::vector<u8> Contents() {
    std::vector<u8> data(3 * 4096 + 123);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<u8>(i * 7 + 3);
    }
    return data;
}

/// The name an NCA with these contents has: the first half of its SHA-256, in hex, then ".nca".
std::string NcaName(const std::vector<u8>& data) {
    std::array<u8, 32> hash{};
    unsigned int size = 0;
    REQUIRE(EVP_Digest(data.data(), data.size(), hash.data(), &size, EVP_sha256(), nullptr) == 1);
    std::string name;
    for (std::size_t i = 0; i < 16; ++i) {
        name += fmt::format("{:02x}", hash[i]);
    }
    return name + ".nca";
}

/// Runs VerifyIntegrity, counting the progress calls; past `limit` calls it cancels, so a loop
/// that no longer advances ends there instead of hanging the test.
Loader::ResultStatus Verify(const VirtualFile& file, std::size_t& calls, std::size_t limit = 1000) {
    Loader::AppLoader_NCA loader{file};
    return loader.VerifyIntegrity([&calls, limit](std::size_t, std::size_t) {
        return ++calls < limit;
    });
}

} // Anonymous namespace

TEST_CASE("NCA verification fails on a read that stops returning data", "[core][loader]") {
    const auto data = Contents();
    const auto file = std::make_shared<ShortReadFile>(data, NcaName(data), 5000, data.size());
    std::size_t calls = 0;
    CHECK(Verify(file, calls) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    // It used to read 0 bytes forever, calling the progress function with no progress until the
    // user cancelled; here that would be the 1000th call.
    CHECK(calls < 10);
}

TEST_CASE("NCA verification passes a whole file, also when it reads in pieces", "[core][loader]") {
    const auto data = Contents();
    for (const std::size_t chunk : {data.size(), std::size_t{1000}}) {
        INFO("chunk " << chunk);
        const auto file = std::make_shared<ShortReadFile>(data, NcaName(data), data.size(), chunk);
        std::size_t calls = 0;
        CHECK(Verify(file, calls) == Loader::ResultStatus::Success);
    }
}
