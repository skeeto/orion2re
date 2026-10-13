// Packs files into a ZIP for embedding in the executable. Entries are stored
// uncompressed so vfs::Mount_Embedded_Archive can serve them in place.
//
// Usage: pack-assets <manifest> <output.zip>
// Each manifest line is "<archive name>\t<source path>".

#include <cstdio>
#include <cstring>

#include "miniz.h"

namespace {
    bool Pack_(FILE* manifest, mz_zip_archive* archive, int* entry_count) {
        char line[4096];
        int line_number = 0;

        while (fgets(line, sizeof(line), manifest) != nullptr) {
            ++line_number;
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0') {
                continue;
            }

            char* tab = strchr(line, '\t');
            if (tab == nullptr) {
                fprintf(stderr, "pack-assets: manifest line %d has no tab\n", line_number);
                return false;
            }
            *tab = '\0';
            const char* archive_name = line;
            const char* source_path = tab + 1;

            if (!mz_zip_writer_add_file(archive, archive_name, source_path, nullptr, 0, MZ_NO_COMPRESSION)) {
                fprintf(stderr, "pack-assets: cannot add '%s' from '%s': %s\n", archive_name, source_path,
                    mz_zip_get_error_string(mz_zip_get_last_error(archive)));
                return false;
            }
            ++*entry_count;
        }

        return true;
    }
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: pack-assets <manifest> <output.zip>\n");
        return 2;
    }

    const char* manifest_path = argv[1];
    const char* output_path = argv[2];

    FILE* manifest = fopen(manifest_path, "rb");
    if (manifest == nullptr) {
        fprintf(stderr, "pack-assets: cannot open manifest '%s'\n", manifest_path);
        return 1;
    }

    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!mz_zip_writer_init_file(&archive, output_path, 0)) {
        fprintf(stderr, "pack-assets: cannot create '%s'\n", output_path);
        fclose(manifest);
        return 1;
    }

    int entry_count = 0;
    bool ok = Pack_(manifest, &archive, &entry_count);
    fclose(manifest);
    ok = mz_zip_writer_finalize_archive(&archive) && ok;
    ok = mz_zip_writer_end(&archive) && ok;
    if (!ok) {
        remove(output_path);
        return 1;
    }

    printf("pack-assets: stored %d entries in '%s'\n", entry_count, output_path);
    return 0;
}
