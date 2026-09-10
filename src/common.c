/* Boring utility functions.  Nothing here knows about PKGL; if you are
 * hunting for the format, you want pkgl.c. */
#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
warn(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("thdat-nc: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

int
fail(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("thdat-nc: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    return 0;
}

int
oom(void)
{
    return fail("out of memory");
}

uint16_t
read_u16(const unsigned char* data)
{
    return (uint16_t)(data[0] | data[1] << 8);
}

uint32_t
read_u32(const unsigned char* data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
        (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

uint64_t
read_u64(const unsigned char* data)
{
    return (uint64_t)read_u32(data) | (uint64_t)read_u32(data + 4) << 32;
}

void
write_u16(unsigned char* data, uint16_t value)
{
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8);
}

void
write_u32(unsigned char* data, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i)
        data[i] = (unsigned char)(value >> (i * 8));
}

void
write_u64(unsigned char* data, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i)
        data[i] = (unsigned char)(value >> (i * 8));
}

uint64_t
align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

int
fits_size_t(uint64_t value)
{
#if SIZE_MAX < UINT64_MAX
    return value <= SIZE_MAX;
#else
    (void)value;
    return 1;
#endif
}

void*
malloc_nonzero(size_t size)
{
    return malloc(size ? size : 1);
}

char*
copy_string(const char* value)
{
    size_t size = strlen(value) + 1;
    char* result = malloc(size);
    if (result)
        memcpy(result, value, size);
    return result;
}

char*
join_path(const char* left, const char* right)
{
    size_t left_size = strlen(left);
    size_t right_size = strlen(right);
    int needs_separator = left_size && left[left_size - 1] != '/' && left[left_size - 1] != '\\';
    char* result = malloc(left_size + right_size + (size_t)needs_separator + 1);
    if (!result)
        return NULL;
    memcpy(result, left, left_size);
    if (needs_separator)
        result[left_size++] = '/';
    memcpy(result + left_size, right, right_size + 1);
    return result;
}

int
reserve_one(void** items, size_t* capacity, size_t count, size_t item_size)
{
    if (count < *capacity)
        return 1;
    size_t next_capacity = *capacity ? *capacity * 2 : 32;
    void* grown = realloc(*items, next_capacity * item_size);
    if (!grown)
        return oom();
    *items = grown;
    *capacity = next_capacity;
    return 1;
}
