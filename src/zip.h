#ifndef ZIP_H
#define ZIP_H
#include <stddef.h>

typedef struct {
    char name[256];
    unsigned method;
    unsigned long csize, usize, offset;
} zip_entry;

typedef struct {
    unsigned char *data;
    long size;
    zip_entry *entries;
    int count;
} zip_archive;

zip_archive *zip_open_file(const char *path);
// finds an entry by file name (any folder, case-insensitive)
const zip_entry *zip_find(const zip_archive *z, const char *name);
unsigned char *zip_extract(const zip_archive *z, const zip_entry *e, size_t *out_len);
void zip_close(zip_archive *z);

#endif
