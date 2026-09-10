/*
 * The PKGL container, as far as I've reversed it from TH06NC.  Pure format
 * knowledge: no I/O, no zstd, nothing platform-specific lives here.
 *
 * Layout (everything little-endian):
 *
 *   offset  size  field
 *   0       4     "PKGL"
 *   4       4     directory_size
 *   8       n     directory, XOR'd with key(crc32(archive basename, no extension))
 *   ...           payloads, each XOR'd with key(record.seed); a zstd frame if
 *                 the record's flag bit 0 is set
 *
 * One directory record = 32 fixed bytes + the name, no terminator:
 *
 *   0    u16  flags        bit 0: payload is zstd
 *   2    u32  seed         payload XOR seed.  The game only uses it as a key,
 *                          never checks it against anything
 *   6    u64  size         uncompressed
 *   14   u64  stored_size  on disk
 *   22   u64  offset       absolute, from start of file
 *   30   u16  name_size    1..255
 *   32   name              '/'-separated relative path
 *
 * The 16-byte XOR key is two splitmix64 outputs, state seeded from the 32-bit
 * seed -- see key_from_seed().
 *
 * Things our packer decides on its own, because the official tool's rules are
 * unknown (README has the seed story):
 *   - entry seed = the constant "poo" (PKGL_PLACEHOLDER_SEED), see below
 *   - entries sorted by name, payloads 16-byte aligned with zero padding
 *   - one trailing zero byte after the last payload
 */
#ifndef THDAT_NC_PKGL_H
#define THDAT_NC_PKGL_H

#include <stddef.h>
#include <stdint.h>

#define PKGL_MAGIC "PKGL"
#define PKGL_HEADER_SIZE 8
#define PKGL_RECORD_SIZE 32
#define PKGL_NAME_MAX 255
#define PKGL_FLAG_ZSTD UINT16_C(0x0001)
#define PKGL_PAYLOAD_ALIGN UINT64_C(16)
#define PKGL_KEY_SIZE 16

/* Our per-entry seed for packed archives.  Honest placeholder: I don't know
 * how the official packer picks seeds (retail ones are all distinct and match
 * no hash I tried), and the loader only uses the field as an XOR key without
 * ever checking it, so any constant works.  This one spells "poo" in a hex
 * editor, so a repacked dat is easy to spot.  If you ever find the real
 * generator, replace this and nothing else. */
#define PKGL_PLACEHOLDER_SEED UINT32_C(0x006f6f70) /* "poo\0", little-endian */

/* Byte offsets inside the fixed part of a record. */
enum {
    RECORD_FLAGS = 0,
    RECORD_SEED = 2,
    RECORD_SIZE = 6,
    RECORD_STORED_SIZE = 14,
    RECORD_OFFSET = 22,
    RECORD_NAME_SIZE = 30,
};

typedef struct {
    uint16_t flags;
    uint32_t seed;
    uint64_t size;
    uint64_t stored_size;
    uint64_t offset;
    char* name;
} pkgl_entry_t;

/* Fixed part only.  record_unpack leaves entry->name NULL; the caller reads
 * name_size (RECORD_NAME_SIZE) and copies the name that follows. */
void record_unpack(const unsigned char* record, pkgl_entry_t* entry);
/* Writes the fixed part and the name; record must have room for both. */
void record_pack(unsigned char* record, const pkgl_entry_t* entry);

/* 32-bit seed -> the 16-byte XOR key the game's loader derives. */
void key_from_seed(uint32_t seed, unsigned char key[PKGL_KEY_SIZE]);
/* Encrypt and decrypt are the same operation, XOR being XOR. */
void xor_with_key(unsigned char* data, size_t size, const unsigned char key[PKGL_KEY_SIZE]);

/* Plain zlib-compatible CRC-32.  Start from UINT32_MAX, flip at the end --
 * crc32_bytes() does both for you. */
uint32_t crc32_update(uint32_t crc, const unsigned char* data, size_t size);
uint32_t crc32_bytes(const unsigned char* data, size_t size);

/* Directory key seed = crc32 of the basename sans extension ("th06ST" for
 * ".../th06ST.dat").  This is why renaming a dat breaks it. */
uint32_t archive_seed(const char* path);

#endif
