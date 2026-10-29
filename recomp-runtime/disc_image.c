/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "disc_image.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* XDVDFS: 2048-byte sectors. Sector 32 of the game partition is the volume
   descriptor ("MICROSOFT*XBOX*MEDIA", then the root directory's sector and
   size). A directory is a binary tree of entries, each 4-byte aligned:
   left and right child offsets in 4-byte units (0: none), the first sector
   and size of the file or directory, attributes (0x10: directory), the name's
   length and the name. An entry never crosses a sector; 0xff fills the rest. */
enum {
    SECTOR = 2048u,
    DESCRIPTOR_SECTOR = 32u,
    ATTRIBUTE_DIRECTORY = 0x10u,
    MAX_DEPTH = 64u,
};

static const char kMagic[] = "MICROSOFT*XBOX*MEDIA";

/* Where the game partition can start: an extracted ISO, and full dumps of
   the three disc formats (XGD1, XGD2, XGD3). */
static const uint64_t kPartitions[] = {0u, 0x18300000u, 0xfd90000u, 0x89d80000u};

static FILE *image;
static uint64_t partition;
static uint32_t root_sector, root_size;

#ifdef _WIN32
static SRWLOCK lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)
#else
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#endif

static bool seek(FILE *file, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(file, (long long)offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static size_t read_at(uint64_t offset, void *buffer, size_t length)
{
    LOCK();
    size_t got = image != NULL && seek(image, offset) ? fread(buffer, 1, length, image) : 0u;
    UNLOCK();
    return got;
}

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

void recomp_disc_image_close(void)
{
    LOCK();
    if (image != NULL) fclose(image);
    image = NULL;
    UNLOCK();
}

bool recomp_disc_image_active(void)
{
    return image != NULL;
}

bool recomp_disc_image_open(const char *path)
{
    recomp_disc_image_close();
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "recomp disc image: can't open %s\n", path);
        return false;
    }
    for (size_t i = 0; i < sizeof kPartitions / sizeof kPartitions[0]; ++i) {
        uint8_t descriptor[28];
        if (!seek(file, kPartitions[i] + (uint64_t)DESCRIPTOR_SECTOR * SECTOR) ||
            fread(descriptor, 1, sizeof descriptor, file) != sizeof descriptor ||
            memcmp(descriptor, kMagic, 20) != 0) continue;
        LOCK();
        image = file;
        partition = kPartitions[i];
        root_sector = u32(descriptor + 20);
        root_size = u32(descriptor + 24);
        UNLOCK();
        fprintf(stderr, "recomp disc image: %s (game partition at 0x%llx)\n", path,
            (unsigned long long)partition);
        return true;
    }
    fclose(file);
    fprintf(stderr, "recomp disc image: %s is not an Xbox disc image\n", path);
    return false;
}

/* A directory's bytes, read whole (directories are small). */
static uint8_t *read_directory(uint32_t sector, uint32_t size)
{
    if (size == 0u || size > 64u * 1024u * 1024u) return NULL;
    uint8_t *bytes = malloc(size);
    if (bytes == NULL) return NULL;
    if (read_at(partition + (uint64_t)sector * SECTOR, bytes, size) != size) {
        free(bytes);
        return NULL;
    }
    return bytes;
}

static void entry_of(const uint8_t *at, RecompDiscImageEntry *entry)
{
    entry->directory = (at[12] & ATTRIBUTE_DIRECTORY) != 0u;
    entry->offset = partition + (uint64_t)u32(at + 4) * SECTOR;
    entry->size = u32(at + 8);
}

/* The entry at `offset` (bytes) of a directory, if it is a whole one. */
static const uint8_t *entry_at(const uint8_t *bytes, uint32_t size, uint32_t offset)
{
    if ((uint64_t)offset + 14u > size) return NULL;
    const uint8_t *at = bytes + offset;
    if (u16(at) == 0xffffu && u16(at + 2) == 0xffffu) return NULL;
    if ((uint64_t)offset + 14u + at[13] > size || at[13] == 0u) return NULL;
    return at;
}

static int compare_name(const char *a, size_t a_length, const uint8_t *b, size_t b_length)
{
    const size_t n = a_length < b_length ? a_length : b_length;
    for (size_t i = 0; i < n; ++i) {
        const int x = toupper((unsigned char)a[i]), y = toupper(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return a_length == b_length ? 0 : a_length < b_length ? -1 : 1;
}

/* The tree is sorted by name without regard to case: walk it. */
static bool find_in(const uint8_t *bytes, uint32_t size, const char *name, size_t length, RecompDiscImageEntry *entry)
{
    uint32_t offset = 0u;
    for (unsigned depth = 0u; depth < 4096u; ++depth) {
        const uint8_t *at = entry_at(bytes, size, offset);
        if (at == NULL) return false;
        const int order = compare_name(name, length, at + 14, at[13]);
        if (order == 0) {
            entry_of(at, entry);
            return true;
        }
        const uint16_t next = order < 0 ? u16(at) : u16(at + 2);
        if (next == 0u) return false;
        offset = (uint32_t)next * 4u;
    }
    return false;
}

bool recomp_disc_image_lookup(const char *path, RecompDiscImageEntry *entry)
{
    if (image == NULL) return false;
    RecompDiscImageEntry current = {true, partition + (uint64_t)root_sector * SECTOR, root_size};
    const char *at = path;
    while (*at == '\\' || *at == '/') ++at;
    while (*at != '\0') {
        const char *end = at;
        while (*end != '\0' && *end != '\\' && *end != '/') ++end;
        if (!current.directory) return false;
        const uint32_t sector = (uint32_t)((current.offset - partition) / SECTOR);
        uint8_t *bytes = read_directory(sector, (uint32_t)current.size);
        if (bytes == NULL) return false;
        RecompDiscImageEntry next;
        const bool found = find_in(bytes, (uint32_t)current.size, at, (size_t)(end - at), &next);
        free(bytes);
        if (!found) return false;
        current = next;
        at = end;
        while (*at == '\\' || *at == '/') ++at;
    }
    *entry = current;
    return true;
}

static void visit(const uint8_t *bytes, uint32_t size, uint32_t offset, unsigned depth,
    RecompDiscImageVisit each, void *context)
{
    const uint8_t *at = entry_at(bytes, size, offset);
    if (at == NULL || depth > MAX_DEPTH * 64u) return;
    if (u16(at) != 0u) visit(bytes, size, (uint32_t)u16(at) * 4u, depth + 1u, each, context);
    char name[256];
    memcpy(name, at + 14, at[13]);
    name[at[13]] = '\0';
    RecompDiscImageEntry entry;
    entry_of(at, &entry);
    each(context, name, &entry);
    if (u16(at + 2) != 0u) visit(bytes, size, (uint32_t)u16(at + 2) * 4u, depth + 1u, each, context);
}

bool recomp_disc_image_list(const char *path, RecompDiscImageVisit each, void *context)
{
    RecompDiscImageEntry directory;
    if (!recomp_disc_image_lookup(path, &directory) || !directory.directory) return false;
    if (directory.size == 0u) return true;
    uint8_t *bytes = read_directory((uint32_t)((directory.offset - partition) / SECTOR), (uint32_t)directory.size);
    if (bytes == NULL) return false;
    visit(bytes, (uint32_t)directory.size, 0u, 0u, each, context);
    free(bytes);
    return true;
}

size_t recomp_disc_image_read(uint64_t offset, void *buffer, size_t length)
{
    return length == 0u ? 0u : read_at(offset, buffer, length);
}
