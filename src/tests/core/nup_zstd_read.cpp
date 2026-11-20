// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <openssl/evp.h>

#include "common/zstd_compression.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {

namespace fs = std::filesystem;
using FileSys::NupContainer;

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("nup-read-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Put(std::vector<u8>& bytes, std::size_t offset, u64 value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (8 * i));
    }
}

// Compressible bytes: literals from a skewed distribution and copies of recent data.
std::vector<u8> Content(std::size_t size, u64 seed) {
    std::vector<u8> out(size);
    std::mt19937_64 rng{seed};
    std::size_t pos = 0;
    while (pos < size) {
        const u64 r = rng();
        const std::size_t length = std::min<std::size_t>(8 + (r & 127), size - pos);
        if (pos > 1024 && (r >> 8) % 3 != 0) {
            const std::size_t back = 1 + (r >> 16) % std::min<std::size_t>(pos, 256 * 1024);
            for (std::size_t i = 0; i < length; ++i) {
                out[pos + i] = out[pos - back + i];
            }
        } else {
            for (std::size_t i = 0; i < length; ++i) {
                const u64 x = rng();
                out[pos + i] = static_cast<u8>(x & (x >> 8));
            }
        }
        pos += length;
    }
    return out;
}

std::vector<u8> Zstd(std::span<const u8> data) {
    return Common::Compression::CompressDataZSTD(data.data(), data.size(), 3);
}

struct Member {
    std::vector<u8> extent;
    u64 size;
    u8 codec;
    std::string name{};
    std::array<u8, 0x20> sha256{};
};

std::array<u8, 0x20> Sha256(std::span<const u8> data) {
    std::array<u8, 0x20> hash{};
    unsigned int length{};
    EVP_Digest(data.data(), data.size(), hash.data(), &length, EVP_sha256(), nullptr);
    return hash;
}

Member ZstdMember(const std::vector<u8>& content) {
    return {Zstd(content), content.size(), 1};
}

// A single-package container whose root directory holds the members as files 0..n-1.
std::vector<u8> BuildNup(const std::vector<Member>& members) {
    constexpr std::size_t TocOffset = 0x80;
    const std::size_t files_at = 0x30 + 0x40 + 0x18;
    const std::size_t children_at = files_at + members.size() * 0x50;
    const std::size_t pool_at = children_at + members.size() * 4;
    std::string pool;
    for (const auto& member : members) {
        pool += member.name;
    }
    const std::size_t toc_size = pool_at + pool.size();
    const std::size_t blob_offset = (TocOffset + toc_size + 15) & ~std::size_t{15};
    std::vector<u8> bytes(blob_offset);
    std::vector<u64> extent_offsets;
    for (const auto& member : members) {
        bytes.resize((bytes.size() + 15) & ~std::size_t{15});
        extent_offsets.push_back(bytes.size());
        bytes.insert(bytes.end(), member.extent.begin(), member.extent.end());
    }

    std::copy(NupContainer::HeaderMagic.begin(), NupContainer::HeaderMagic.end(), bytes.begin());
    Put(bytes, 0x04, 0x80, 2);
    Put(bytes, 0x08, TocOffset, 8);
    Put(bytes, 0x10, toc_size, 8);
    Put(bytes, 0x38, blob_offset, 8);
    Put(bytes, 0x40, bytes.size(), 8);
    Put(bytes, 0x48, 1, 4);
    const std::size_t toc = TocOffset;
    Put(bytes, toc + 0x00, 0x01434F54, 4);
    Put(bytes, toc + 0x04, 1, 4);
    Put(bytes, toc + 0x08, 1, 8);
    Put(bytes, toc + 0x10, members.size(), 8);
    Put(bytes, toc + 0x18, pool_at, 8);
    Put(bytes, toc + 0x20, pool.size(), 8);
    std::copy(pool.begin(), pool.end(), bytes.begin() + toc + pool_at);
    Put(bytes, toc + 0x30 + 0x0C, 0xFFFFFFFF, 4);
    Put(bytes, toc + 0x30 + 0x40 + 0x0C, members.size(), 4);
    std::size_t name_offset = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const std::size_t record = toc + files_at + i * 0x50;
        Put(bytes, record + 0x00, name_offset, 4);
        Put(bytes, record + 0x04, members[i].name.size(), 4);
        name_offset += members[i].name.size();
        Put(bytes, record + 0x08, members[i].size, 8);
        Put(bytes, record + 0x10, extent_offsets[i], 8);
        Put(bytes, record + 0x18, members[i].extent.size(), 8);
        std::copy(members[i].sha256.begin(), members[i].sha256.end(), bytes.begin() + record + 0x20);
        bytes[record + 0x40] = members[i].codec;
        Put(bytes, toc + children_at + i * 4, 1 + i, 4);
    }
    unsigned int length{};
    EVP_Digest(bytes.data() + toc, toc_size, bytes.data() + 0x18, &length, EVP_sha256(), nullptr);
    return bytes;
}

FileSys::VirtualFile Backing(std::vector<u8> bytes) {
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes));
}

bool ReadMatches(const FileSys::VirtualFile& file, const std::vector<u8>& content,
                 std::size_t offset, std::size_t length) {
    std::vector<u8> buffer(length);
    const std::size_t expected =
        std::min(length, content.size() - std::min(offset, content.size()));
    return file->Read(buffer.data(), length, offset) == expected &&
           std::equal(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(expected),
                      content.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::size_t DirectoryBytes(const fs::path& dir) {
    std::size_t total = 0;
    // Windows directory entries can report a stale size for a file that is still open.
    for (const auto& entry : fs::directory_iterator(dir)) {
        std::ifstream stream{entry.path(), std::ios::binary | std::ios::ate};
        total += static_cast<std::size_t>(stream.tellg());
    }
    return total;
}

} // namespace

TEST_CASE("NUP zstd random reads match the source in memory and spilled", "[nup][nup-read]") {
    const std::vector<std::size_t> sizes{1, 100, 64 * 1024, 300 * 1024, 3 * 1024 * 1024};
    std::vector<std::vector<u8>> contents;
    std::vector<Member> members;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        contents.push_back(Content(sizes[i], i + 1));
        members.push_back(ZstdMember(contents.back()));
    }
    const auto bytes = BuildNup(members);
    TempDir t;
    // 64 MiB keeps every member in memory; 128 KiB spills those above 64 KiB.
    for (const std::size_t cap : {std::size_t{64} << 20, std::size_t{128} << 10}) {
        NupContainer nup{Backing(bytes), cap, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        u8 byte{};
        REQUIRE(nup.OpenFile(4)->Read(&byte, 0, 10) == 0);
        std::mt19937_64 rng{cap};
        for (int i = 0; i < 400; ++i) {
            const std::size_t member = rng() % contents.size();
            const auto& content = contents[member];
            const std::size_t offset = rng() % (content.size() + 1);
            const std::size_t length = 1 + rng() % (200 * 1024);
            REQUIRE(ReadMatches(nup.OpenFile(member), content, offset, length));
        }
        for (std::size_t i = 0; i < contents.size(); ++i) {
            REQUIRE(nup.OpenFile(i)->ReadAllBytes() == contents[i]);
        }
    }
}

TEST_CASE("NUP zstd reads are safe across threads and evictions", "[nup][nup-read]") {
    std::vector<std::vector<u8>> contents;
    std::vector<Member> members;
    for (int i = 0; i < 6; ++i) {
        contents.push_back(Content(std::size_t{1} << 20, 100 + i));
        members.push_back(ZstdMember(contents.back()));
    }
    const auto bytes = BuildNup(members);
    TempDir t;
    // 4 MiB holds three 1 MiB members at a time; 1 MiB spills them all.
    for (const std::size_t cap : {std::size_t{4} << 20, std::size_t{1} << 20}) {
        NupContainer nup{Backing(bytes), cap, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        std::vector<FileSys::VirtualFile> files;
        for (std::size_t i = 0; i < contents.size(); ++i) {
            files.push_back(nup.OpenFile(i));
        }
        std::atomic<int> mismatches{};
        std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < 8; ++worker) {
            threads.emplace_back([&, worker] {
                std::mt19937_64 rng{worker * 7919 + cap};
                if (worker % 2 == 0) {
                    // Streams that decode the member a little at a time, racing each other.
                    const std::size_t member = worker / 2;
                    for (std::size_t offset = 0; offset < contents[member].size();
                         offset += 8 * 1024) {
                        mismatches += !ReadMatches(files[member], contents[member], offset, 8192);
                    }
                    return;
                }
                for (int i = 0; i < 300; ++i) {
                    const std::size_t member = rng() % contents.size();
                    const std::size_t offset = rng() % contents[member].size();
                    const std::size_t length = 1 + rng() % (64 * 1024);
                    mismatches += !ReadMatches(files[member], contents[member], offset, length);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        REQUIRE(mismatches == 0);
    }
}

TEST_CASE("NUP zstd members of several frames decode in memory and spilled", "[nup][nup-read]") {
    const auto first = Content(200 * 1024, 7);
    const auto second = Content(300 * 1024, 8);
    std::vector<u8> content = first;
    content.insert(content.end(), second.begin(), second.end());
    Member member{Zstd(first), content.size(), 1};
    const auto tail = Zstd(second);
    member.extent.insert(member.extent.end(), tail.begin(), tail.end());
    const auto bytes = BuildNup({member});
    TempDir t;
    for (const std::size_t cap : {std::size_t{64} << 20, std::size_t{64} << 10}) {
        NupContainer nup{Backing(bytes), cap, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(ReadMatches(nup.OpenFile(0), content, content.size() - 5000, 5000));
        REQUIRE(nup.OpenFile(0)->ReadAllBytes() == content);
    }
}

TEST_CASE("NUP zstd spill file grows only as far as reads need", "[nup][nup-read]") {
    const auto content = Content(8 << 20, 3);
    const auto bytes = BuildNup({ZstdMember(content)});
    TempDir t;
    NupContainer nup{Backing(bytes), 1 << 20, t.path};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto file = nup.OpenFile(0);
    REQUIRE(ReadMatches(file, content, 0, 4096));
    REQUIRE(DirectoryBytes(t.path) < content.size() / 4);
    REQUIRE(ReadMatches(file, content, content.size() - 4096, 4096));
    REQUIRE(DirectoryBytes(t.path) == content.size());
}

TEST_CASE("NUP zstd reads fail cleanly on corrupt or truncated extents", "[nup][nup-read]") {
    const auto content = Content(2 << 20, 5);
    const auto good = ZstdMember(content);
    TempDir t;
    for (const std::size_t cap : {std::size_t{64} << 20, std::size_t{64} << 10}) {
        DYNAMIC_SECTION("truncated, cap " << cap) {
            Member truncated = good;
            truncated.extent.resize(truncated.extent.size() / 2);
            NupContainer nup{Backing(BuildNup({truncated})), cap, t.path};
            REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
            const auto file = nup.OpenFile(0);
            // The intact prefix still reads; anything past the cut fails every time.
            REQUIRE(ReadMatches(file, content, 1000, 4096));
            std::vector<u8> buffer(4096);
            for (int attempt = 0; attempt < 2; ++attempt) {
                REQUIRE(file->Read(buffer.data(), buffer.size(), content.size() - 4096) == 0);
            }
            REQUIRE(file->ReadAllBytes().empty());
            REQUIRE(nup.VerifyIntegrity({}) ==
                    Loader::ResultStatus::ErrorIntegrityVerificationFailed);
        }
        DYNAMIC_SECTION("bad frame header, cap " << cap) {
            Member corrupt = good;
            std::fill_n(corrupt.extent.begin(), 8, u8{0xA5});
            NupContainer nup{Backing(BuildNup({corrupt})), cap, t.path};
            REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
            std::vector<u8> buffer(4096);
            REQUIRE(nup.OpenFile(0)->Read(buffer.data(), buffer.size(), 0) == 0);
        }
        DYNAMIC_SECTION("recorded size smaller than the frame, cap " << cap) {
            Member oversized = good;
            oversized.size = content.size() - 1000;
            NupContainer nup{Backing(BuildNup({oversized})), cap, t.path};
            REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
            REQUIRE(nup.OpenFile(0)->ReadAllBytes().empty());
        }
        DYNAMIC_SECTION("recorded size ending on a decoder output block, cap " << cap) {
            // zstd hands spilled output over in 128 KiB blocks, so the recorded size is reached
            // exactly at a block end while the frame still holds more.
            Member oversized = good;
            oversized.size = content.size() - 128 * 1024;
            NupContainer nup{Backing(BuildNup({oversized})), cap, t.path};
            REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
            const auto file = nup.OpenFile(0);
            std::vector<u8> buffer(4096);
            REQUIRE(file->Read(buffer.data(), buffer.size(), oversized.size - 1) == 0);
            REQUIRE(file->ReadAllBytes().empty());
        }
        DYNAMIC_SECTION("recorded size larger than the frame, cap " << cap) {
            Member undersized = good;
            undersized.size = content.size() + 1000;
            NupContainer nup{Backing(BuildNup({undersized})), cap, t.path};
            REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
            REQUIRE(nup.OpenFile(0)->ReadAllBytes().empty());
        }
    }
}

TEST_CASE("NUP zstd spilled members resume after their decoder is freed", "[nup][nup-read]") {
    std::vector<std::vector<u8>> contents;
    std::vector<Member> members;
    for (int i = 0; i < 5; ++i) {
        contents.push_back(Content(std::size_t{2} << 20, 200 + i));
        members.push_back(ZstdMember(contents.back()));
    }
    TempDir t;
    NupContainer nup{Backing(BuildNup(members)), 1 << 20, t.path};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    // Starting every member leaves only the newest decoders alive; the older members decode
    // again from their first frame when read further.
    for (int round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < contents.size(); ++i) {
            const std::size_t offset = static_cast<std::size_t>(round) * 512 * 1024;
            REQUIRE(ReadMatches(nup.OpenFile(i), contents[i], offset, 4096));
        }
    }
    for (std::size_t i = 0; i < contents.size(); ++i) {
        REQUIRE(nup.OpenFile(i)->ReadAllBytes() == contents[i]);
    }
}

TEST_CASE("NUP serves a sound sibling in place of a .zs member with no zstd header",
          "[nup][nup-read]") {
    const auto zstd_file = [](u64 seed) { return Zstd(Content(200 * 1024, seed)); };
    auto damaged = zstd_file(1);
    std::fill_n(damaged.begin(), 12288, u8{0});
    const auto stand_in = zstd_file(2);
    const auto other_prefix = zstd_file(3);
    const auto other_kind = zstd_file(4);
    auto lonely = zstd_file(5);
    std::fill_n(lonely.begin(), 12288, u8{0});
    const std::vector<u8> zeros(4096, 0);
    Member unreadable = ZstdMember(zstd_file(6));
    std::fill_n(unreadable.extent.begin(), 8, u8{0xA5});

    const auto named = [](Member member, std::string name) {
        member.name = std::move(name);
        return member;
    };
    const std::vector<Member> members{
        named(ZstdMember(damaged), "Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs"),
        named(ZstdMember(stand_in), "Rock_WaterTemple_Gate_P_01_Nrm.bntx.zs"),
        named({other_prefix, other_prefix.size(), 0}, "Rock_WaterTemple_Wall_A_01_Nrm.bntx.zs"),
        named(ZstdMember(other_kind), "Rock_WaterTemple_Gate_M_01_Alb.bntx.zs"),
        named(ZstdMember(lonely), "Lonely_Mask.bntx.zs"),
        named(unreadable, "Broken_A_01_Nrm.bntx.zs"),
        named(ZstdMember(zeros), "Model.bfres"),
    };
    TempDir t;
    NupContainer nup{Backing(BuildNup(members)), std::size_t{64} << 20, t.path};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto dir = nup.GetRomFSDir(0);
    REQUIRE(dir != nullptr);
    REQUIRE(dir->GetFiles().size() == members.size());
    const auto read = [&](std::string_view name) {
        const auto file = dir->GetFile(name);
        REQUIRE(file != nullptr);
        REQUIRE(file->GetName() == name);
        return file->ReadAllBytes();
    };

    // Same last name token, then the longest common prefix.
    REQUIRE(read("Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs") == stand_in);
    REQUIRE(read("Rock_WaterTemple_Gate_P_01_Nrm.bntx.zs") == stand_in);
    REQUIRE(read("Rock_WaterTemple_Wall_A_01_Nrm.bntx.zs") == other_prefix);
    REQUIRE(read("Rock_WaterTemple_Gate_M_01_Alb.bntx.zs") == other_kind);
    // Without a sibling of its kind a damaged member is served as stored.
    REQUIRE(read("Lonely_Mask.bntx.zs") == lonely);
    // A member that cannot be decoded is not known to be damaged, so it is not replaced.
    REQUIRE(read("Broken_A_01_Nrm.bntx.zs").empty());
    REQUIRE(read("Model.bfres") == zeros);

    // Extraction reads the package tree, which keeps the stored bytes.
    const auto package = nup.GetPackageDir(0);
    REQUIRE(package != nullptr);
    REQUIRE(package->GetFile("Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs")->ReadAllBytes() == damaged);
}

TEST_CASE("Verifying a NUP hashes its members a chunk at a time", "[nup][nup-read]") {
    // The verifier reads 1 MiB at a time: members of exactly one chunk, of one chunk and a part,
    // stored and compressed, in memory and spilled.
    constexpr std::size_t Chunk = 1 << 20;
    const auto exact = Content(Chunk, 11);
    const auto longer = Content(Chunk + 12345, 12);
    std::vector<Member> members{
        {exact, exact.size(), 0, {}, Sha256(exact)},
        {longer, longer.size(), 0, {}, Sha256(longer)},
        {Zstd(exact), exact.size(), 1, {}, Sha256(exact)},
        {Zstd(longer), longer.size(), 1, {}, Sha256(longer)},
    };
    const std::size_t total = exact.size() * 2 + longer.size() * 2;
    TempDir t;
    for (const std::size_t cap : {std::size_t{64} << 20, std::size_t{64} << 10}) {
        CAPTURE(cap);
        NupContainer nup{Backing(BuildNup(members)), cap, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        std::vector<std::pair<std::size_t, std::size_t>> reports;
        REQUIRE(nup.VerifyIntegrity([&](std::size_t done, std::size_t all) {
            reports.emplace_back(done, all);
            return true;
        }) == Loader::ResultStatus::Success);
        // Progress is in bytes, once per chunk, and ends at the total.
        REQUIRE(reports.size() > members.size());
        REQUIRE(std::ranges::is_sorted(reports));
        REQUIRE(reports.back() == std::pair{total, total});

        // A cancel stops it at the chunk it was reported for.
        std::size_t calls{};
        REQUIRE(nup.VerifyIntegrity([&](std::size_t, std::size_t) { return ++calls < 2; }) ==
                Loader::ResultStatus::ErrorIntegrityVerificationFailed);
        REQUIRE(calls == 2);
    }

    // A changed byte past the first chunk is found.
    auto changed = members;
    changed[1].extent[Chunk + 100] ^= 1;
    NupContainer nup{Backing(BuildNup(changed)), std::size_t{64} << 20, t.path};
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
}

TEST_CASE("Verifying a NUP whose member holds less than its record declares fails",
          "[nup][nup-read]") {
    const auto content = Content(3 << 20, 13);
    Member shorter = ZstdMember(content);
    shorter.size = content.size() + 1000;
    shorter.sha256 = Sha256(content);
    TempDir t;
    for (const std::size_t cap : {std::size_t{64} << 20, std::size_t{64} << 10}) {
        CAPTURE(cap);
        NupContainer nup{Backing(BuildNup({shorter})), cap, t.path};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
    }
}

TEST_CASE("Verifying a NUP with an impossible member size fails without reading it whole",
          "[nup][nup-read]") {
    // Found by fuzz_nup: a compressed member records 0x250000000200 bytes. Verification read each
    // member whole, so this allocated that size up front and std::bad_alloc ended the emulator
    // ("Verify integrity" on a damaged .nup).
    static constexpr std::array<u8, 986> Damaged{
    0x4E, 0x55, 0x50, 0x01, 0x80, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x50, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x44, 0x24, 0xEF, 0x29, 0x49, 0xAB, 0x8F,
    0x38, 0x5B, 0xA0, 0x0C, 0x98, 0x25, 0x1C, 0x82, 0xA6, 0x1E, 0xEA, 0xB0, 0x4E, 0xBC, 0xBA, 0xD2,
    0x05, 0xDF, 0x27, 0xF5, 0xA0, 0xB7, 0x3D, 0x96, 0xD0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xDA, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xDD, 0x46, 0xC3, 0xEE, 0xBB, 0x18, 0x84, 0xFF, 0x3B, 0x52, 0x58, 0xC0, 0xA2, 0xFC, 0x93, 0x98,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF6, 0x5D, 0x5F, 0xED, 0x2C, 0x6E, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x54, 0x4F, 0x43, 0x01, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x41, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0D, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xD0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x5A, 0x97, 0xBA, 0x4F, 0x4D, 0xA7, 0xAB, 0x92, 0x9E, 0xBE, 0x51, 0x65, 0x8D, 0xCE, 0x88, 0x5B,
    0x3D, 0x81, 0xDE, 0x6B, 0x89, 0xBC, 0xB2, 0x6E, 0xA5, 0xDF, 0x96, 0x37, 0xD1, 0xFA, 0xA9, 0x69,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x11, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xF0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x11, 0xE7, 0xF5, 0xA6, 0xF1, 0x5A, 0x4A, 0xDD, 0xBA, 0x9B, 0x6B, 0x21, 0xBC, 0x4F, 0x8E, 0xCB,
    0xDD, 0x96, 0x9E, 0x17, 0x93, 0x35, 0x26, 0x9F, 0xC6, 0x8D, 0x3A, 0x05, 0xF0, 0xF3, 0xDA, 0x4A,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1A, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x32, 0xBE, 0xEC, 0xB5, 0x8A, 0x12, 0x8A, 0xF8, 0x24, 0x85, 0x04, 0x60, 0x0B, 0xD2, 0x03, 0xDC,
    0xC6, 0x76, 0xAD, 0xF4, 0x10, 0x45, 0x30, 0x04, 0x85, 0x65, 0x5E, 0x6B, 0x87, 0x80, 0xA0, 0x1D,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1F, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x72, 0x02, 0x28, 0xE4, 0xB7, 0xB0, 0x18, 0xB5, 0xE0, 0xC8, 0xC5, 0xDC, 0xC1, 0x5B, 0x89, 0x55,
    0x17, 0x5F, 0xA5, 0xE5, 0x82, 0x6C, 0x7E, 0x80, 0xC2, 0x67, 0xF2, 0xA2, 0xD3, 0x97, 0xD0, 0xE0,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x65, 0x78, 0x65, 0x66,
    0x73, 0x72, 0x6F, 0x6D, 0x66, 0x73, 0x73, 0x75, 0x62, 0x6D, 0x61, 0x69, 0x6E, 0x6D, 0x61, 0x69,
    0x6E, 0x2E, 0x6E, 0x70, 0x64, 0x6D, 0x61, 0x2E, 0x62, 0x69, 0x6E, 0x62, 0x2E, 0x74, 0x78, 0x74,
    0x28, 0xB5, 0x2F, 0xFD, 0x60, 0x00, 0x02, 0x4D, 0x00, 0x00, 0x10, 0x4D, 0x4D, 0x01, 0x00, 0xFB,
    0x2A, 0x80, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x80, 0x45, 0x00, 0x00, 0x10, 0x4E, 0x4E, 0x01, 0x00, 0x3B, 0x05,
    0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x27, 0xB5, 0x2F, 0xFD, 0x04, 0x00, 0xEF, 0x00, 0x4E, 0xFF, 0x60, 0xBC, 0xBE, 0xFE, 0x08, 0xFB,
    0x52, 0xC0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF,
    0x27, 0xB5, 0x2F, 0xFD, 0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x10, 0x62, 0x62, 0x01, 0x00, 0x9B, 0x00,
    0x0B, 0x01, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E,
    0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E,
    0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4C, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E,
    0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E,
    0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E,
    0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E,
    0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E,
    0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E,
    0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x4E, 0x08, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
    0x00, 0x73, 0x65, 0x65, 0x64, 0x2D, 0x64, 0x75, 0x6D, 0x70,
    };
    TempDir t;
    NupContainer nup{Backing({Damaged.begin(), Damaged.end()}), std::size_t{8} << 20, t.path};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    Loader::ResultStatus status{};
    REQUIRE_NOTHROW(status = nup.VerifyIntegrity({}));
    REQUIRE(status == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
}
