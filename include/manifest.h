#ifndef AWS_S3_MANIFEST_H
#define AWS_S3_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    char *path;
    uint64_t size;
    uint64_t mtime;
    int is_prefix;
} manifest_entry_t;

typedef struct {
    manifest_entry_t *entries;
    size_t count;
    size_t capacity;
} manifest_t;

void manifest_init(manifest_t *manifest);
void manifest_free(manifest_t *manifest);
int manifest_add(manifest_t *manifest, const char *name, const char *path,
                 uint64_t size, uint64_t mtime, int is_prefix);
const manifest_entry_t *manifest_find(const manifest_t *manifest, const char *name);
int manifest_collect_local(const char *root, manifest_t *manifest,
                           char *error, size_t error_size);

#endif

