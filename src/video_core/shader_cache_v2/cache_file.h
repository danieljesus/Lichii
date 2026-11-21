// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <memory>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/fs/file.h"
#include "video_core/shader_cache_v2/cache_format.h"

namespace Common::FS {
class FileLock;
}

namespace VideoCommon::ShaderCacheV2 {

/// A log read into memory; `parsed` points into `bytes`.
struct LogFile {
    std::vector<u8> bytes;
    ParsedLog parsed;
    /// A strict read returned fewer bytes than the log's size: a read error. Nothing read may then
    /// decide a rewrite or a removal.
    bool read_failed{};

    LogFile() = default;
    LogFile(LogFile&&) = default;
    LogFile& operator=(LogFile&&) = default;
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;
};

/// How a log is read.
enum class ReadMode {
    /// Without the log's lock: another emulator may compact it meanwhile, so fewer bytes than its
    /// size are a file that shrank, and what was read is parsed.
    Tolerant,
    /// With the log's lock held: nothing else changes it, so fewer bytes than its size, or a file
    /// that is not a regular one, are a read error (LogFile::read_failed).
    Strict,
};

/// Reads a log of `size` bytes from `stream`. See ParseLog for `for_each`.
[[nodiscard]] LogFile ReadLog(std::istream& stream, u64 size, const ForEach& for_each = {},
                              ReadMode mode = ReadMode::Tolerant);

/// Reads the whole log, by default without its lock: records are only appended under it, and the
/// reader ignores one cut off by an append still in progress. A missing file reads as empty. See
/// ParseLog for `for_each`.
[[nodiscard]] LogFile ReadLogFile(const std::filesystem::path& file, const ForEach& for_each = {},
                                  ReadMode mode = ReadMode::Tolerant);

/// The payload of the record of `kind` and `key` that a read of the log found at `offset`, read
/// again from the file the same way. Empty when the file no longer holds it there intact (a
/// compaction rewrote it).
[[nodiscard]] std::optional<std::vector<u8>> ReadRecordPayload(const std::filesystem::path& file,
                                                               RecordKind kind, u64 key,
                                                               u64 offset, u64 payload_size);

enum class WriteResult {
    Written,
    Busy,         ///< Another process holds the log's lock
    Failed,       ///< A write failed; the file is as it was
    OtherVersion, ///< The file is another format version's, and stays as it is
    Exists,       ///< LogBuilder::Start found the log there already
};

/// Appends encoded records (EncodeRecord) holding the log's lock, which is tried once. The file
/// and its header are created when missing, a damaged header is written again, and a record cut
/// off by a crash is padded to the next record boundary first. Once written, `records_offset`
/// (when given) is where the first of them begins in the file.
[[nodiscard]] WriteResult AppendToLog(const std::filesystem::path& file,
                                      std::span<const u8> records, u64* records_offset = nullptr);

/// Bounds what CompactLog keeps. While the live records take more than `max_bytes` (as written
/// again), only `target_bytes` of them are kept: those of the lowest rank first, the newest first
/// within a rank. The kept records stay in the order of the file.
struct CompactLimit {
    u64 max_bytes{~u64{0}};
    u64 target_bytes{~u64{0}};
    std::function<u32(const LogRecord&)> rank;
};

/// Rewrites the log with only its live records: to a temporary file that is committed to the disk
/// and renamed over it. Holding the log's lock, tried once, so that it never runs while another
/// process reads it under the lock or writes it.
[[nodiscard]] WriteResult CompactLog(const std::filesystem::path& file,
                                     const CompactLimit& limit = {});

/// CompactLog's rewrite of `file` from `log`, read whole with the log's lock held, which the
/// caller holds. A log whose read failed is not rewritten (WriteResult::Failed).
[[nodiscard]] WriteResult CompactRead(const std::filesystem::path& file, const LogFile& log,
                                      const CompactLimit& limit);

/// Writes a log that a caller produces record by record, holding the log's lock and a temporary
/// file from Start until Commit. A log made from a large source therefore costs one record of
/// memory, not the whole source written out twice.
class LogBuilder {
public:
    LogBuilder();
    ~LogBuilder();

    LogBuilder(const LogBuilder&) = delete;
    LogBuilder& operator=(const LogBuilder&) = delete;

    /// Opens the temporary file beside `file`; `Written` once it is open. `Exists` when the log is
    /// there already, `Busy` when another process holds its lock, `Failed` when the temporary file
    /// can't be written.
    [[nodiscard]] WriteResult Start(const std::filesystem::path& file);

    /// Encodes and buffers a record, writing it once a chunk has piled up. False once a write has
    /// failed, and on every call after that.
    bool Append(RecordKind kind, u64 key, std::span<const u8> payload);

    /// Writes what is left, commits it to the disk and renames it over the log, unless that exists
    /// by then. The builder is spent afterwards.
    [[nodiscard]] WriteResult Commit();

    /// Drops the temporary file, leaving the log as it was. Called by the destructor too.
    void Discard();

private:
    std::filesystem::path file;
    std::filesystem::path temporary;
    std::unique_ptr<Common::FS::FileLock> lock;
    Common::FS::IOFile target;
    std::vector<u8> buffer;
    bool written{true}; ///< False once a write failed
};

[[nodiscard]] u64 UnixNow();

} // namespace VideoCommon::ShaderCacheV2
