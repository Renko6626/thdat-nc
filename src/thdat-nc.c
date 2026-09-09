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
#include <windows.h>
#include <zstd.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
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

typedef struct {
    char* name;
    unsigned char* data;
    unsigned char* stored;
    size_t size;
    size_t stored_size;
    uint64_t offset;
    uint32_t seed;
    uint16_t flags;
} input_file_t;

typedef struct {
    input_file_t* files;
    size_t count;
    size_t capacity;
} input_list_t;

#ifndef _WIN32
typedef size_t (*zstd_decompress_t)(void*, size_t, const void*, size_t);
typedef size_t (*zstd_compress_bound_t)(size_t);
typedef size_t (*zstd_compress_t)(void*, size_t, const void*, size_t, int);
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
write_u16(unsigned char* data, uint16_t value)
{
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8);
}

static void
write_u32(unsigned char* data, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i)
        data[i] = (unsigned char)(value >> (i * 8));
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
crc32_update(uint32_t crc, const unsigned char* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc;
}

static uint32_t
crc32_bytes(const unsigned char* data, size_t size)
{
    return crc32_update(UINT32_MAX, data, size) ^ UINT32_MAX;
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

static char*
copy_string(const char* value)
{
    size_t size = strlen(value) + 1;
    char* result = malloc(size);
    if (result)
        memcpy(result, value, size);
    return result;
}

static char*
join_path(const char* left, const char* right, char separator)
{
    size_t left_size = strlen(left);
    size_t right_size = strlen(right);
    int needs_separator = left_size && left[left_size - 1] != '/' && left[left_size - 1] != '\\';
    char* result = malloc(left_size + right_size + (size_t)needs_separator + 1);
    if (!result)
        return NULL;
    memcpy(result, left, left_size);
    if (needs_separator)
        result[left_size++] = separator;
    memcpy(result + left_size, right, right_size + 1);
    return result;
}

static unsigned char*
read_whole_file(const char* path, size_t* size)
{
    FILE* stream = fopen(path, "rb");
    if (!stream) {
        fprintf(stderr, "thdat-nc: cannot open input %s: %s\n", path, strerror(errno));
        return NULL;
    }
#ifdef _WIN32
    if (_fseeki64(stream, 0, SEEK_END) || _ftelli64(stream) < 0) {
        fclose(stream);
        return NULL;
    }
    int64_t length = _ftelli64(stream);
    if ((uint64_t)length > SIZE_MAX || _fseeki64(stream, 0, SEEK_SET)) {
#else
    if (fseeko(stream, 0, SEEK_END)) {
        fclose(stream);
        return NULL;
    }
    off_t length = ftello(stream);
    if (length < 0 || (uint64_t)length > SIZE_MAX || fseeko(stream, 0, SEEK_SET)) {
#endif
        fprintf(stderr, "thdat-nc: input is too large: %s\n", path);
        fclose(stream);
        return NULL;
    }
    *size = (size_t)length;
    unsigned char* data = malloc(*size ? *size : 1);
    if (!data || fread(data, 1, *size, stream) != *size) {
        fprintf(stderr, "thdat-nc: cannot read input %s\n", path);
        free(data);
        fclose(stream);
        return NULL;
    }
    fclose(stream);
    return data;
}

static int
append_input_file(input_list_t* list, const char* root, const char* name)
{
    size_t name_size = strlen(name);
    if (!name_size || name_size > 255) {
        fprintf(stderr, "thdat-nc: input name must contain 1 to 255 bytes: %s\n", name);
        return 0;
    }
    if (list->count == list->capacity) {
        size_t next_capacity = list->capacity ? list->capacity * 2 : 32;
        input_file_t* next = realloc(list->files, next_capacity * sizeof(*next));
        if (!next) {
            fprintf(stderr, "thdat-nc: out of memory\n");
            return 0;
        }
        list->files = next;
        list->capacity = next_capacity;
    }
#ifdef _WIN32
    char* path = join_path(root, name, '\\');
    if (path) {
        for (char* cursor = path + strlen(root); *cursor; ++cursor)
            if (*cursor == '/')
                *cursor = '\\';
    }
#else
    char* path = join_path(root, name, '/');
#endif
    if (!path)
        return 0;
    input_file_t* file = &list->files[list->count];
    memset(file, 0, sizeof(*file));
    file->name = copy_string(name);
    file->data = read_whole_file(path, &file->size);
    free(path);
    if (!file->name || !file->data) {
        free(file->name);
        free(file->data);
        memset(file, 0, sizeof(*file));
        return 0;
    }
    ++list->count;
    return 1;
}

#ifdef _WIN32
static int
collect_inputs(input_list_t* list, const char* root, const char* relative)
{
    char* directory = *relative ? join_path(root, relative, '\\') : copy_string(root);
    if (!directory)
        return 0;
    for (char* cursor = directory + strlen(root); *cursor; ++cursor)
        if (*cursor == '/')
            *cursor = '\\';
    char* pattern = join_path(directory, "*", '\\');
    free(directory);
    if (!pattern)
        return 0;
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "thdat-nc: cannot enumerate input directory: %s\n", relative);
        return 0;
    }
    int ok = 1;
    do {
        if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, ".."))
            continue;
        char* child = *relative ? join_path(relative, found.cFileName, '/') : copy_string(found.cFileName);
        if (!child) {
            ok = 0;
            break;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            fprintf(stderr, "thdat-nc: skipping reparse point: %s\n", child);
        } else if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = collect_inputs(list, root, child);
        } else {
            ok = append_input_file(list, root, child);
        }
        free(child);
    } while (ok && FindNextFileA(search, &found));
    FindClose(search);
    return ok;
}
#else
static int
collect_inputs(input_list_t* list, const char* root, const char* relative)
{
    char* directory = *relative ? join_path(root, relative, '/') : copy_string(root);
    if (!directory)
        return 0;
    DIR* stream = opendir(directory);
    if (!stream) {
        fprintf(stderr, "thdat-nc: cannot open input directory %s: %s\n", directory, strerror(errno));
        free(directory);
        return 0;
    }
    int ok = 1;
    struct dirent* item;
    while (ok && (item = readdir(stream)) != NULL) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
            continue;
        char* child = *relative ? join_path(relative, item->d_name, '/') : copy_string(item->d_name);
        char* path = child ? join_path(root, child, '/') : NULL;
        struct stat status;
        if (!child || !path || lstat(path, &status)) {
            fprintf(stderr, "thdat-nc: cannot inspect input: %s\n", child ? child : item->d_name);
            ok = 0;
        } else if (S_ISDIR(status.st_mode)) {
            ok = collect_inputs(list, root, child);
        } else if (S_ISREG(status.st_mode)) {
            ok = append_input_file(list, root, child);
        } else if (S_ISLNK(status.st_mode)) {
            fprintf(stderr, "thdat-nc: skipping symbolic link: %s\n", child);
        }
        free(path);
        free(child);
    }
    closedir(stream);
    free(directory);
    return ok;
}
#endif

static int
compare_inputs(const void* left, const void* right)
{
    return strcmp(((const input_file_t*)left)->name, ((const input_file_t*)right)->name);
}

static void
free_inputs(input_list_t* list)
{
    for (size_t i = 0; i < list->count; ++i) {
        free(list->files[i].name);
        free(list->files[i].data);
        free(list->files[i].stored);
    }
    free(list->files);
    memset(list, 0, sizeof(*list));
}

static uint32_t
input_seed(const input_file_t* file)
{
    uint32_t crc = crc32_update(UINT32_MAX, (const unsigned char*)file->name, strlen(file->name));
    const unsigned char zero = 0;
    crc = crc32_update(crc, &zero, 1);
    return crc32_update(crc, file->data, file->size) ^ UINT32_MAX;
}

static int
compress_inputs(input_list_t* list)
{
#ifndef _WIN32
    void* library = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
    zstd_compress_bound_t compress_bound = NULL;
    zstd_compress_t compress = NULL;
    zstd_is_error_t is_error = NULL;
    zstd_get_error_name_t error_name = NULL;
    if (library) {
        *(void**)&compress_bound = dlsym(library, "ZSTD_compressBound");
        *(void**)&compress = dlsym(library, "ZSTD_compress");
        *(void**)&is_error = dlsym(library, "ZSTD_isError");
        *(void**)&error_name = dlsym(library, "ZSTD_getErrorName");
    }
    if (!library || !compress_bound || !compress || !is_error || !error_name) {
        fprintf(stderr, "thdat-nc: cannot load complete Zstd runtime\n");
        if (library)
            dlclose(library);
        return 0;
    }
#endif
    int ok = 1;
    for (size_t i = 0; i < list->count; ++i) {
        input_file_t* file = &list->files[i];
#ifdef _WIN32
        size_t bound = ZSTD_compressBound(file->size);
#else
        size_t bound = compress_bound(file->size);
#endif
        unsigned char* candidate = malloc(bound ? bound : 1);
        if (!candidate) {
            ok = 0;
            break;
        }
#ifdef _WIN32
        size_t result = ZSTD_compress(candidate, bound, file->data, file->size, 3);
        int failed = ZSTD_isError(result);
        const char* failure = failed ? ZSTD_getErrorName(result) : NULL;
#else
        size_t result = compress(candidate, bound, file->data, file->size, 3);
        int failed = is_error(result);
        const char* failure = failed ? error_name(result) : NULL;
#endif
        if (failed) {
            fprintf(stderr, "thdat-nc: Zstd encode failed for %s: %s\n", file->name, failure);
            free(candidate);
            ok = 0;
            break;
        }
        if (result < file->size) {
            file->stored = candidate;
            file->stored_size = result;
            file->flags = 1;
        } else {
            free(candidate);
            file->stored = malloc(file->size ? file->size : 1);
            if (!file->stored) {
                ok = 0;
                break;
            }
            memcpy(file->stored, file->data, file->size);
            file->stored_size = file->size;
        }
        file->seed = input_seed(file);
    }
#ifndef _WIN32
    dlclose(library);
#endif
    if (!ok)
        fprintf(stderr, "thdat-nc: out of memory while preparing archive\n");
    return ok;
}

static uint64_t
align16(uint64_t value)
{
    return (value + 15) & ~UINT64_C(15);
}

static int
write_zeros(FILE* stream, uint64_t count)
{
    static const unsigned char zeros[16] = {0};
    while (count) {
        size_t chunk = count > sizeof(zeros) ? sizeof(zeros) : (size_t)count;
        if (fwrite(zeros, 1, chunk, stream) != chunk)
            return 0;
        count -= chunk;
    }
    return 1;
}

static int
archive_create(const char* output_path, const char* input_root)
{
    input_list_t list = {0};
    unsigned char* directory = NULL;
    FILE* output = NULL;
    int ok = 0;
    struct stat root_status;
    if (stat(input_root, &root_status) || !S_ISDIR(root_status.st_mode)) {
        fprintf(stderr, "thdat-nc: input is not a directory: %s\n", input_root);
        goto done;
    }
    if (!collect_inputs(&list, input_root, "") || !list.count) {
        if (!list.count)
            fprintf(stderr, "thdat-nc: input directory contains no regular files\n");
        goto done;
    }
    qsort(list.files, list.count, sizeof(*list.files), compare_inputs);
    if (!compress_inputs(&list))
        goto done;

    uint64_t directory_size64 = 0;
    for (size_t i = 0; i < list.count; ++i)
        directory_size64 += 32 + strlen(list.files[i].name);
    if (directory_size64 > UINT32_MAX) {
        fprintf(stderr, "thdat-nc: archive directory is too large\n");
        goto done;
    }
    uint32_t directory_size = (uint32_t)directory_size64;
    uint64_t next_offset = align16(8 + directory_size64);
    for (size_t i = 0; i < list.count; ++i) {
        list.files[i].offset = next_offset;
        if (UINT64_MAX - next_offset < list.files[i].stored_size) {
            fprintf(stderr, "thdat-nc: archive is too large\n");
            goto done;
        }
        next_offset += list.files[i].stored_size;
        if (i + 1 < list.count)
            next_offset = align16(next_offset);
    }

    directory = malloc(directory_size ? directory_size : 1);
    if (!directory)
        goto done;
    size_t cursor = 0;
    for (size_t i = 0; i < list.count; ++i) {
        input_file_t* file = &list.files[i];
        size_t name_size = strlen(file->name);
        write_u16(directory + cursor, file->flags);
        write_u32(directory + cursor + 2, file->seed);
        write_u64(directory + cursor + 6, file->size);
        write_u64(directory + cursor + 14, file->stored_size);
        write_u64(directory + cursor + 22, file->offset);
        write_u16(directory + cursor + 30, (uint16_t)name_size);
        memcpy(directory + cursor + 32, file->name, name_size);
        cursor += 32 + name_size;
    }
    unsigned char key[16];
    xor_key(archive_seed(output_path), key);
    for (size_t i = 0; i < directory_size; ++i)
        directory[i] ^= key[i & 15];

    output = fopen(output_path, "wb");
    if (!output) {
        fprintf(stderr, "thdat-nc: cannot create %s: %s\n", output_path, strerror(errno));
        goto done;
    }
    unsigned char header[8] = {'P', 'K', 'G', 'L'};
    write_u32(header + 4, directory_size);
    if (fwrite(header, 1, sizeof(header), output) != sizeof(header) ||
        fwrite(directory, 1, directory_size, output) != directory_size ||
        !write_zeros(output, list.files[0].offset - 8 - directory_size))
        goto write_fail;
    for (size_t i = 0; i < list.count; ++i) {
        input_file_t* file = &list.files[i];
        xor_key(file->seed, key);
        for (size_t byte = 0; byte < file->stored_size; ++byte)
            file->stored[byte] ^= key[byte & 15];
        if (fwrite(file->stored, 1, file->stored_size, output) != file->stored_size)
            goto write_fail;
        if (i + 1 < list.count &&
            !write_zeros(output, list.files[i + 1].offset - file->offset - file->stored_size))
            goto write_fail;
        printf("%s\n", file->name);
    }
    if (fputc(0, output) == EOF || fclose(output)) {
        output = NULL;
        goto write_fail;
    }
    output = NULL;
    ok = 1;
    goto done;

write_fail:
    fprintf(stderr, "thdat-nc: cannot write %s: %s\n", output_path, strerror(errno));
done:
    if (output)
        fclose(output);
    free(directory);
    free_inputs(&list);
    return ok;
}

static void
usage(FILE* stream)
{
    fprintf(stream,
        "Usage:\n"
        "  thdat-nc -c ARCHIVE INPUT-DIRECTORY\n"
        "  thdat-nc -l ARCHIVE\n"
        "  thdat-nc -x [-C DIR] ARCHIVE [FILE...]\n"
        "  thdat-nc -h\n"
        "  -c  create a deterministic PKGL archive\n"
        "  -l  list PKGL archive entries\n"
        "  -x  extract all or selected entries\n"
        "  -C  extract below DIR (default: current directory)\n"
        "  -h  show this help\n");
}

int
main(int argc, char** argv)
{
    int mode = 0;
    const char* output_root = ".";
    int option;
    while ((option = getopt(argc, argv, "clxC:h")) != -1) {
        switch (option) {
        case 'c': mode = 'c'; break;
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

    if (mode == 'c') {
        if (optind + 2 != argc) {
            usage(stderr);
            return 2;
        }
        return archive_create(argv[optind], argv[optind + 1]) ? 0 : 1;
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
