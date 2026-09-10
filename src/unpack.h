/* Unpack side: open an archive, list it, pull entries out, write them to disk. */
#ifndef THDAT_NC_UNPACK_H
#define THDAT_NC_UNPACK_H

#include <stdio.h>

#include "pkgl.h"
#include "zstd_api.h"

typedef struct {
    FILE* stream;
    pkgl_entry_t* entries;
    size_t count;
    size_t capacity;
    zstd_api_t zstd;
    int zstd_loaded; /* lazy: -l never needs zstd at all */
} pkgl_archive_t;

/* On failure the archive is already closed; nothing to clean up. */
int archive_open(pkgl_archive_t* archive, const char* path);
void archive_close(pkgl_archive_t* archive);
const pkgl_entry_t* archive_find(const pkgl_archive_t* archive, const char* name);

/* Seek, decrypt, inflate if needed.  Returns malloc'd entry->size bytes. */
unsigned char* entry_read(pkgl_archive_t* archive, const pkgl_entry_t* entry);
/* Writes output_root/<entry name>, creating directories as needed, and
 * prints the name on success like thdat does. */
int extract_entry(pkgl_archive_t* archive, const pkgl_entry_t* entry, const char* output_root);

#endif
