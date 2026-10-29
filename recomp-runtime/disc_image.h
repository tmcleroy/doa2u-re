/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECOMP_DISC_IMAGE_H
#define RECOMP_DISC_IMAGE_H

/* An Xbox disc image read in place: the game partition's XDVDFS file system,
   in an extracted-ISO ("xiso", the partition alone) or a full disc dump (the
   video partition first). The image is only ever read. When one is open the
   kernel serves the D: drive from it instead of a folder. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompDiscImageEntry {
    bool directory;
    uint64_t offset;   /* bytes from the start of the image file */
    uint64_t size;
} RecompDiscImageEntry;

/* Opens and indexes the image; false (with a reason on stderr) if it is not
   an Xbox disc image. Another call replaces the open image. */
bool recomp_disc_image_open(const char *path);
void recomp_disc_image_close(void);
bool recomp_disc_image_active(void);

/* A path inside the game partition, with '\\' or '/' between names, matched
   without regard to case; "" is the root directory. */
bool recomp_disc_image_lookup(const char *path, RecompDiscImageEntry *entry);

/* Calls `each` for every entry of a directory, in the image's order (a
   sorted tree); false if the path is not a directory. */
typedef void (*RecompDiscImageVisit)(void *context, const char *name, const RecompDiscImageEntry *entry);
bool recomp_disc_image_list(const char *path, RecompDiscImageVisit each, void *context);

/* Reads up to `length` bytes at `offset` of the image file; the count read. */
size_t recomp_disc_image_read(uint64_t offset, void *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
