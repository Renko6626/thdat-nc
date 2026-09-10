/* Zstandard behind one function table.  Windows gets libzstd linked in
 * statically; Linux dlopens the system libzstd.so.1 so you don't need headers
 * or a -dev package to build.  Callers just use the table and don't care. */
#ifndef THDAT_NC_ZSTD_API_H
#define THDAT_NC_ZSTD_API_H

#include <stddef.h>

#define ZSTD_LEVEL 3

typedef struct {
    size_t (*compress_bound)(size_t);
    size_t (*compress)(void*, size_t, const void*, size_t, int);
    size_t (*decompress)(void*, size_t, const void*, size_t);
    unsigned int (*is_error)(size_t);
    const char* (*error_name)(size_t);
    void* library; /* dlopen handle on Linux, unused on Windows */
} zstd_api_t;

int zstd_open(zstd_api_t* api);
void zstd_close(zstd_api_t* api);

#endif
