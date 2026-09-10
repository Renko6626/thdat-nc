/* Packing a directory into a PKGL archive.  The whole recipe:
 *
 *   1. walk the tree, read every regular file, sort by name (bytewise, so
 *      the order never depends on the filesystem)
 *   2. zstd each file, compare sizes, keep whichever is smaller -- flag bit 0
 *      says which one won
 *   3. lay out: 8-byte header, directory, then payloads on 16-byte boundaries
 *      with zero padding between them
 *   4. build the directory, XOR it with the key from the output file's basename
 *   5. write header + directory + each payload XOR'd with its own seed key,
 *      then one trailing zero byte
 *
 * Same input tree and same output basename -> byte-identical archive.  Every
 * entry gets the same placeholder seed ("poo"); see pkgl.h for why. */
#include "pack.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "pkgl.h"
#include "platform.h"
#include "zstd_api.h"

typedef struct {
    char* name;            /* archive name: '/'-separated, relative to the input root */
    unsigned char* data;   /* original content; handed over to `stored` when kept raw */
    size_t size;
    unsigned char* stored; /* bytes that go into the archive, set by pack_input */
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

/* ---- collecting ------------------------------------------------------- */

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

static unsigned char*
read_whole_file(const char* path, size_t* size)
{
    FILE* stream = fopen(path, "rb");
    if (!stream) {
        fail("cannot open input %s: %s", path, strerror(errno));
        return NULL;
    }
    uint64_t length = 0;
    unsigned char* data = NULL;
    if (!file_size(stream, &length) || !fits_size_t(length)) {
        fail("cannot size input %s", path);
    } else if (!(data = malloc_nonzero((size_t)length))) {
        oom();
    } else if (fread(data, 1, (size_t)length, stream) != length) {
        fail("cannot read input %s", path);
        free(data);
        data = NULL;
    } else {
        *size = (size_t)length;
    }
    fclose(stream);
    return data;
}

static int
append_input_file(input_list_t* list, const char* root, const char* name)
{
    if (strlen(name) > PKGL_NAME_MAX)
        return fail("input name exceeds %d bytes: %s", PKGL_NAME_MAX, name);
    char* path = join_path(root, name);
    if (!path)
        return oom();

    input_file_t file = {0};
    file.data = read_whole_file(path, &file.size);
    free(path);
    if (!file.data)
        return 0;
    file.name = copy_string(name);
    if (!file.name) {
        free(file.data);
        return oom();
    }
    if (!reserve_one((void**)&list->files, &list->capacity, list->count, sizeof(*list->files))) {
        free(file.name);
        free(file.data);
        return 0;
    }
    file.seed = PKGL_PLACEHOLDER_SEED;
    list->files[list->count++] = file;
    return 1;
}

/* Recursive walk.  `relative` is "" at the top and grows with '/'; it is
 * also exactly the name the entry gets inside the archive. */
static int
collect_inputs(input_list_t* list, const char* root, const char* relative)
{
    char* directory = *relative ? join_path(root, relative) : copy_string(root);
    if (!directory)
        return oom();
    dir_iter_t iter;
    if (!dir_open(&iter, directory)) {
        free(directory);
        return 0;
    }

    int ok = 1;
    const char* name;
    node_kind_t kind;
    while (ok && dir_next(&iter, &name, &kind)) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char* child = *relative ? join_path(relative, name) : copy_string(name);
        if (!child) {
            ok = oom();
            break;
        }
        if (kind == NODE_DIRECTORY)
            ok = collect_inputs(list, root, child);
        else if (kind == NODE_FILE)
            ok = append_input_file(list, root, child);
        else
            warn("skipping %s: not a regular file", child);
        free(child);
    }
    ok = ok && !iter.failed;
    dir_close(&iter);
    free(directory);
    return ok;
}

static int
compare_inputs(const void* left, const void* right)
{
    return strcmp(((const input_file_t*)left)->name, ((const input_file_t*)right)->name);
}

/* ---- packing ---------------------------------------------------------- */

/* zstd it, keep the result only if it actually got smaller. */
static int
pack_input(const zstd_api_t* zstd, input_file_t* file)
{
    size_t bound = zstd->compress_bound(file->size);
    unsigned char* frame = malloc_nonzero(bound);
    if (!frame)
        return oom();
    size_t result = zstd->compress(frame, bound, file->data, file->size, ZSTD_LEVEL);
    if (zstd->is_error(result)) {
        free(frame);
        return fail("Zstd encode failed for %s: %s", file->name, zstd->error_name(result));
    }
    if (result < file->size) {
        file->stored = frame;
        file->stored_size = result;
        file->flags = PKGL_FLAG_ZSTD;
        free(file->data);
    } else {
        free(frame);
        file->stored = file->data;
        file->stored_size = file->size;
    }
    file->data = NULL;
    return 1;
}

/* Lay out the file: header, directory, then payloads each on a 16-byte
 * boundary.  Also hands back the directory size. */
static int
layout_inputs(input_list_t* list, uint32_t* directory_size)
{
    uint64_t directory_bytes = 0;
    for (size_t i = 0; i < list->count; ++i)
        directory_bytes += PKGL_RECORD_SIZE + strlen(list->files[i].name);
    if (directory_bytes > UINT32_MAX)
        return fail("archive directory is too large");

    uint64_t next_offset = align_up(PKGL_HEADER_SIZE + directory_bytes, PKGL_PAYLOAD_ALIGN);
    for (size_t i = 0; i < list->count; ++i) {
        input_file_t* file = &list->files[i];
        if (UINT64_MAX - next_offset < file->stored_size)
            return fail("archive is too large");
        file->offset = next_offset;
        next_offset = align_up(next_offset + file->stored_size, PKGL_PAYLOAD_ALIGN);
    }
    *directory_size = (uint32_t)directory_bytes;
    return 1;
}

/* Build and encrypt the directory.  Needs the output path because the key
 * comes from the archive's basename. */
static unsigned char*
build_directory(const input_list_t* list, uint32_t directory_size, const char* archive_path)
{
    unsigned char* directory = malloc_nonzero(directory_size);
    if (!directory) {
        oom();
        return NULL;
    }
    size_t cursor = 0;
    for (size_t i = 0; i < list->count; ++i) {
        const input_file_t* file = &list->files[i];
        pkgl_entry_t entry = {
            file->flags, file->seed, file->size, file->stored_size, file->offset, file->name};
        record_pack(directory + cursor, &entry);
        cursor += PKGL_RECORD_SIZE + strlen(file->name);
    }
    unsigned char key[PKGL_KEY_SIZE];
    key_from_seed(archive_seed(archive_path), key);
    xor_with_key(directory, directory_size, key);
    return directory;
}

static int
write_zeros(FILE* stream, uint64_t count)
{
    static const unsigned char zeros[64] = {0};
    while (count) {
        size_t chunk = count > sizeof(zeros) ? sizeof(zeros) : (size_t)count;
        if (fwrite(zeros, 1, chunk, stream) != chunk)
            return 0;
        count -= chunk;
    }
    return 1;
}

/* Write it all out.  Payloads are XOR'd in place right before writing. */
static int
write_archive(const char* path, input_list_t* list, const unsigned char* directory,
    uint32_t directory_size)
{
    FILE* output = fopen(path, "wb");
    if (!output)
        return fail("cannot create %s: %s", path, strerror(errno));

    unsigned char header[PKGL_HEADER_SIZE] = PKGL_MAGIC;
    write_u32(header + 4, directory_size);
    int ok = fwrite(header, 1, sizeof(header), output) == sizeof(header) &&
        fwrite(directory, 1, directory_size, output) == directory_size;
    uint64_t position = PKGL_HEADER_SIZE + directory_size;
    for (size_t i = 0; ok && i < list->count; ++i) {
        input_file_t* file = &list->files[i];
        unsigned char key[PKGL_KEY_SIZE];
        key_from_seed(file->seed, key);
        xor_with_key(file->stored, file->stored_size, key);
        ok = write_zeros(output, file->offset - position) &&
            fwrite(file->stored, 1, file->stored_size, output) == file->stored_size;
        position = file->offset + file->stored_size;
        if (ok)
            printf("%s\n", file->name);
    }
    /* One trailing zero byte.  The tests pin it; don't drop it casually. */
    ok = ok && fputc(0, output) != EOF;
    if (fclose(output) != 0)
        ok = 0;
    return ok || fail("cannot write %s: %s", path, strerror(errno));
}

int
archive_create(const char* output_path, const char* input_root)
{
    if (!is_directory(input_root))
        return fail("input is not a directory: %s", input_root);

    input_list_t list = {0};
    zstd_api_t zstd;
    int zstd_loaded = 0;
    unsigned char* directory = NULL;
    uint32_t directory_size = 0;
    int ok = 0;

    if (!collect_inputs(&list, input_root, ""))
        goto done;
    if (!list.count) {
        fail("input directory contains no regular files");
        goto done;
    }
    qsort(list.files, list.count, sizeof(*list.files), compare_inputs);

    if (!zstd_open(&zstd))
        goto done;
    zstd_loaded = 1;
    for (size_t i = 0; i < list.count; ++i)
        if (!pack_input(&zstd, &list.files[i]))
            goto done;

    if (!layout_inputs(&list, &directory_size))
        goto done;
    directory = build_directory(&list, directory_size, output_path);
    if (!directory)
        goto done;
    ok = write_archive(output_path, &list, directory, directory_size);

done:
    free(directory);
    if (zstd_loaded)
        zstd_close(&zstd);
    free_inputs(&list);
    return ok;
}
