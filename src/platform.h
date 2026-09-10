/* The handful of places where Windows and POSIX actually differ. */
#ifndef THDAT_NC_PLATFORM_H
#define THDAT_NC_PLATFORM_H

#include <stdint.h>
#include <stdio.h>

int file_seek(FILE* stream, uint64_t offset);
/* Size via seek-to-end; rewinds to the start afterwards. */
int file_size(FILE* stream, uint64_t* size);
/* OK as long as the directory exists afterwards, whoever made it. */
int make_directory(const char* path);
int is_directory(const char* path);

typedef enum {
    NODE_FILE,
    NODE_DIRECTORY,
    NODE_OTHER, /* symlink, reparse point, device... we don't pack those */
} node_kind_t;

/* Tiny directory iterator so the recursive walk exists only once.  After
 * dir_next() returns 0, check `failed` to tell "done" from "broke". */
typedef struct {
    void* handle;      /* DIR* or HANDLE */
    void* find_data;   /* Windows: WIN32_FIND_DATAA*, owned */
    const char* path;  /* POSIX: needed for lstat; not owned */
    int first_pending; /* Windows: FindFirstFile already fetched one */
    int failed;
} dir_iter_t;

int dir_open(dir_iter_t* iter, const char* path);
/* `*name` stays valid until the next dir_next()/dir_close(). */
int dir_next(dir_iter_t* iter, const char** name, node_kind_t* kind);
void dir_close(dir_iter_t* iter);

#endif
