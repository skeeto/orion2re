#include "pch.h"

#include "vfs_detail.h"

#include <algorithm>

namespace vfs_detail {
    const s_vfs_backend_ops g_vfs_memory_file_ops = {
        Memory_File_Read_,
        Memory_File_Size_,
        Memory_File_Seek_,
        Memory_File_Tell_,
        Memory_File_Close_,
    };

    static bool Compare_Memory_Entries_(const s_vfs_memory_entry& lhs, const s_vfs_memory_entry& rhs) {
        return strcmp(lhs.asset_key, rhs.asset_key) < 0;
    }

    // Locates a stored entry's data through its local file header, which may
    // carry different extra-field bytes than the central directory.
    static const uint8_t* Stored_Entry_Data_(const uint8_t* archive, size_t archive_size, const mz_zip_archive_file_stat& file_stat) {
        constexpr size_t LOCAL_HEADER_SIZE = 30;
        constexpr uint32_t LOCAL_HEADER_SIGNATURE = 0x04034B50;
        uint32_t signature;
        uint16_t name_length;
        uint16_t extra_length;

        if (file_stat.m_local_header_ofs > archive_size || archive_size - file_stat.m_local_header_ofs < LOCAL_HEADER_SIZE) {
            return nullptr;
        }

        const uint8_t* header = archive + file_stat.m_local_header_ofs;
        memcpy(&signature, header, sizeof(signature));
        memcpy(&name_length, header + 26, sizeof(name_length));
        memcpy(&extra_length, header + 28, sizeof(extra_length));
        if (signature != LOCAL_HEADER_SIGNATURE) {
            return nullptr;
        }

        const uint64_t data_offset = file_stat.m_local_header_ofs + LOCAL_HEADER_SIZE + name_length + extra_length;
        if (data_offset > archive_size || archive_size - data_offset < file_stat.m_comp_size) {
            return nullptr;
        }

        return archive + data_offset;
    }

    static bool Read_Embedded_Entries_(mz_zip_archive* archive, const uint8_t* data, size_t size, std::vector<s_vfs_memory_entry>* entries) {
        const mz_uint total_files = mz_zip_reader_get_num_files(archive);
        for (mz_uint file_index = 0; file_index < total_files; ++file_index) {
            mz_zip_archive_file_stat file_stat;
            char normalized_path[VFS_MAX_PATH];
            s_vfs_memory_entry entry{};

            if (!mz_zip_reader_file_stat(archive, file_index, &file_stat)) {
                return false;
            }

            if (file_stat.m_is_directory) {
                continue;
            }

            if (file_stat.m_is_encrypted || file_stat.m_method != 0 || file_stat.m_comp_size != file_stat.m_uncomp_size || file_stat.m_uncomp_size > INT32_MAX) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "vfs: embedded entry '%s' is not stored uncompressed", file_stat.m_filename);
                return false;
            }

            if (!Normalize_Asset_Path_(file_stat.m_filename, normalized_path, sizeof(normalized_path)) ||
                !Build_Asset_Key_(normalized_path, entry.asset_key, sizeof(entry.asset_key))) {
                return false;
            }

            entry.data = Stored_Entry_Data_(data, size, file_stat);
            entry.size = (int32_t)file_stat.m_uncomp_size;
            if (entry.data == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "vfs: embedded entry '%s' has a bad local header", file_stat.m_filename);
                return false;
            }

            entries->push_back(entry);
        }

        std::sort(entries->begin(), entries->end(), Compare_Memory_Entries_);
        for (size_t i = 1; i < entries->size(); ++i) {
            if (strcmp((*entries)[i - 1].asset_key, (*entries)[i].asset_key) == 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "vfs: embedded archive has duplicate entry '%s'", (*entries)[i].asset_key);
                return false;
            }
        }

        return true;
    }

    bool Find_Embedded_Entry_(const char* asset_path, s_vfs_memory_entry* memory_entry) {
        s_vfs_memory_entry lookup_entry{};
        const std::vector<s_vfs_memory_entry>& entries = g_vfs_state.embedded_entries;

        if (entries.empty() || !Build_Asset_Key_(asset_path, lookup_entry.asset_key, sizeof(lookup_entry.asset_key))) {
            return false;
        }

        auto it = std::lower_bound(entries.begin(), entries.end(), lookup_entry, Compare_Memory_Entries_);
        if (it == entries.end() || strcmp(it->asset_key, lookup_entry.asset_key) != 0) {
            return false;
        }

        if (memory_entry != nullptr) {
            *memory_entry = *it;
        }

        return true;
    }

    s_vfs_file* Open_Memory_File_(const s_vfs_memory_entry& memory_entry) {
        s_vfs_file* vfs_file = new s_vfs_file{};
        vfs_file->backend_kind = VFS_BACKEND_MEMORY;
        vfs_file->ops = &g_vfs_memory_file_ops;
        vfs_file->backend_state = const_cast<uint8_t*>(memory_entry.data);
        vfs_file->data_size = memory_entry.size;
        vfs_file->position = 0;
        return vfs_file;
    }

    size_t Memory_File_Read_(void* ptr, size_t size, size_t count, s_vfs_file* file) {
        size_t max_count;

        if (ptr == nullptr || file == nullptr || size == 0 || count == 0 || file->position >= file->data_size) {
            return 0;
        }

        max_count = (size_t)(file->data_size - file->position) / size;
        if (max_count > count) {
            max_count = count;
        }

        memcpy(ptr, (const uint8_t*)file->backend_state + file->position, max_count * size);
        file->position += (int32_t)(max_count * size);
        return max_count;
    }

    int32_t Memory_File_Size_(s_vfs_file* file) {
        if (file == nullptr) {
            return -1;
        }

        return file->data_size;
    }

    int32_t Memory_File_Seek_(s_vfs_file* file, int32_t offset, int32_t origin) {
        int64_t new_position;

        if (file == nullptr) {
            return -1;
        }

        switch (origin) {
            case SEEK_SET:
                new_position = offset;
                break;
            case SEEK_CUR:
                new_position = (int64_t)file->position + offset;
                break;
            case SEEK_END:
                new_position = (int64_t)file->data_size + offset;
                break;
            default:
                return -1;
        }

        if (new_position < 0 || new_position > file->data_size) {
            return -1;
        }

        file->position = (int32_t)new_position;
        return 0;
    }

    int32_t Memory_File_Tell_(s_vfs_file* file) {
        if (file == nullptr) {
            return -1;
        }

        return file->position;
    }

    int32_t Memory_File_Close_(s_vfs_file* file) {
        if (file == nullptr) {
            return -1;
        }

        delete file;
        return 0;
    }
} // namespace vfs_detail

namespace vfs {
    int8_t Mount_Embedded_Archive(const void* data, size_t size) {
        mz_zip_archive archive;
        std::vector<vfs_detail::s_vfs_memory_entry> entries;

        if (!vfs_detail::g_vfs_state.initialized || data == nullptr) {
            return 0;
        }

        mz_zip_zero_struct(&archive);
        if (!mz_zip_reader_init_mem(&archive, data, size, 0)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "vfs: embedded archive is not a valid ZIP");
            return 0;
        }

        const bool read_ok = vfs_detail::Read_Embedded_Entries_(&archive, (const uint8_t*)data, size, &entries);
        mz_zip_reader_end(&archive);
        if (!read_ok) {
            return 0;
        }

        vfs_detail::g_vfs_state.embedded_entries = std::move(entries);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "vfs: mounted %u embedded assets", (unsigned)vfs_detail::g_vfs_state.embedded_entries.size());
        return 1;
    }
} // namespace vfs
