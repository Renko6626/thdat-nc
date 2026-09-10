/* Pack side: walk a directory, build a PKGL archive out of it. */
#ifndef THDAT_NC_PACK_H
#define THDAT_NC_PACK_H

/* Prints each entry name as it goes, like thdat -c.  Deterministic: same
 * input tree + same output basename = same bytes. */
int archive_create(const char* output_path, const char* input_root);

#endif
