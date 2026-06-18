#include "manifest.h"

#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void manifest_init(manifest_t *manifest) {
    memset(manifest, 0, sizeof(*manifest));
}

void manifest_free(manifest_t *manifest) {
    size_t index;
    for (index = 0U; index < manifest->count; ++index) {
        free(manifest->entries[index].name);
        free(manifest->entries[index].path);
    }
    free(manifest->entries);
    memset(manifest, 0, sizeof(*manifest));
}

int manifest_add(manifest_t *manifest, const char *name, const char *path,
                 uint64_t size, uint64_t mtime, int is_prefix) {
    manifest_entry_t *entry;
    manifest_entry_t *replacement;
    size_t capacity;

    if (manifest->count == manifest->capacity) {
        capacity = manifest->capacity == 0U ? 16U : manifest->capacity * 2U;
        replacement = realloc(manifest->entries, capacity * sizeof(*replacement));
        if (replacement == NULL) {
            return -1;
        }
        manifest->entries = replacement;
        manifest->capacity = capacity;
    }
    entry = &manifest->entries[manifest->count];
    memset(entry, 0, sizeof(*entry));
    entry->name = strdup(name);
    entry->path = path == NULL ? NULL : strdup(path);
    if (entry->name == NULL || (path != NULL && entry->path == NULL)) {
        free(entry->name);
        free(entry->path);
        return -1;
    }
    entry->size = size;
    entry->mtime = mtime;
    entry->is_prefix = is_prefix;
    ++manifest->count;
    return 0;
}

const manifest_entry_t *manifest_find(const manifest_t *manifest, const char *name) {
    size_t index;
    for (index = 0U; index < manifest->count; ++index) {
        if (strcmp(manifest->entries[index].name, name) == 0) {
            return &manifest->entries[index];
        }
    }
    return NULL;
}

static int collect_directory(const char *root, const char *relative,
                             manifest_t *manifest, char *error, size_t error_size) {
    char directory_path[4096];
    DIR *directory;
    struct dirent *item;

    if (*relative == '\0') {
        snprintf(directory_path, sizeof(directory_path), "%s", root);
    } else if (join_path(directory_path, sizeof(directory_path), root, relative) != 0) {
        snprintf(error, error_size, "ruta local demasiado larga");
        return -1;
    }
    directory = opendir(directory_path);
    if (directory == NULL) {
        snprintf(error, error_size, "no se pudo abrir %s: %s",
                 directory_path, strerror(errno));
        return -1;
    }
    while ((item = readdir(directory)) != NULL) {
        char child_relative[4096];
        char child_path[4096];
        struct stat status;

        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (*relative == '\0') {
            snprintf(child_relative, sizeof(child_relative), "%s", item->d_name);
        } else if (join_path(child_relative, sizeof(child_relative),
                             relative, item->d_name) != 0) {
            closedir(directory);
            snprintf(error, error_size, "ruta relativa demasiado larga");
            return -1;
        }
        if (join_path(child_path, sizeof(child_path), root, child_relative) != 0 ||
            lstat(child_path, &status) != 0) {
            closedir(directory);
            snprintf(error, error_size, "no se pudo inspeccionar %s: %s",
                     child_path, strerror(errno));
            return -1;
        }
        if (S_ISLNK(status.st_mode)) {
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            if (collect_directory(root, child_relative, manifest, error, error_size) != 0) {
                closedir(directory);
                return -1;
            }
        } else if (S_ISREG(status.st_mode)) {
            if (manifest_add(manifest, child_relative, child_path,
                             (uint64_t) status.st_size, (uint64_t) status.st_mtime, 0) != 0) {
                closedir(directory);
                snprintf(error, error_size, "memoria insuficiente");
                return -1;
            }
        }
    }
    if (closedir(directory) != 0) {
        snprintf(error, error_size, "no se pudo cerrar %s", directory_path);
        return -1;
    }
    return 0;
}

int manifest_collect_local(const char *root, manifest_t *manifest,
                           char *error, size_t error_size) {
    struct stat status;

    if (lstat(root, &status) != 0) {
        snprintf(error, error_size, "no existe %s: %s", root, strerror(errno));
        return -1;
    }
    if (S_ISLNK(status.st_mode)) {
        snprintf(error, error_size, "no se siguen enlaces simbólicos: %s", root);
        return -1;
    }
    if (S_ISREG(status.st_mode)) {
        return manifest_add(manifest, path_basename_const(root), root,
                            (uint64_t) status.st_size, (uint64_t) status.st_mtime, 0);
    }
    if (!S_ISDIR(status.st_mode)) {
        snprintf(error, error_size, "%s no es un archivo ni directorio regular", root);
        return -1;
    }
    return collect_directory(root, "", manifest, error, error_size);
}
