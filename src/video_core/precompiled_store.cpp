// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>
#include <system_error>

#include "common/logging.h"
#include "video_core/precompiled_store.h"

namespace VideoCommon {
namespace {

constexpr u32 IndexVersion = 1;
constexpr size_t BlobEntrySize = 24;
constexpr size_t RecordEntrySize = 16 + 3 * PrecompiledStore::MaxStages * sizeof(u32);
constexpr size_t MarkerEntrySize = 8;
constexpr u8 RecordCompute = 1;
constexpr u8 RecordFailed = 2;

template <typename T>
void Put(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
T Get(const std::vector<char>& bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

/// The bytes of `path` (none when it is missing), or nothing when it is there but cannot be read:
/// a store that can't be read is kept, not started again. A read error does not always say so:
/// libc++ reads a directory, or a file whose read fails, as having no bytes, with no exception and
/// no badbit. So anything but a regular file is not read, and a read that returns fewer bytes than
/// the file's size is an error. The store's lock is held while it is read (PreparePrecompiled), so
/// no other emulator changes the file between its size and the read.
std::optional<std::vector<char>> ReadAll(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (!error) {
            return std::vector<char>{};
        }
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return std::nullopt;
    }
    const u64 size = std::filesystem::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::vector<char> bytes(static_cast<size_t>(size));
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (file.bad() || static_cast<u64>(file.gcount()) != size) {
        return std::nullopt;
    }
    return bytes;
}

} // Anonymous namespace

PrecompiledStore::~PrecompiledStore() {
    Close();
}

bool PrecompiledStore::Open(const std::filesystem::path& data_path,
                            const std::filesystem::path& index_path, std::string_view stamp,
                            u32 record_count, Loaded& loaded) {
    std::scoped_lock lock{mutex};
    loaded = {};
    loaded.states.assign(record_count, RecordState::Pending);
    failed = false;
    switch (Resume(data_path, index_path, stamp, record_count, loaded)) {
    case ResumeResult::Resumed:
        return true;
    case ResumeResult::Failed:
        LOG_ERROR(Render, "Precompiled pipelines: cannot read or write {} or {}; kept as they are",
                  data_path.string(), index_path.string());
        loaded = {};
        return false;
    case ResumeResult::Mismatch:
        break;
    }
    loaded = {};
    loaded.states.assign(record_count, RecordState::Pending);
    return Start(data_path, index_path, stamp, record_count);
}

bool PrecompiledStore::Start(const std::filesystem::path& data_path,
                             const std::filesystem::path& index_path, std::string_view stamp,
                             u32 record_count) {
    data.close();
    index.close();
    blobs.clear();
    indexed_blobs = 0;
    blob_ids[0].clear();
    blob_ids[1].clear();
    published.clear();
    data.clear();
    index.clear();
    data.open(data_path, std::ios::binary | std::ios::trunc);
    index.open(index_path, std::ios::binary | std::ios::trunc);
    if (!data || !index) {
        LOG_ERROR(Render, "Precompiled pipelines: cannot write {} or {}", data_path.string(),
                  index_path.string());
        data.close();
        index.close();
        return false;
    }
    data.write(DataMagic.data(), DataMagic.size());
    Put(data, static_cast<u32>(stamp.size()));
    data.write(stamp.data(), static_cast<std::streamsize>(stamp.size()));
    data_offset = DataMagic.size() + sizeof(u32) + stamp.size();
    index.write(IndexMagic.data(), IndexMagic.size());
    Put(index, IndexVersion);
    Put(index, record_count);
    Put(index, static_cast<u32>(stamp.size()));
    index.write(stamp.data(), static_cast<std::streamsize>(stamp.size()));
    data.flush();
    index.flush();
    return static_cast<bool>(data) && static_cast<bool>(index);
}

PrecompiledStore::ResumeResult PrecompiledStore::Resume(const std::filesystem::path& data_path,
                                                        const std::filesystem::path& index_path,
                                                        std::string_view stamp, u32 record_count,
                                                        Loaded& loaded) {
    std::error_code error;
    const u64 data_size = std::filesystem::file_size(data_path, error);
    if (error) {
        // Only a missing file starts again: one that is there but has no size is kept.
        std::error_code exists_error;
        const bool missing = !std::filesystem::exists(data_path, exists_error) && !exists_error;
        return missing ? ResumeResult::Mismatch : ResumeResult::Failed;
    }
    // The data file's header: its stamp must be this one too. A read error is not always reported
    // as one (see ReadAll): a read that returns fewer bytes than the file holds from there is an
    // error, and the store is kept; only a file shorter than the header is one to start again.
    {
        if (!std::filesystem::is_regular_file(data_path, error) || error) {
            return ResumeResult::Failed;
        }
        std::ifstream file(data_path, std::ios::binary);
        if (!file) {
            return ResumeResult::Failed; // it is there (its size was read), but can't be read
        }
        const auto read_failed = [&file, data_size](std::streamsize wanted, u64 at) {
            const u64 left = data_size > at ? data_size - at : 0;
            return file.bad() ||
                   static_cast<u64>(file.gcount()) < (std::min)(static_cast<u64>(wanted), left);
        };
        std::array<char, 8> magic{};
        u32 stamp_size{};
        file.read(magic.data(), magic.size());
        if (read_failed(static_cast<std::streamsize>(magic.size()), 0)) {
            return ResumeResult::Failed;
        }
        file.read(reinterpret_cast<char*>(&stamp_size), sizeof(stamp_size));
        if (read_failed(sizeof(stamp_size), magic.size())) {
            return ResumeResult::Failed;
        }
        if (!file || std::string_view{magic.data(), magic.size()} != DataMagic ||
            stamp_size != stamp.size()) {
            return ResumeResult::Mismatch;
        }
        std::string file_stamp(stamp_size, '\0');
        file.read(file_stamp.data(), stamp_size);
        if (read_failed(stamp_size, magic.size() + sizeof(stamp_size))) {
            return ResumeResult::Failed;
        }
        if (!file || file_stamp != stamp) {
            return ResumeResult::Mismatch;
        }
    }
    const u64 data_begin = DataMagic.size() + sizeof(u32) + stamp.size();
    const std::optional<std::vector<char>> read = ReadAll(index_path);
    if (!read) {
        return ResumeResult::Failed;
    }
    const std::vector<char>& bytes = *read;
    const size_t header_size = IndexMagic.size() + 3 * sizeof(u32) + stamp.size();
    if (bytes.size() < header_size ||
        std::string_view{bytes.data(), IndexMagic.size()} != IndexMagic ||
        Get<u32>(bytes, 8) != IndexVersion || Get<u32>(bytes, 12) != record_count ||
        Get<u32>(bytes, 16) != stamp.size() ||
        std::string_view{bytes.data() + 20, stamp.size()} != stamp) {
        return ResumeResult::Mismatch;
    }

    std::vector<Blob> read_blobs;
    std::vector<Record> records;
    std::vector<u32> starts(record_count, 0);
    size_t offset = header_size;
    // Entries are read up to the first one that is cut off or names what is not there: a run
    // writes a record's entry only after what it names, so later entries are not trusted.
    while (offset < bytes.size()) {
        const auto type = static_cast<EntryType>(static_cast<u8>(bytes[offset]));
        const size_t left = bytes.size() - offset;
        if (type == EntryType::Blob) {
            if (left < BlobEntrySize) {
                break;
            }
            Blob blob;
            blob.kind = static_cast<BlobKind>(static_cast<u8>(bytes[offset + 1]));
            blob.size = Get<u32>(bytes, offset + 4);
            blob.offset = Get<u64>(bytes, offset + 8);
            blob.hash = Get<u64>(bytes, offset + 16);
            if (static_cast<u8>(blob.kind) > 1 || blob.offset < data_begin ||
                blob.offset + blob.size > data_size) {
                break;
            }
            read_blobs.push_back(blob);
            offset += BlobEntrySize;
        } else if (type == EntryType::Record) {
            if (left < RecordEntrySize) {
                break;
            }
            Record record;
            record.env_count = static_cast<u8>(bytes[offset + 1]);
            const u8 flags = static_cast<u8>(bytes[offset + 2]);
            record.compute = (flags & RecordCompute) != 0;
            record.failed = (flags & RecordFailed) != 0;
            record.index = Get<u32>(bytes, offset + 4);
            record.key_hash = Get<u64>(bytes, offset + 8);
            std::memcpy(record.envs.data(), bytes.data() + offset + 16, sizeof(record.envs));
            std::memcpy(record.infos.data(), bytes.data() + offset + 36, sizeof(record.infos));
            std::memcpy(record.spirv.data(), bytes.data() + offset + 56, sizeof(record.spirv));
            const auto bad_blob = [&](u32 id) {
                return id != NoBlob && id >= read_blobs.size();
            };
            if (record.index >= record_count || record.env_count > MaxStages ||
                std::ranges::any_of(record.infos, bad_blob) ||
                std::ranges::any_of(record.spirv, bad_blob)) {
                break;
            }
            if (loaded.states[record.index] == RecordState::Pending) {
                loaded.states[record.index] = RecordState::Prepared;
                records.push_back(record);
            }
            offset += RecordEntrySize;
        } else if (type == EntryType::Started || type == EntryType::Abandoned) {
            if (left < MarkerEntrySize) {
                break;
            }
            const u32 record_index = Get<u32>(bytes, offset + 4);
            if (record_index >= record_count) {
                break;
            }
            if (type == EntryType::Started) {
                ++starts[record_index];
            } else if (starts[record_index] != 0) {
                --starts[record_index];
            }
            offset += MarkerEntrySize;
        } else {
            break;
        }
    }

    data.close();
    index.close();
    data.clear();
    index.clear();
    // What follows the last entry read goes, so new entries follow it.
    std::filesystem::resize_file(index_path, offset, error);
    if (error) {
        return ResumeResult::Failed;
    }
    data.open(data_path, std::ios::binary | std::ios::app);
    index.open(index_path, std::ios::binary | std::ios::app);
    if (!data || !index) {
        data.close();
        index.close();
        return ResumeResult::Failed;
    }
    data_offset = data_size;
    blobs = read_blobs;
    indexed_blobs = blobs.size();
    blob_ids[0].clear();
    blob_ids[1].clear();
    for (u32 id = 0; id < blobs.size(); ++id) {
        blob_ids[static_cast<u8>(blobs[id].kind)].try_emplace(blobs[id].hash, id);
    }
    published.clear();

    for (u32 record_index = 0; record_index < record_count; ++record_index) {
        if (loaded.states[record_index] != RecordState::Pending || starts[record_index] == 0) {
            continue;
        }
        ++loaded.interrupted;
        if (starts[record_index] >= PoisonStarts) {
            // Noted as failed, so the next open finds it done.
            Record record;
            record.index = record_index;
            record.failed = true;
            WriteRecordLocked(record);
            loaded.states[record_index] = RecordState::Poisoned;
            ++loaded.poisoned;
        }
    }
    index.flush();
    loaded.blobs = std::move(read_blobs);
    loaded.records = std::move(records);
    loaded.resumed = true;
    if (!index) {
        FailLocked();
        return ResumeResult::Failed;
    }
    return ResumeResult::Resumed;
}

u32 PrecompiledStore::AddBlob(BlobKind kind, u64 hash, std::span<const u8> bytes) {
    std::scoped_lock lock{mutex};
    auto& ids = blob_ids[static_cast<u8>(kind)];
    const auto [it, added] = ids.try_emplace(hash, static_cast<u32>(blobs.size()));
    if (added) {
        data.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        blobs.push_back({.offset = data_offset,
                         .size = static_cast<u32>(bytes.size()),
                         .kind = kind,
                         .hash = hash});
        data_offset += bytes.size();
    }
    return it->second;
}

bool PrecompiledStore::WriteMarker(EntryType type, u32 record_index) {
    std::scoped_lock lock{mutex};
    if (!index.is_open()) {
        return false;
    }
    Put(index, static_cast<u8>(type));
    Put(index, std::array<u8, 3>{});
    Put(index, record_index);
    // On disk before the build starts: a driver crash ends the process without unwinding.
    index.flush();
    if (!index) {
        // Failed() stops the preparation: a build without its marker could crash unnoted.
        FailLocked();
        return false;
    }
    return true;
}

bool PrecompiledStore::MarkStarted(u32 record_index) {
    return WriteMarker(EntryType::Started, record_index);
}

void PrecompiledStore::MarkAbandoned(u32 record_index) {
    WriteMarker(EntryType::Abandoned, record_index);
}

void PrecompiledStore::WriteRecordLocked(const Record& record) {
    Put(index, static_cast<u8>(EntryType::Record));
    Put(index, record.env_count);
    Put(index, static_cast<u8>((record.compute ? RecordCompute : 0) |
                               (record.failed ? RecordFailed : 0)));
    Put(index, u8{0});
    Put(index, record.index);
    Put(index, record.key_hash);
    Put(index, record.envs);
    Put(index, record.infos);
    Put(index, record.spirv);
}

void PrecompiledStore::FailLocked() {
    // What was noted before stays published: its entries and blobs are on disk.
    failed = true;
    data.close();
    index.close();
}

bool PrecompiledStore::Failed() const {
    std::scoped_lock lock{mutex};
    return failed;
}

bool PrecompiledStore::AddRecord(const Record& record) {
    std::scoped_lock lock{mutex};
    if (!index.is_open()) {
        return false;
    }
    // The blobs reach the file before the entries that name them; a record whose blobs did not
    // is not noted or published.
    data.flush();
    if (!data) {
        LOG_ERROR(Render, "Precompiled pipelines: writing the kept data failed; stopping");
        FailLocked();
        return false;
    }
    for (; indexed_blobs < blobs.size(); ++indexed_blobs) {
        const Blob& blob = blobs[indexed_blobs];
        Put(index, static_cast<u8>(EntryType::Blob));
        Put(index, static_cast<u8>(blob.kind));
        Put(index, u16{0});
        Put(index, blob.size);
        Put(index, blob.offset);
        Put(index, blob.hash);
    }
    WriteRecordLocked(record);
    index.flush();
    if (!index) {
        LOG_ERROR(Render, "Precompiled pipelines: writing the kept index failed; stopping");
        FailLocked();
        return false;
    }
    published.push_back(record);
    return true;
}

void PrecompiledStore::TakePublished(size_t known_blobs, std::vector<Blob>& new_blobs,
                                     std::vector<Record>& new_records) {
    std::scoped_lock lock{mutex};
    new_blobs.clear();
    if (known_blobs < indexed_blobs) {
        new_blobs.assign(blobs.begin() + static_cast<std::ptrdiff_t>(known_blobs),
                         blobs.begin() + static_cast<std::ptrdiff_t>(indexed_blobs));
    }
    new_records = std::move(published);
    published.clear();
}

bool PrecompiledStore::HasPublished() const {
    std::scoped_lock lock{mutex};
    return !published.empty();
}

void PrecompiledStore::Close() {
    std::scoped_lock lock{mutex};
    if (data.is_open()) {
        data.flush();
        data.close();
    }
    if (index.is_open()) {
        index.flush();
        index.close();
    }
}

} // namespace VideoCommon
