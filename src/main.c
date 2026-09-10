/* thdat-nc: list / extract / create PKGL archives (TH06NC's data DAT files).
 * Same -l / -x / -c flags as THTK's thdat, since that's what our fingers know.
 * Format lives in pkgl.h; this file is just the CLI. */

#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>

#include "common.h"
#include "unpack.h"
#include "pack.h"

static void
usage(FILE* stream)
{
    fprintf(stream,
        "Usage:\n"
        "  thdat-nc -c ARCHIVE INPUT-DIRECTORY\n"
        "  thdat-nc -l ARCHIVE\n"
        "  thdat-nc -x [-C DIR] ARCHIVE [FILE...]\n"
        "  thdat-nc -h\n"
        "  -c  create a deterministic PKGL archive\n"
        "  -l  list PKGL archive entries\n"
        "  -x  extract all or selected entries\n"
        "  -C  extract below DIR (default: current directory)\n"
        "  -h  show this help\n");
}

static int
run_list(const pkgl_archive_t* archive)
{
    printf("Name\tSize\tStored\n");
    for (size_t i = 0; i < archive->count; ++i) {
        const pkgl_entry_t* entry = &archive->entries[i];
        printf("%s\t%" PRIu64 "\t%" PRIu64 "\n", entry->name, entry->size, entry->stored_size);
    }
    return 1;
}

/* No names = everything.  Keeps going past failures, reports the overall
 * result at the end. */
static int
run_extract(pkgl_archive_t* archive, const char* output_root, char** names, int name_count)
{
    int ok = 1;
    if (name_count == 0) {
        for (size_t i = 0; i < archive->count; ++i)
            ok &= extract_entry(archive, &archive->entries[i], output_root);
        return ok;
    }
    for (int i = 0; i < name_count; ++i) {
        const pkgl_entry_t* entry = archive_find(archive, names[i]);
        if (!entry)
            ok = fail("entry not found: %s", names[i]);
        else
            ok &= extract_entry(archive, entry, output_root);
    }
    return ok;
}

int
main(int argc, char** argv)
{
    int mode = 0;
    const char* output_root = ".";
    int option;
    while ((option = getopt(argc, argv, "clxC:h")) != -1) {
        switch (option) {
        case 'c':
        case 'l':
        case 'x':
            mode = option;
            break;
        case 'C':
            output_root = optarg;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    char** operands = argv + optind;
    int operand_count = argc - optind;
    if (!mode || operand_count < 1 || (mode == 'c' && operand_count != 2)) {
        usage(stderr);
        return 2;
    }

    if (mode == 'c')
        return archive_create(operands[0], operands[1]) ? 0 : 1;

    pkgl_archive_t archive;
    if (!archive_open(&archive, operands[0]))
        return 1;
    int ok = mode == 'l' ? run_list(&archive)
                         : run_extract(&archive, output_root, operands + 1, operand_count - 1);
    archive_close(&archive);
    return ok ? 0 : 1;
}
