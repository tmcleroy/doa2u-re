/* SPDX-License-Identifier: GPL-3.0-or-later */
/* disc_image.c on a small image made here: a full-dump layout (the game
   partition at 0x18300000 is too large to write), so an extracted one: the
   descriptor at sector 32, a root directory with a file and a directory as a
   two-entry tree, and that directory with one file. */
#include "disc_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static void put32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }

/* One entry at `at`: children (in 4-byte units), sector, size, attributes, name. */
static unsigned entry(unsigned char *at, unsigned left, unsigned right, unsigned sector, unsigned size,
    unsigned attributes, const char *name)
{
    put16(at, left);
    put16(at + 2, right);
    put32(at + 4, sector);
    put32(at + 8, size);
    at[12] = (unsigned char)attributes;
    at[13] = (unsigned char)strlen(name);
    memcpy(at + 14, name, strlen(name));
    return (14u + (unsigned)strlen(name) + 3u) & ~3u;
}

static int names;
static void count(void *context, const char *name, const RecompDiscImageEntry *found)
{
    (void)found;
    char *order = context;
    strcat(order, name);
    strcat(order, ",");
    ++names;
}

int main(void)
{
    enum { SECTOR = 2048, SECTORS = 40 };
    static unsigned char image[SECTOR * SECTORS];
    memset(image, 0xff, sizeof image);
    unsigned char *descriptor = image + 32 * SECTOR;
    memcpy(descriptor, "MICROSOFT*XBOX*MEDIA", 20);
    put32(descriptor + 20, 33);
    put32(descriptor + 24, SECTOR);
    /* Root (sector 33): "Media" (directory) with "default.xbe" to its left,
       as the tree sorts names without regard to case. */
    unsigned char *root = image + 33 * SECTOR;
    const unsigned first = entry(root, 0, 0, 34, SECTOR, 0x10, "Media");
    put16(root, first / 4u);
    entry(root + first, 0, 0, 35, 5, 0x20, "default.xbe");
    entry(image + 34 * SECTOR, 0, 0, 36, 3, 0x20, "tune.wma");
    memcpy(image + 35 * SECTOR, "XBEH!", 5);
    memcpy(image + 36 * SECTOR, "abc", 3);

    char path[] = "disc_image_test.iso";
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    fwrite(image, 1, sizeof image, file);
    fclose(file);

    CHECK(recomp_disc_image_open(path));
    RecompDiscImageEntry found = {0};
    CHECK(recomp_disc_image_lookup("", &found) && found.directory);
    CHECK(recomp_disc_image_lookup("\\DEFAULT.XBE", &found) && !found.directory && found.size == 5u);
    char bytes[8] = {0};
    CHECK(recomp_disc_image_read(found.offset, bytes, 5u) == 5u && memcmp(bytes, "XBEH!", 5) == 0);
    CHECK(recomp_disc_image_lookup("media/Tune.wma", &found) && found.size == 3u);
    CHECK(!recomp_disc_image_lookup("media/none", &found));
    CHECK(!recomp_disc_image_lookup("default.xbe/x", &found));
    char order[64] = {0};
    CHECK(recomp_disc_image_list("", count, order) && names == 2 && strcmp(order, "default.xbe,Media,") == 0);
    recomp_disc_image_close();
    CHECK(!recomp_disc_image_active());
    remove(path);

    image[32 * SECTOR] = 'X';
    file = fopen(path, "wb");
    fwrite(image, 1, sizeof image, file);
    fclose(file);
    CHECK(!recomp_disc_image_open(path));
    remove(path);

    if (failures == 0) printf("disc image: ok\n");
    return failures != 0;
}
