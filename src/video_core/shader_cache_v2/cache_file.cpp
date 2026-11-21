// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

#include "common/fs/file.h"
#include "common/fs/file_lock.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/shader_cache_v2/cache_file.h"

namespace VideoCommon::ShaderCacheV2 {
namespace {

using Common::FS::FileAccessMode;
using Common::FS::FileShareFlag;
using Common::FS::FileType;
using Common::FS::IOFile;

/// How many bytes LogBuilder writes at once
constexpr size_t WriteChunk = 4 * 1024 * 1024;

u64 FileSize(const std::filesystem::path& file) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    return error ? 0 : static_cast<u64>(size);
}

/// Writes `bytes` to a temporary file, commits it to the disk and renames it over `file`
WriteResult ReplaceFile(const std::filesystem::path& file, std::span<const u8> bytes) {
    auto temporary = file;
    temporary += ".tmp";
    bool written{};
    {
        const IOFile target{temporary, FileAccessMode::Write, FileType::BinaryFile,
                            FileShareFlag::ShareReadWrite};
        // The rename can reach the disk before the data: commit the data first.
        written = target.IsOpen() && target.WriteSpan(bytes) == bytes.size() && target.Commit();
    }
    std::error_code error;
    if (written) {
        std::filesystem::rename(temporary, file, error);
        if (!error) {
            return WriteResult::Written;
        }
    }
    LOG_ERROR(Common_Filesystem, "Failed to write shader cache {}: {}",
              Common::FS::PathToUTF8String(file),
              error ? error.message() : "the new file can't be written");
    std::filesystem::remove(temporary, error);
    return WriteResult::Failed;
}

} // Anonymous namespace

u64 UnixNow() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
}

namespace {

LogFile EmptyLog(const ForEach& for_each) {
    LogFile log;
    log.parsed = ParseLog(log.bytes, for_each);
    return log;
}

// Nothing kept, and parsed as no log: a caller that missed read_failed still finds nothing to
// rewrite, and no records to drop.
LogFile FailedRead() {
    LogFile log = EmptyLog({});
    log.read_failed = true;
    return log;
}

} // Anonymous namespace

LogFile ReadLog(std::istream& stream, u64 size, const ForEach& for_each, ReadMode mode) {
    if (size == 0) {
        return EmptyLog(for_each);
    }
    LogFile log;
    log.bytes.resize(static_cast<size_t>(size));
    // In chunks: MinGW's libstdc++ hands one read to the CRT's _read, which aborts the process
    // (invalid parameter) on a count above INT_MAX, a cache past 2 GiB
    constexpr u64 Chunk = u64{256} << 20;
    u64 done = 0;
    while (done < size) {
        const u64 want = std::min(Chunk, size - done);
        stream.read(reinterpret_cast<char*>(log.bytes.data()) + done,
                    static_cast<std::streamsize>(want));
        done += static_cast<u64>(stream.gcount());
        if (static_cast<u64>(stream.gcount()) != want) {
            break;
        }
    }
    // A read error is not always reported as one: libc++ returns fewer bytes with no badbit.
    // Under the lock nothing shrinks the file, so fewer bytes are an error, and nothing read may
    // decide a rewrite or a removal. Without it, the file shrank since (a compaction replaced it).
    if (mode == ReadMode::Strict && (done < size || stream.bad())) {
        return FailedRead();
    }
    log.read_failed = stream.bad();
    log.bytes.resize(static_cast<size_t>(done));
    log.parsed = ParseLog(log.bytes, for_each);
    return log;
}

LogFile ReadLogFile(const std::filesystem::path& file, const ForEach& for_each, ReadMode mode) {
    const bool strict = mode == ReadMode::Strict;
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(file, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return EmptyLog(for_each);
    }
    // A directory or a device at the path could report any size; only a regular file is a log.
    if (error || status.type() != std::filesystem::file_type::regular) {
        if (strict) {
            return FailedRead();
        }
        LOG_WARNING(Common_Filesystem, "Shader cache {} is not a regular file; read as empty",
                    Common::FS::PathToUTF8String(file));
        return EmptyLog(for_each);
    }
    const u64 size = std::filesystem::file_size(file, error);
    std::ifstream stream(file, std::ios::binary);
    if (error || !stream.is_open()) {
        return strict ? FailedRead() : EmptyLog(for_each);
    }
    if (strict) {
        // Under the lock the stream must see the size file_size gave.
        stream.seekg(0, std::ios::end);
        const std::streamoff end = stream.tellg();
        stream.seekg(0);
        if (!stream || end < 0 || static_cast<u64>(end) != size) {
            return FailedRead();
        }
    }
    return ReadLog(stream, size, for_each, mode);
}

std::optional<std::vector<u8>> ReadRecordPayload(const std::filesystem::path& file,
                                                 RecordKind kind, u64 key, u64 offset,
                                                 u64 payload_size) {
    std::vector<u8> bytes(RecordHeaderSize + payload_size + RecordHashSize);
    std::ifstream stream(file, std::ios::binary);
    if (!stream.seekg(static_cast<std::streamoff>(offset)) ||
        !stream.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    const std::optional<LogRecord> record = ParseRecord(bytes, offset);
    if (!record || record->kind != kind || record->key != key ||
        record->payload.size() != payload_size) {
        return std::nullopt;
    }
    bytes.erase(bytes.begin(), bytes.begin() + RecordHeaderSize);
    bytes.resize(payload_size);
    return bytes;
}

WriteResult AppendToLog(const std::filesystem::path& file, std::span<const u8> records,
                        u64* records_offset) {
    // Without locks on this file system (the lock is neither held nor busy) it goes on as the
    // other caches do there.
    const Common::FS::FileLock lock{PipelineCacheLockPath(file)};
    if (lock.Busy()) {
        return WriteResult::Busy;
    }
    const u64 size = FileSize(file);
    std::vector<u8> bytes;
    if (size < FileHeaderSize) {
        // New, or cut off within its header before any record was written
        const auto header = EncodeFileHeader(UnixNow());
        bytes.assign(header.begin(), header.end());
        bytes.insert(bytes.end(), records.begin(), records.end());
        const IOFile target{file, FileAccessMode::Write, FileType::BinaryFile,
                            FileShareFlag::ShareReadWrite};
        if (target.IsOpen() && target.WriteSpan(std::span<const u8>{bytes}) == bytes.size() &&
            target.Flush()) {
            if (records_offset) {
                *records_offset = FileHeaderSize;
            }
            return WriteResult::Written;
        }
        LOG_ERROR(Common_Filesystem, "Failed to create shader cache {}",
                  Common::FS::PathToUTF8String(file));
        return WriteResult::Failed;
    }
    {
        std::array<u8, FileHeaderSize> header{};
        const IOFile source{file, FileAccessMode::Read, FileType::BinaryFile,
                            FileShareFlag::ShareReadWrite};
        if (!source.IsOpen() || source.ReadSpan(std::span<u8>{header}) != header.size()) {
            return WriteResult::Failed;
        }
        switch (ParseLog(header).header) {
        case HeaderStatus::OtherVersion:
            return WriteResult::OtherVersion;
        case HeaderStatus::Damaged: {
            // The records check themselves and stay
            const IOFile target{file, FileAccessMode::ReadWrite,
                                FileType::BinaryFile, FileShareFlag::ShareReadWrite};
            const auto fresh = EncodeFileHeader(UnixNow());
            if (!target.IsOpen() || target.WriteSpan(std::span<const u8>{fresh}) != fresh.size() ||
                !target.Flush()) {
                return WriteResult::Failed;
            }
            break;
        }
        case HeaderStatus::Valid:
        case HeaderStatus::Empty:
            break;
        }
    }
    // A record cut off by a crash leaves the end off a record boundary.
    const u64 padding = (RecordAlignment - size % RecordAlignment) % RecordAlignment;
    bytes.resize(padding);
    bytes.insert(bytes.end(), records.begin(), records.end());
    bool written{};
    {
        const IOFile target{file, FileAccessMode::Append, FileType::BinaryFile,
                            FileShareFlag::ShareReadWrite};
        written = target.IsOpen() &&
                  target.WriteSpan(std::span<const u8>{bytes}) == bytes.size() && target.Flush();
    }
    if (written) {
        if (records_offset) {
            *records_offset = size + padding;
        }
        return WriteResult::Written;
    }
    // Cut off what was written (a full disk), so that the next records follow complete ones.
    std::error_code error;
    std::filesystem::resize_file(file, size, error);
    LOG_ERROR(Common_Filesystem, "Failed to append to shader cache {}",
              Common::FS::PathToUTF8String(file));
    return WriteResult::Failed;
}

WriteResult CompactLog(const std::filesystem::path& file, const CompactLimit& limit) {
    const Common::FS::FileLock lock{PipelineCacheLockPath(file)};
    if (!lock.Held()) {
        return WriteResult::Busy;
    }
    return CompactRead(file, ReadLogFile(file, {}, ReadMode::Strict), limit);
}

WriteResult CompactRead(const std::filesystem::path& file, const LogFile& log,
                        const CompactLimit& limit) {
    // A log that couldn't be read whole would be rewritten without the records after the error.
    if (log.read_failed) {
        LOG_ERROR(Common_Filesystem, "Couldn't read shader cache {} whole; not compacted",
                  Common::FS::PathToUTF8String(file));
        return WriteResult::Failed;
    }
    if (log.parsed.header == HeaderStatus::Empty) {
        return WriteResult::Written;
    }
    if (log.parsed.header == HeaderStatus::OtherVersion) {
        return WriteResult::OtherVersion;
    }
    const auto header = EncodeFileHeader(log.parsed.created_unix != 0 ? log.parsed.created_unix
                                                                       : UnixNow());
    std::vector<u8> bytes(header.begin(), header.end());
    u64 live_bytes{};
    for (const LogRecord& record : log.parsed.live) {
        live_bytes += RecordSize(record.payload.size());
    }
    std::vector<const LogRecord*> kept;
    kept.reserve(log.parsed.live.size());
    for (const LogRecord& record : log.parsed.live) {
        kept.push_back(&record);
    }
    if (live_bytes > limit.max_bytes) {
        const auto rank = [&](const LogRecord* record) {
            return limit.rank ? limit.rank(*record) : 0;
        };
        std::ranges::stable_sort(kept, [&](const LogRecord* a, const LogRecord* b) {
            const u32 rank_a = rank(a);
            const u32 rank_b = rank(b);
            return rank_a != rank_b ? rank_a < rank_b : a->offset > b->offset;
        });
        u64 total{};
        size_t count{};
        while (count < kept.size() &&
               total + RecordSize(kept[count]->payload.size()) <= limit.target_bytes) {
            total += RecordSize(kept[count]->payload.size());
            ++count;
        }
        kept.resize(count);
        std::ranges::sort(kept, {}, &LogRecord::offset);
    }
    for (const LogRecord* record : kept) {
        EncodeRecord(bytes, record->kind, record->key, record->payload, record->flags);
    }
    return ReplaceFile(file, bytes);
}

// Out of line so that a caller does not need the complete type of the lock it holds
LogBuilder::LogBuilder() = default;

LogBuilder::~LogBuilder() {
    Discard();
}

WriteResult LogBuilder::Start(const std::filesystem::path& log) {
    file = log;
    lock = std::make_unique<Common::FS::FileLock>(PipelineCacheLockPath(file));
    const auto release = [this](WriteResult result) {
        lock.reset();
        file.clear();
        return result;
    };
    if (lock->Busy()) {
        return release(WriteResult::Busy);
    }
    std::error_code error;
    if (std::filesystem::exists(file, error) || error) {
        return release(WriteResult::Exists);
    }
    temporary = file;
    temporary += ".migrating";
    target = Common::FS::IOFile{temporary, FileAccessMode::Write, FileType::BinaryFile,
                                FileShareFlag::ShareReadWrite};
    if (!target.IsOpen()) {
        temporary.clear();
        return release(WriteResult::Failed);
    }
    const auto header = EncodeFileHeader(UnixNow());
    buffer.assign(header.begin(), header.end());
    buffer.reserve(WriteChunk);
    return WriteResult::Written;
}

bool LogBuilder::Append(RecordKind kind, u64 key, std::span<const u8> payload) {
    if (!written) {
        return false;
    }
    EncodeRecord(buffer, kind, key, payload);
    if (buffer.size() < WriteChunk) {
        return true;
    }
    if (target.WriteSpan(std::span<const u8>{buffer}) != buffer.size()) {
        written = false;
        return false;
    }
    buffer.clear();
    return true;
}

WriteResult LogBuilder::Commit() {
    if (!written || !target.IsOpen()) {
        Discard();
        return WriteResult::Failed;
    }
    if (!buffer.empty()) {
        const bool flushed = target.WriteSpan(std::span<const u8>{buffer}) == buffer.size();
        buffer.clear();
        if (!flushed) {
            Discard();
            return WriteResult::Failed;
        }
    }
    // The rename can reach the disk before the data: commit the data first.
    if (!target.Commit()) {
        Discard();
        return WriteResult::Failed;
    }
    target.Close();
    std::error_code error;
    if (std::filesystem::exists(file, error) || error) {
        Discard();
        return WriteResult::Exists;
    }
    std::filesystem::rename(temporary, file, error);
    if (error) {
        LOG_ERROR(Common_Filesystem, "Failed to write shader cache {}: {}",
                  Common::FS::PathToUTF8String(file), error.message());
        Discard();
        return WriteResult::Failed;
    }
    temporary.clear();
    lock.reset();
    return WriteResult::Written;
}

void LogBuilder::Discard() {
    if (temporary.empty()) {
        return;
    }
    target.Close();
    std::error_code error;
    std::filesystem::remove(temporary, error);
    temporary.clear();
    buffer.clear();
    lock.reset();
}

} // namespace VideoCommon::ShaderCacheV2
