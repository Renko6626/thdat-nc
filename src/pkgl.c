/* Everything that is "the format" and nothing that is I/O: record layout,
 * the XOR key schedule, crc32, and the directory-key seed.  Pack and unpack
 * both go through here so a format detail is only ever written down once. */
#include "pkgl.h"

#include <string.h>

#include "common.h"

void
record_unpack(const unsigned char* record, pkgl_entry_t* entry)
{
    entry->flags = read_u16(record + RECORD_FLAGS);
    entry->seed = read_u32(record + RECORD_SEED);
    entry->size = read_u64(record + RECORD_SIZE);
    entry->stored_size = read_u64(record + RECORD_STORED_SIZE);
    entry->offset = read_u64(record + RECORD_OFFSET);
    entry->name = NULL;
}

void
record_pack(unsigned char* record, const pkgl_entry_t* entry)
{
    size_t name_size = strlen(entry->name);
    write_u16(record + RECORD_FLAGS, entry->flags);
    write_u32(record + RECORD_SEED, entry->seed);
    write_u64(record + RECORD_SIZE, entry->size);
    write_u64(record + RECORD_STORED_SIZE, entry->stored_size);
    write_u64(record + RECORD_OFFSET, entry->offset);
    write_u16(record + RECORD_NAME_SIZE, (uint16_t)name_size);
    memcpy(record + PKGL_RECORD_SIZE, entry->name, name_size);
}

/* splitmix64's finaliser, verbatim. */
static uint64_t
mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

/* The 0x9e3779b1 / golden-ratio constants are just splitmix64's. */
void
key_from_seed(uint32_t seed, unsigned char key[PKGL_KEY_SIZE])
{
    const uint64_t golden = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t state = ((uint64_t)seed * UINT32_C(0x9e3779b1) + 1) ^ ((uint64_t)seed << 32);
    write_u64(key, mix64(state + golden));
    write_u64(key + 8, mix64(state + golden * 2));
}

void
xor_with_key(unsigned char* data, size_t size, const unsigned char key[PKGL_KEY_SIZE])
{
    for (size_t i = 0; i < size; ++i)
        data[i] ^= key[i % PKGL_KEY_SIZE];
}

uint32_t
crc32_update(uint32_t crc, const unsigned char* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t) - (int32_t)(crc & 1));
    }
    return crc;
}

uint32_t
crc32_bytes(const unsigned char* data, size_t size)
{
    return crc32_update(UINT32_MAX, data, size) ^ UINT32_MAX;
}

uint32_t
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
