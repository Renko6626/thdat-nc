/* Unpacking a PKGL archive.  Open = check magic, read the directory, XOR it
 * with the key from the archive's own basename, parse the 32-byte records.
 * Entry read = seek to offset, XOR with the entry's seed key, then either
 * hand the bytes back as-is or inflate the zstd frame to `size` bytes.
 * Extraction on top of that just sanitises the name and writes the file.
 * zstd is loaded lazily so a plain -l never touches it. */
#include "unpack.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "platform.h"

/* ---- opening ---------------------------------------------------------- */

void
archive_close(pkgl_archive_t* archive)
{
    for (size_t i = 0; i < archive->count; ++i)
        free(archive->entries[i].name);
    free(archive->entries);
    if (archive->stream)
        fclose(archive->stream);
    if (archive->zstd_loaded)
        zstd_close(&archive->zstd);
    memset(archive, 0, sizeof(*archive));
}

/* Walk the (already decrypted) directory into archive->entries. */
static int
parse_directory(pkgl_archive_t* archive, const unsigned char* directory, size_t directory_size)
{
    size_t cursor = 0;
    while (cursor < directory_size) {
        if (directory_size - cursor < PKGL_RECORD_SIZE)
            return fail("truncated directory record at %#lx", (unsigned long)cursor);
        pkgl_entry_t entry;
        record_unpack(directory + cursor, &entry);
        uint16_t name_size = read_u16(directory + cursor + RECORD_NAME_SIZE);
        cursor += PKGL_RECORD_SIZE;
        if (name_size == 0 || name_size > PKGL_NAME_MAX || directory_size - cursor < name_size)
            return fail("invalid name at directory offset %#lx", (unsigned long)cursor);

        entry.name = malloc((size_t)name_size + 1);
        if (!entry.name)
            return oom();
        memcpy(entry.name, directory + cursor, name_size);
        entry.name[name_size] = '\0';
        cursor += name_size;

        if (!reserve_one((void**)&archive->entries, &archive->capacity, archive->count,
                sizeof(*archive->entries))) {
            free(entry.name);
            return 0;
        }
        archive->entries[archive->count++] = entry;
    }
    return 1;
}

int
archive_open(pkgl_archive_t* archive, const char* path)
{
    memset(archive, 0, sizeof(*archive));
    archive->stream = fopen(path, "rb");
    if (!archive->stream)
        return fail("cannot open %s: %s", path, strerror(errno));

    unsigned char header[PKGL_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), archive->stream) != sizeof(header) ||
        memcmp(header, PKGL_MAGIC, 4) != 0) {
        archive_close(archive);
        return fail("%s is not a PKGL archive", path);
    }

    uint32_t directory_size = read_u32(header + 4);
    unsigned char* directory = malloc_nonzero(directory_size);
    int ok = directory != NULL &&
        fread(directory, 1, directory_size, archive->stream) == directory_size;
    if (!ok) {
        fail("truncated directory in %s", path);
    } else {
        unsigned char key[PKGL_KEY_SIZE];
        key_from_seed(archive_seed(path), key);
        xor_with_key(directory, directory_size, key);
        ok = parse_directory(archive, directory, directory_size);
    }
    free(directory);
    if (!ok)
        archive_close(archive);
    return ok;
}

const pkgl_entry_t*
archive_find(const pkgl_archive_t* archive, const char* name)
{
    for (size_t i = 0; i < archive->count; ++i)
        if (strcmp(name, archive->entries[i].name) == 0)
            return &archive->entries[i];
    return NULL;
}

/* ---- reading entries -------------------------------------------------- */

static int
archive_zstd(pkgl_archive_t* archive, const zstd_api_t** api)
{
    if (!archive->zstd_loaded) {
        if (!zstd_open(&archive->zstd))
            return 0;
        archive->zstd_loaded = 1;
    }
    *api = &archive->zstd;
    return 1;
}

static unsigned char*
zstd_inflate(pkgl_archive_t* archive, const pkgl_entry_t* entry,
    const unsigned char* stored, size_t stored_size, size_t size)
{
    const zstd_api_t* zstd;
    if (!archive_zstd(archive, &zstd))
        return NULL;
    unsigned char* data = malloc_nonzero(size);
    if (!data) {
        oom();
        return NULL;
    }
    size_t result = zstd->decompress(data, size, stored, stored_size);
    if (zstd->is_error(result) || result != size) {
        fail("Zstd decode failed for %s: %s", entry->name,
            zstd->is_error(result) ? zstd->error_name(result) : "size mismatch");
        free(data);
        return NULL;
    }
    return data;
}

unsigned char*
entry_read(pkgl_archive_t* archive, const pkgl_entry_t* entry)
{
    if (!fits_size_t(entry->stored_size) || !fits_size_t(entry->size)) {
        fail("entry is too large: %s", entry->name);
        return NULL;
    }
    size_t stored_size = (size_t)entry->stored_size;
    size_t size = (size_t)entry->size;

    unsigned char* stored = malloc_nonzero(stored_size);
    if (!stored) {
        oom();
        return NULL;
    }
    if (!file_seek(archive->stream, entry->offset) ||
        fread(stored, 1, stored_size, archive->stream) != stored_size) {
        fail("truncated payload: %s", entry->name);
        free(stored);
        return NULL;
    }
    unsigned char key[PKGL_KEY_SIZE];
    key_from_seed(entry->seed, key);
    xor_with_key(stored, stored_size, key);

    if (!(entry->flags & PKGL_FLAG_ZSTD)) {
        if (stored_size != size) {
            fail("size mismatch: %s", entry->name);
            free(stored);
            return NULL;
        }
        return stored;
    }
    unsigned char* data = zstd_inflate(archive, entry, stored, stored_size, size);
    free(stored);
    return data;
}

/* ---- extracting ------------------------------------------------------- */

/* No absolute paths, no drive letters, no ".." -- an archive shouldn't be
 * able to write outside -C. */
static int
name_is_safe(const char* name)
{
    if (!*name || *name == '/' || *name == '\\' || strchr(name, ':'))
        return 0;
    const char* component = name;
    for (const char* cursor = name;; ++cursor) {
        if (*cursor != '/' && *cursor != '\\' && *cursor != '\0')
            continue;
        if (cursor - component == 2 && component[0] == '.' && component[1] == '.')
            return 0;
        if (*cursor == '\0')
            return 1;
        component = cursor + 1;
    }
}

/* mkdir -p for everything before the last '/'. */
static int
make_parents(char* path)
{
    for (char* cursor = path; *cursor; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        int ok = !*path || make_directory(path);
        *cursor = '/';
        if (!ok)
            return 0;
    }
    return 1;
}

static int
write_file(const char* path, const unsigned char* data, size_t size)
{
    FILE* output = fopen(path, "wb");
    if (!output)
        return fail("cannot write %s: %s", path, strerror(errno));
    int ok = fwrite(data, 1, size, output) == size;
    if (fclose(output) != 0)
        ok = 0;
    return ok || fail("cannot write %s: %s", path, strerror(errno));
}

int
extract_entry(pkgl_archive_t* archive, const pkgl_entry_t* entry, const char* output_root)
{
    if (!name_is_safe(entry->name))
        return fail("unsafe entry name: %s", entry->name);

    char* relative = copy_string(entry->name);
    if (!relative)
        return oom();
    for (char* cursor = relative; *cursor; ++cursor)
        if (*cursor == '\\')
            *cursor = '/';
    char* output_path = join_path(output_root, relative);
    free(relative);
    if (!output_path)
        return oom();

    int ok = 0;
    if (!make_parents(output_path)) {
        fail("cannot create output directory for %s: %s", output_path, strerror(errno));
    } else {
        unsigned char* data = entry_read(archive, entry);
        ok = data && write_file(output_path, data, (size_t)entry->size);
        free(data);
    }
    if (ok)
        printf("%s\n", entry->name);
    free(output_path);
    return ok;
}
