// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/scope_exit.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "tests/common/restore_eden_path.h"

namespace {

using namespace Core::Crypto;

Key128 Fill(u8 value) {
    Key128 key;
    key.fill(value);
    return key;
}

/// A temporary folder this run makes itself (create_directory fails on one that exists), so
/// another tests process can't remove or fill it meanwhile.
std::filesystem::path MakeTemporaryFolder(const std::string& prefix) {
    std::random_device random;
    std::filesystem::path directory;
    do {
        directory = std::filesystem::temp_directory_path() / (prefix + std::to_string(random()));
    } while (!std::filesystem::create_directory(directory));
    return directory;
}

Key128 Decrypt(const Key128& key, const Key128& data) {
    Key128 out{};
    AESCipher<Key128> cipher(key, Mode::ECB);
    cipher.Transcode(data.data(), data.size(), out.data(), Op::Decrypt);
    return out;
}

TEST_CASE("Keys of a generation the key file lacks are derived from its sources", "[core][crypto]") {
    auto& keys = KeyManager::Instance();
    if (keys.HasKey(S128KeyType::Source, u64(SourceKeyType::Master)) ||
        keys.HasKey(S128KeyType::Master, 0x1F)) {
        SKIP("Keys are installed");
    }

    const auto directory = MakeTemporaryFolder("lichii-key-derivation-test-");
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    };
    {
        std::ofstream file{directory / "prod.keys"};
        const auto line = [&file](const std::string& name, u8 value) {
            file << name << " = " << Common::HexToString(Fill(value)) << '\n';
        };
        line("master_key_source", 1);
        line("tsec_root_key_02", 2);
        line("master_kek_source_1f", 3);
        line("key_area_key_application_source", 4);
        line("aes_kek_generation_source", 5);
        line("aes_key_generation_source", 6);
        line("titlekek_source", 7);
    }

    const auto previous = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, directory);
    keys.ReloadKeys();
    Tests::RestoreEdenPath(Common::FS::EdenPath::KeysDir, previous);

    const auto master = Decrypt(Decrypt(Fill(2), Fill(3)), Fill(1));
    CHECK(keys.GetKey(S128KeyType::Master, 0x1F) == master);
    CHECK(keys.GetKey(S128KeyType::KeyArea, 0x1F, u64(KeyAreaKeyType::Application)) ==
          GenerateKeyEncryptionKey(Fill(4), master, Fill(5), Fill(6)));
    CHECK(keys.GetKey(S128KeyType::Titlekek, 0x1F) == Decrypt(master, Fill(7)));
}

TEST_CASE("Keyblob lines past the keyblob table are skipped", "[core][crypto]") {
    // The table holds keyblobs 00 to 1f; ff used to be written about 36 KiB past it. Under
    // AddressSanitizer the old code fails here with a global-buffer-overflow. The file holds
    // no other key, so the keys the other tests see are left as they were.
    auto& keys = KeyManager::Instance();
    const auto directory = MakeTemporaryFolder("lichii-keyblob-index-test-");
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    };
    {
        std::ofstream file{directory / "prod.keys"};
        file << "keyblob_ff = " << std::string(0x90 * 2, 'a') << '\n';
        file << "encrypted_keyblob_ff = " << std::string(0xB0 * 2, 'b') << '\n';
        file << "keyblob_20 = " << std::string(0x90 * 2, 'c') << '\n';
    }

    const auto titlekek_source = keys.GetKey(S128KeyType::Source, u64(SourceKeyType::Titlekek));
    const auto previous = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, directory);
    keys.ReloadKeys();
    Tests::RestoreEdenPath(Common::FS::EdenPath::KeysDir, previous);

    CHECK(keys.GetKey(S128KeyType::Source, u64(SourceKeyType::Titlekek)) == titlekek_source);
}

} // namespace
