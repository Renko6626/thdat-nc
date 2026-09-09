#define _FILE_OFFSET_BITS 64
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <zstd.h>
#else
#include <dlfcn.h>
#endif

typedef struct {
    uint16_t flags;
    uint32_t checksum;
    uint64_t size;
    uint64_t stored_size;
    uint64_t offset;
    char* name;
} pkgl_entry_t;

typedef struct {
    FILE* stream;
    pkgl_entry_t* entries;
    size_t entry_count;
} pkgl_archive_t;

#ifndef _WIN32
typedef size_t (*zstd_decompress_t)(void*, size_t, const void*, size_t);
typedef unsigned int (*zstd_is_error_t)(size_t);
typedef const char* (*zstd_get_error_name_t)(size_t);
#endif

static uint16_t
read_u16(const unsigned char* data)
{
    return (uint16_t)data[0] | (uint16_t)data[1] << 8;
}

static uint32_t
read_u32(const unsigned char* data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
        (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t
read_u64(const unsigned char* data)
{
    return (uint64_t)read_u32(data) | (uint64_t)read_u32(data + 4) << 32;
}

static void
write_u64(unsigned char* data, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i)
        data[i] = (unsigned char)(value >> (i * 8));
}

static uint64_t
mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static void
xor_key(uint32_t seed, unsigned char key[16])
{
    const uint64_t golden = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t state = ((uint64_t)seed * UINT32_C(0x9e3779b1) + 1) ^
        ((uint64_t)seed << 32);
    write_u64(key, mix64(state + golden));
    write_u64(key + 8, mix64(state + golden * 2));
}

static uint32_t
crc32_bytes(const unsigned char* data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc ^ UINT32_MAX;
}

static uint32_t
archive_seed(const char* path)
{
    const char* base = path;
    const char* dot = NULL;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
            dot = NULL;
        } else if (*cursor == '.') {
            dot = cursor;
        }
    }
    size_t length = dot && dot > base ? (size_t)(dot - base) : strlen(base);
    return crc32_bytes((const unsigned char*)base, length);
}

static void
archive_free(pkgl_archive_t* archive)
{
    if (!archive)
        return;
    for (size_t i = 0; i < archive->entry_count; ++i)
        free(archive->entries[i].name);
    free(archive->entries);
    if (archive->stream)
        fclose(archive->stream);
    memset(archive, 0, sizeof(*archive));
}

static int
archive_open(pkgl_archive_t* archive, const char* path)
{
    unsigned char header[8];
    unsigned char* directory = NULL;
    unsigned char key[16];
    size_t cursor = 0;
    size_t capacity = 0;

    memset(archive, 0, sizeof(*archive));
    archive->stream = fopen(path, "rb");
    if (!archive->stream) {
        fprintf(stderr, "thdat-nc: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }
    if (fread(header, 1, sizeof(header), archive->stream) != sizeof(header) ||
        memcmp(header, "PKGL", 4) != 0) {
        fprintf(stderr, "thdat-nc: %s is not a PKGL archive\n", path);
        goto fail;
    }

    uint32_t directory_size = read_u32(header + 4);
    directory = malloc(directory_size ? directory_size : 1);
    if (!directory || fread(directory, 1, directory_size, archive->stream) != directory_size) {
        fprintf(stderr, "thdat-nc: truncated directory in %s\n", path);
        goto fail;
    }
    xor_key(archive_seed(path), key);
    for (size_t i = 0; i < directory_size; ++i)
        directory[i] ^= key[i & 15];

    while (cursor < directory_size) {
        if ((size_t)directory_size - cursor < 32) {
            fprintf(stderr, "thdat-nc: truncated directory record at %#zx\n", cursor);
            goto fail;
        }
        const unsigned char* record = directory + cursor;
        uint16_t name_size = read_u16(record + 30);
        cursor += 32;
        if (name_size == 0 || name_size > 255 || (size_t)directory_size - cursor < name_size) {
            fprintf(stderr, "thdat-nc: invalid name at directory offset %#zx\n", cursor);
            goto fail;
        }
        if (archive->entry_count == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 32;
            pkgl_entry_t* next = realloc(archive->entries, next_capacity * sizeof(*next));
            if (!next) {
                fprintf(stderr, "thdat-nc: out of memory\n");
                goto fail;
            }
            archive->entries = next;
            capacity = next_capacity;
        }
        pkgl_entry_t* entry = &archive->entries[archive->entry_count];
        memset(entry, 0, sizeof(*entry));
        entry->flags = read_u16(record);
        entry->checksum = read_u32(record + 2);
        entry->size = read_u64(record + 6);
        entry->stored_size = read_u64(record + 14);
        entry->offset = read_u64(record + 22);
        entry->name = malloc((size_t)name_size + 1);
        if (!entry->name) {
            fprintf(stderr, "thdat-nc: out of memory\n");
            goto fail;
        }
        memcpy(entry->name, directory + cursor, name_size);
        entry->name[name_size] = '\0';
        cursor += name_size;
        ++archive->entry_count;
    }
    free(directory);
    return 1;

fail:
    free(directory);
    archive_free(archive);
    return 0;
}

#ifndef _WIN32
static int
load_zstd(void** library, zstd_decompress_t* decompress,
    zstd_is_error_t* is_error, zstd_get_error_name_t* error_name)
{
    *library = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!*library) {
        fprintf(stderr, "thdat-nc: cannot load libzstd.so.1: %s\n", dlerror());
        return 0;
    }
    *(void**)decompress = dlsym(*library, "ZSTD_decompress");
    *(void**)is_error = dlsym(*library, "ZSTD_isError");
    *(void**)error_name = dlsym(*library, "ZSTD_getErrorName");
    if (!*decompress || !*is_error || !*error_name) {
        fprintf(stderr, "thdat-nc: incomplete Zstd runtime\n");
        dlclose(*library);
        *library = NULL;
        return 0;
    }
    return 1;
}
#endif

static int
seek_to(FILE* stream, uint64_t offset)
{
    if (offset > INT64_MAX)
        return 0;
#ifdef _WIN32
    return _fseeki64(stream, (int64_t)offset, SEEK_SET) == 0;
#else
    return fseeko(stream, (off_t)offset, SEEK_SET) == 0;
#endif
}

static unsigned char*
entry_decode(pkgl_archive_t* archive, const pkgl_entry_t* entry)
{
    if (entry->stored_size > SIZE_MAX || entry->size > SIZE_MAX || entry->offset > INT64_MAX) {
        fprintf(stderr, "thdat-nc: entry is too large: %s\n", entry->name);
        return NULL;
    }
    size_t stored_size = (size_t)entry->stored_size;
    size_t output_size = (size_t)entry->size;
    unsigned char* stored = malloc(stored_size ? stored_size : 1);
    if (!stored) {
        fprintf(stderr, "thdat-nc: out of memory reading %s\n", entry->name);
        return NULL;
    }
    if (!seek_to(archive->stream, entry->offset) ||
        fread(stored, 1, stored_size, archive->stream) != stored_size) {
        fprintf(stderr, "thdat-nc: truncated payload: %s\n", entry->name);
        free(stored);
        return NULL;
    }
    unsigned char key[16];
    xor_key(entry->checksum, key);
    for (size_t i = 0; i < stored_size; ++i)
        stored[i] ^= key[i & 15];

    if (!(entry->flags & 1)) {
        if (stored_size != output_size) {
            fprintf(stderr, "thdat-nc: size mismatch: %s\n", entry->name);
            free(stored);
            return NULL;
        }
        return stored;
    }

    unsigned char* output = malloc(output_size ? output_size : 1);
#ifdef _WIN32
    if (!output) {
        free(stored);
        return NULL;
    }
    size_t result = ZSTD_decompress(output, output_size, stored, stored_size);
    free(stored);
    if (ZSTD_isError(result) || result != output_size) {
        fprintf(stderr, "thdat-nc: Zstd decode failed for %s: %s\n",
            entry->name, ZSTD_isError(result) ? ZSTD_getErrorName(result) : "size mismatch");
        free(output);
        return NULL;
    }
    return output;
#else
    void* library = NULL;
    zstd_decompress_t decompress = NULL;
    zstd_is_error_t is_error = NULL;
    zstd_get_error_name_t error_name = NULL;
    if (!output || !load_zstd(&library, &decompress, &is_error, &error_name)) {
        free(output);
        free(stored);
        return NULL;
    }
    size_t result = decompress(output, output_size, stored, stored_size);
    free(stored);
    if (is_error(result) || result != output_size) {
        fprintf(stderr, "thdat-nc: Zstd decode failed for %s: %s\n",
            entry->name, is_error(result) ? error_name(result) : "size mismatch");
        free(output);
        dlclose(library);
        return NULL;
    }
    dlclose(library);
    return output;
#endif
}

static int
safe_name(const char* name)
{
    if (!*name || *name == '/' || *name == '\\' || strchr(name, ':'))
        return 0;
    const char* component = name;
    for (const char* cursor = name;; ++cursor) {
        if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
            size_t length = (size_t)(cursor - component);
            if (length == 2 && component[0] == '.' && component[1] == '.')
                return 0;
            if (*cursor == '\0')
                return 1;
            component = cursor + 1;
        }
    }
}

static int
make_directory(const char* path)
{
    if (strcmp(path, ".") == 0)
        return 1;
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0777) == 0 || errno == EEXIST;
#endif
}

static int
make_parents(char* path)
{
    for (char* cursor = path; *cursor; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (*path && !make_directory(path)) {
            *cursor = '/';
            return 0;
        }
        *cursor = '/';
    }
    return 1;
}

static int
extract_entry(pkgl_archive_t* archive, const pkgl_entry_t* entry, const char* output_root)
{
    if (!safe_name(entry->name)) {
        fprintf(stderr, "thdat-nc: unsafe entry name: %s\n", entry->name);
        return 0;
    }
    size_t root_length = strlen(output_root);
    size_t name_length = strlen(entry->name);
    char* output_path = malloc(root_length + name_length + 2);
    if (!output_path)
        return 0;
    memcpy(output_path, output_root, root_length);
    output_path[root_length] = '/';
    for (size_t i = 0; i <= name_length; ++i)
        output_path[root_length + 1 + i] = entry->name[i] == '\\' ? '/' : entry->name[i];
    if (!make_parents(output_path)) {
        fprintf(stderr, "thdat-nc: cannot create output directory for %s\n", output_path);
        free(output_path);
        return 0;
    }

    unsigned char* data = entry_decode(archive, entry);
    if (!data) {
        free(output_path);
        return 0;
    }
    FILE* output = fopen(output_path, "wb");
    if (!output || fwrite(data, 1, (size_t)entry->size, output) != entry->size) {
        fprintf(stderr, "thdat-nc: cannot write %s: %s\n", output_path, strerror(errno));
        if (output)
            fclose(output);
        free(data);
        free(output_path);
        return 0;
    }
    fclose(output);
    free(data);
    printf("%s\n", entry->name);
    free(output_path);
    return 1;
}

static void
usage(FILE* stream)
{
    fprintf(stream,
        "Usage: thdat-nc (-l | -x) [-C DIR] ARCHIVE [FILE...]\n"
        "  -l  list PKGL archive entries\n"
        "  -x  extract all or selected entries\n"
        "  -C  extract below DIR (default: current directory)\n");
}

int
main(int argc, char** argv)
{
    int mode = 0;
    const char* output_root = ".";
    int option;
    while ((option = getopt(argc, argv, "lxC:h")) != -1) {
        switch (option) {
        case 'l': mode = 'l'; break;
        case 'x': mode = 'x'; break;
        case 'C': output_root = optarg; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (!mode || optind >= argc) {
        usage(stderr);
        return 2;
    }

    pkgl_archive_t archive;
    if (!archive_open(&archive, argv[optind]))
        return 1;
    ++optind;
    int ok = 1;
    if (mode == 'l') {
        printf("Name\tSize\tStored\n");
        for (size_t i = 0; i < archive.entry_count; ++i)
            printf("%s\t%" PRIu64 "\t%" PRIu64 "\n", archive.entries[i].name,
                archive.entries[i].size, archive.entries[i].stored_size);
    } else if (optind == argc) {
        for (size_t i = 0; i < archive.entry_count; ++i)
            ok &= extract_entry(&archive, &archive.entries[i], output_root);
    } else {
        for (int argument = optind; argument < argc; ++argument) {
            const pkgl_entry_t* found = NULL;
            for (size_t i = 0; i < archive.entry_count; ++i) {
                if (strcmp(argv[argument], archive.entries[i].name) == 0) {
                    found = &archive.entries[i];
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "thdat-nc: entry not found: %s\n", argv[argument]);
                ok = 0;
            } else {
                ok &= extract_entry(&archive, found, output_root);
            }
        }
    }
    archive_free(&archive);
    return ok ? 0 : 1;
}
