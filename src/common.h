/* Bits every module wants: error printing, little-endian byte helpers, and a
 * few tiny string/array helpers that C forgot to ship with. */
#ifndef THDAT_NC_COMMON_H
#define THDAT_NC_COMMON_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define PRINTF_LIKE(format_index, first_argument) \
    __attribute__((format(printf, format_index, first_argument)))
#else
#define PRINTF_LIKE(format_index, first_argument)
#endif

/* Both print "thdat-nc: <message>" to stderr.  fail() returns 0 so error
 * paths can be a one-liner: return fail("..."). */
void warn(const char* format, ...) PRINTF_LIKE(1, 2);
int fail(const char* format, ...) PRINTF_LIKE(1, 2);
int oom(void);

uint16_t read_u16(const unsigned char* data);
uint32_t read_u32(const unsigned char* data);
uint64_t read_u64(const unsigned char* data);
void write_u16(unsigned char* data, uint16_t value);
void write_u32(unsigned char* data, uint32_t value);
void write_u64(unsigned char* data, uint64_t value);

uint64_t align_up(uint64_t value, uint64_t alignment);
/* The format has 64-bit sizes; a 32-bit build can't malloc that much. */
int fits_size_t(uint64_t value);
/* malloc(0) may return NULL; this never does for a good reason. */
void* malloc_nonzero(size_t size);

char* copy_string(const char* value);
/* Always joins with '/'.  Windows is fine with that, no need for backslash games. */
char* join_path(const char* left, const char* right);
/* Make room for one more element (doubling growth). */
int reserve_one(void** items, size_t* capacity, size_t count, size_t item_size);

#endif
