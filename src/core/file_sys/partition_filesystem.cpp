// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <span>
#include <utility>

#include "common/logging.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/loader/loader.h"

namespace FileSys {

bool PartitionFilesystem::Header::HasValidMagicValue() const {
    return magic == Common::MakeMagic('H', 'F', 'S', '0') ||
           magic == Common::MakeMagic('P', 'F', 'S', '0');
}

PartitionFilesystem::PartitionFilesystem(VirtualFile file) {
    // At least be as large as the header
    if (file->GetSize() < sizeof(Header)) {
        status = Loader::ResultStatus::ErrorBadPFSHeader;
        return;
    }

    // For cartridges, HFSs can get very large, so we need to calculate the size up to
    // the actual content itself instead of just blindly reading in the entire file.
    if (sizeof(Header) != file->ReadObject(&pfs_header)) {
        status = Loader::ResultStatus::ErrorBadPFSHeader;
        return;
    }

    if (!pfs_header.HasValidMagicValue()) {
        status = Loader::ResultStatus::ErrorBadPFSHeader;
        return;
    }

    is_hfs = pfs_header.magic == Common::MakeMagic('H', 'F', 'S', '0');

    const u64 entry_size = is_hfs ? sizeof(HFSEntry) : sizeof(PFSEntry);
    const u64 metadata_size =
        sizeof(Header) + (pfs_header.num_entries * entry_size) + pfs_header.strtab_size;
    if (metadata_size > file->GetSize()) {
        status = Loader::ResultStatus::ErrorIncorrectPFSFileSize;
        return;
    }

    // Actually read in now...
    const std::vector<u8> file_data = file->ReadBytes(metadata_size);
    if (file_data.size() != metadata_size) {
        status = Loader::ResultStatus::ErrorIncorrectPFSFileSize;
        return;
    }

    const std::size_t entries_offset = sizeof(Header);
    const std::size_t strtab_offset = entries_offset + (pfs_header.num_entries * entry_size);
    const std::span<const char> strtab{
        reinterpret_cast<const char*>(file_data.data()) + strtab_offset, pfs_header.strtab_size};
    content_offset = strtab_offset + pfs_header.strtab_size;
    const u64 file_size = file->GetSize();
    for (u32 i = 0; i < pfs_header.num_entries; i++) {
        FSEntry entry;

        memcpy(&entry, &file_data[entries_offset + (i * entry_size)], sizeof(FSEntry));
        if (entry.strtab_offset >= strtab.size()) {
            offsets.clear();
            sizes.clear();
            pfs_files.clear();
            status = Loader::ResultStatus::ErrorBadPFSHeader;
            return;
        }
        const auto name_bytes = strtab.subspan(entry.strtab_offset);
        std::string name(name_bytes.data(), strnlen(name_bytes.data(), name_bytes.size()));

        // An entry is opened as a file of the size it gives, and everything read from it later,
        // an NCA's checks included, takes that size as what the file stores. An entry that runs
        // past the end of the file, as in a truncated dump, is kept with the bytes the file
        // holds from its offset, so the size is a true one; whether those are enough is for
        // whatever reads the entry to judge.
        const u64 held = entry.offset < file_size - content_offset
                             ? file_size - content_offset - entry.offset
                             : 0;
        const u64 kept = std::min<u64>(entry.size, held);
        if (kept != entry.size) {
            LOG_WARNING(Loader, "{} entry of {} bytes at {} runs past the end of its file: kept {}",
                        is_hfs ? "HFS0" : "PFS0", entry.size, content_offset + entry.offset, kept);
        }

        offsets.insert_or_assign(name, content_offset + entry.offset);
        sizes.insert_or_assign(name, kept);

        pfs_files.emplace_back(std::make_shared<OffsetVfsFile>(
            file, kept, content_offset + entry.offset, std::move(name)));
    }

    status = Loader::ResultStatus::Success;
}

PartitionFilesystem::~PartitionFilesystem() = default;

Loader::ResultStatus PartitionFilesystem::GetStatus() const {
    return status;
}

std::map<std::string, u64> PartitionFilesystem::GetFileOffsets() const {
    return offsets;
}

std::map<std::string, u64> PartitionFilesystem::GetFileSizes() const {
    return sizes;
}

std::vector<VirtualFile> PartitionFilesystem::GetFiles() const {
    return pfs_files;
}

std::vector<VirtualDir> PartitionFilesystem::GetSubdirectories() const {
    return {};
}

std::string PartitionFilesystem::GetName() const {
    return is_hfs ? "HFS0" : "PFS0";
}

VirtualDir PartitionFilesystem::GetParentDirectory() const {
    // TODO(DarkLordZach): Add support for nested containers.
    return nullptr;
}

} // namespace FileSys
