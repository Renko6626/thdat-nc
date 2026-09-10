/* Windows vs POSIX, kept in one place: 64-bit seek/tell, mkdir, and a small
 * readdir-style iterator.  Paths use '/' on both; Windows doesn't mind. */
#include "platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

int
file_seek(FILE* stream, uint64_t offset)
{
    if (offset > INT64_MAX)
        return 0;
#ifdef _WIN32
    return _fseeki64(stream, (int64_t)offset, SEEK_SET) == 0;
#else
    return fseeko(stream, (off_t)offset, SEEK_SET) == 0;
#endif
}

int
file_size(FILE* stream, uint64_t* size)
{
#ifdef _WIN32
    if (_fseeki64(stream, 0, SEEK_END))
        return 0;
    int64_t end = _ftelli64(stream);
#else
    if (fseeko(stream, 0, SEEK_END))
        return 0;
    off_t end = ftello(stream);
#endif
    if (end < 0)
        return 0;
    *size = (uint64_t)end;
    return file_seek(stream, 0);
}

int
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

int
is_directory(const char* path)
{
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

#ifdef _WIN32

int
dir_open(dir_iter_t* iter, const char* path)
{
    memset(iter, 0, sizeof(*iter));
    WIN32_FIND_DATAA* found = malloc(sizeof(*found));
    char* pattern = join_path(path, "*");
    if (!found || !pattern) {
        free(found);
        free(pattern);
        return oom();
    }
    HANDLE handle = FindFirstFileA(pattern, found);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        free(found);
        return fail("cannot open input directory %s (error %lu)", path, (unsigned long)GetLastError());
    }
    iter->handle = handle;
    iter->find_data = found;
    iter->first_pending = 1;
    return 1;
}

int
dir_next(dir_iter_t* iter, const char** name, node_kind_t* kind)
{
    WIN32_FIND_DATAA* found = iter->find_data;
    if (iter->first_pending)
        iter->first_pending = 0;
    else if (!FindNextFileA(iter->handle, found))
        return 0;
    DWORD attributes = found->dwFileAttributes;
    *name = found->cFileName;
    *kind = attributes & FILE_ATTRIBUTE_REPARSE_POINT ? NODE_OTHER
        : attributes & FILE_ATTRIBUTE_DIRECTORY     ? NODE_DIRECTORY
                                                    : NODE_FILE;
    return 1;
}

void
dir_close(dir_iter_t* iter)
{
    FindClose(iter->handle);
    free(iter->find_data);
}

#else

int
dir_open(dir_iter_t* iter, const char* path)
{
    memset(iter, 0, sizeof(*iter));
    iter->handle = opendir(path);
    if (!iter->handle)
        return fail("cannot open input directory %s: %s", path, strerror(errno));
    iter->path = path;
    return 1;
}

int
dir_next(dir_iter_t* iter, const char** name, node_kind_t* kind)
{
    struct dirent* item = readdir(iter->handle);
    if (!item)
        return 0;
    char* full_path = join_path(iter->path, item->d_name);
    struct stat status;
    int inspected = full_path && lstat(full_path, &status) == 0;
    if (!inspected)
        warn("cannot inspect input %s: %s", item->d_name, strerror(errno));
    free(full_path);
    if (!inspected) {
        iter->failed = 1;
        return 0;
    }
    *name = item->d_name;
    *kind = S_ISDIR(status.st_mode) ? NODE_DIRECTORY : S_ISREG(status.st_mode) ? NODE_FILE : NODE_OTHER;
    return 1;
}

void
dir_close(dir_iter_t* iter)
{
    closedir(iter->handle);
}

#endif
