// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Its own executable: the log backend is made once per process, so each test case runs in a
// process of its own (see the add_test lines), and the second Lichii is a child process.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"

#if defined(__linux__) || defined(__APPLE__)
#define LOG_LOCK_CHILD 1
#include <spawn.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace {

fs::path TempPath(std::string_view what) {
    return fs::temp_directory_path() /
           (std::string{"lichii-log-lock-"} + std::string{what} + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

#ifdef LOG_LOCK_CHILD
// This test executable, to start it again as the second Lichii.
std::string ExecutablePath() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    path.resize(std::char_traits<char>::length(path.c_str()));
    return path;
#else
    return fs::read_symlink("/proc/self/exe").string();
#endif
}
#endif

std::string ReadText(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

// Starts the log in dir, writes marker and flushes it.
void StartLog(const fs::path& dir, const std::string& marker) {
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, dir);
    Common::Log::Initialize();
    LOG_INFO(Log, "{}", marker);
    Common::Log::Flush();
}

// The log already started in this process (both cases run in one process without a filter).
bool LogStarted() {
    return !Common::Log::FilePath().empty();
}

} // Anonymous namespace

TEST_CASE("The log folder is created with its missing parents", "[common][logging]") {
    if (LogStarted()) {
        SKIP("Needs a process of its own");
    }
    const auto root = TempPath("parents");
    SCOPE_EXIT {
        std::error_code ec;
        fs::remove_all(root, ec);
    };
    const auto dir = root / "missing" / "log";
    // SetEdenPath keeps the previous folder for a path that isn't a directory yet.
    fs::create_directories(dir);
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, dir);
    fs::remove_all(root);
    REQUIRE_FALSE(fs::exists(root));

    Common::Log::Initialize();
    LOG_INFO(Log, "parents marker");
    Common::Log::Flush();
    CHECK(Common::Log::FilePath() == dir / "eden_log.txt");
    CHECK(ReadText(dir / "eden_log.txt").find("parents marker") != std::string::npos);
}

// Run by the next test case in a child process: a second Lichii starting its log in the folder.
TEST_CASE("Log lock child", "[.log_lock_child]") {
    const char* dir = std::getenv("LICHII_LOG_LOCK_DIR");
    REQUIRE(dir != nullptr);
    StartLog(dir, "child marker");
}

TEST_CASE("A second Lichii leaves a log another one still writes", "[common][logging]") {
#ifndef LOG_LOCK_CHILD
    SKIP("Starts a child process with posix_spawn");
#else
    if (LogStarted()) {
        SKIP("Needs a process of its own");
    }
    const auto dir = TempPath("live");
    SCOPE_EXIT {
        std::error_code ec;
        fs::remove_all(dir, ec);
    };
    REQUIRE(fs::create_directories(dir));
    // The log of a session that has ended.
    {
        std::ofstream{dir / "eden_log.txt"} << "previous session";
    }
    StartLog(dir, "parent marker");
    const auto log = dir / "eden_log.txt";
    REQUIRE(Common::Log::FilePath() == log);
    REQUIRE(ReadText(dir / "eden_log.txt.old.txt") == "previous session");

    const std::string variable = "LICHII_LOG_LOCK_DIR=" + dir.string();
    std::vector<std::string> environment{variable};
    for (char** entry = environ; *entry != nullptr; ++entry) {
        if (std::string_view{*entry}.starts_with("LICHII_LOG_LOCK_DIR=")) {
            continue;
        }
        environment.emplace_back(*entry);
    }
    std::vector<char*> envp;
    for (auto& entry : environment) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    std::string exe = ExecutablePath();
    REQUIRE_FALSE(exe.empty());
    std::string filter = "[.log_lock_child]";
    char* argv[] = {exe.data(), filter.data(), nullptr};
    pid_t child{};
    REQUIRE(posix_spawn(&child, exe.c_str(), nullptr, nullptr, argv, envp.data()) == 0);
    int status{};
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);

    // The child wrote eden_log_2.txt; this process's log stayed where it is, untouched.
    CHECK(ReadText(dir / "eden_log.txt.old.txt") == "previous session");
    CHECK(ReadText(dir / "eden_log_2.txt").find("child marker") != std::string::npos);
    LOG_INFO(Log, "parent after child");
    Common::Log::Flush();
    const auto text = ReadText(log);
    CHECK(text.find("parent marker") != std::string::npos);
    CHECK(text.find("parent after child") != std::string::npos);
    CHECK(text.find("child marker") == std::string::npos);
#endif
}
