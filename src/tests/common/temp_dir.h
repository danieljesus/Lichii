// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace Tests {

/// A new folder under the temporary folder that no other tests process shares. A fixed name,
/// removed and made again, let two tests processes running at once (two build folders, a CI
/// matrix) delete each other's files; create_directory fails on a folder that exists, so a name
/// already taken is skipped instead.
inline std::filesystem::path MakeTempDir(std::string_view prefix) {
    std::random_device random;
    std::filesystem::path directory;
    do {
        directory = std::filesystem::temp_directory_path() /
                    (std::string{prefix} + '-' + std::to_string(random()));
    } while (!std::filesystem::create_directory(directory));
    return directory;
}

/// A MakeTempDir folder removed with everything in it when this goes out of scope.
struct TempDir {
    explicit TempDir(std::string_view prefix) : path{MakeTempDir(prefix)} {}
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path path;
};

} // namespace Tests
