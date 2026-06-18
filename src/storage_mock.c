#include "storage.h"

#include "common.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *key;
    int fd;
    uint64_t size;
    uint64_t mtime;
} mock_object_t;

typedef struct {
    char *name;
    mock_object_t *objects;
    size_t count;
    size_t capacity;
} mock_bucket_t;

struct storage {
    mock_bucket_t *buckets;
    size_t count;
    size_t capacity;
};

static void set_error(char *error, size_t error_size, const char *message) {
    snprintf(error, error_size, "%s", message);
}

static mock_bucket_t *find_bucket(storage_t *storage, const char *name) {
    size_t index;
    for (index = 0U; index < storage->count; ++index) {
        if (strcmp(storage->buckets[index].name, name) == 0) {
            return &storage->buckets[index];
        }
    }
    return NULL;
}

static mock_object_t *find_object(mock_bucket_t *bucket, const char *key) {
    size_t index;
    for (index = 0U; index < bucket->count; ++index) {
        if (strcmp(bucket->objects[index].key, key) == 0) {
            return &bucket->objects[index];
        }
    }
    return NULL;
}

static int key_matches_prefix(const char *key, const char *prefix) {
    size_t length = strlen(prefix);
    return length == 0U || strcmp(key, prefix) == 0 ||
           (strncmp(key, prefix, length) == 0 && key[length] == '/');
}

static void destroy_object(mock_object_t *object) {
    free(object->key);
    if (object->fd >= 0) {
        close(object->fd);
    }
}

static void destroy_bucket(mock_bucket_t *bucket) {
    size_t index;
    for (index = 0U; index < bucket->count; ++index) {
        destroy_object(&bucket->objects[index]);
    }
    free(bucket->objects);
    free(bucket->name);
}

int storage_create(storage_t **storage, const char *root,
                   char *error, size_t error_size) {
    (void) root;
    *storage = calloc(1U, sizeof(**storage));
    if (*storage == NULL) {
        set_error(error, error_size, "memoria insuficiente");
        return STATUS_INTERNAL_ERROR;
    }
    return STATUS_OK;
}

void storage_destroy(storage_t *storage) {
    size_t index;
    if (storage == NULL) {
        return;
    }
    for (index = 0U; index < storage->count; ++index) {
        destroy_bucket(&storage->buckets[index]);
    }
    free(storage->buckets);
    free(storage);
}

int storage_list_buckets(storage_t *storage, manifest_t *result,
                         char *error, size_t error_size) {
    size_t index;
    for (index = 0U; index < storage->count; ++index) {
        if (manifest_add(result, storage->buckets[index].name, NULL, 0U, 0U, 1) != 0) {
            set_error(error, error_size, "memoria insuficiente");
            return STATUS_INTERNAL_ERROR;
        }
    }
    return STATUS_OK;
}

int storage_make_bucket(storage_t *storage, const char *bucket,
                        char *error, size_t error_size) {
    mock_bucket_t *replacement;
    mock_bucket_t *created;
    size_t capacity;

    if (find_bucket(storage, bucket) != NULL) {
        set_error(error, error_size, "el bucket ya existe");
        return STATUS_ALREADY_EXISTS;
    }
    if (storage->count == storage->capacity) {
        capacity = storage->capacity == 0U ? 8U : storage->capacity * 2U;
        replacement = realloc(storage->buckets, capacity * sizeof(*replacement));
        if (replacement == NULL) {
            set_error(error, error_size, "memoria insuficiente");
            return STATUS_INTERNAL_ERROR;
        }
        storage->buckets = replacement;
        storage->capacity = capacity;
    }
    created = &storage->buckets[storage->count];
    memset(created, 0, sizeof(*created));
    created->name = strdup(bucket);
    if (created->name == NULL) {
        set_error(error, error_size, "memoria insuficiente");
        return STATUS_INTERNAL_ERROR;
    }
    ++storage->count;
    return STATUS_OK;
}

int storage_remove_bucket(storage_t *storage, const char *bucket, int force,
                          char *error, size_t error_size) {
    size_t index;
    for (index = 0U; index < storage->count; ++index) {
        if (strcmp(storage->buckets[index].name, bucket) != 0) {
            continue;
        }
        if (storage->buckets[index].count > 0U && !force) {
            set_error(error, error_size, "el bucket no está vacío");
            return STATUS_NOT_EMPTY;
        }
        destroy_bucket(&storage->buckets[index]);
        if (index + 1U < storage->count) {
            memmove(&storage->buckets[index], &storage->buckets[index + 1U],
                    (storage->count - index - 1U) * sizeof(storage->buckets[0]));
        }
        --storage->count;
        return STATUS_OK;
    }
    set_error(error, error_size, "bucket no encontrado");
    return STATUS_NOT_FOUND;
}

static int prefix_already_added(const manifest_t *manifest, const char *name) {
    return manifest_find(manifest, name) != NULL;
}

int storage_list_objects(storage_t *storage, const char *bucket_name,
                         const char *prefix, int recursive, manifest_t *result,
                         char *error, size_t error_size) {
    mock_bucket_t *bucket = find_bucket(storage, bucket_name);
    size_t index;
    size_t prefix_length = strlen(prefix);

    if (bucket == NULL) {
        set_error(error, error_size, "bucket no encontrado");
        return STATUS_NOT_FOUND;
    }
    for (index = 0U; index < bucket->count; ++index) {
        mock_object_t *object = &bucket->objects[index];
        const char *relative;
        const char *slash;

        if (!key_matches_prefix(object->key, prefix)) {
            continue;
        }
        if (recursive) {
            if (manifest_add(result, object->key, NULL, object->size,
                             object->mtime, 0) != 0) {
                set_error(error, error_size, "memoria insuficiente");
                return STATUS_INTERNAL_ERROR;
            }
            continue;
        }
        if (prefix_length == 0U) {
            relative = object->key;
        } else if (strcmp(object->key, prefix) == 0) {
            relative = object->key + prefix_length;
        } else {
            relative = object->key + prefix_length + 1U;
        }
        slash = strchr(relative, '/');
        if (slash == NULL || *relative == '\0') {
            if (manifest_add(result, object->key, NULL, object->size,
                             object->mtime, 0) != 0) {
                set_error(error, error_size, "memoria insuficiente");
                return STATUS_INTERNAL_ERROR;
            }
        } else {
            char name[513];
            size_t base_length = (size_t) (slash - object->key);
            if (base_length + 1U >= sizeof(name)) {
                set_error(error, error_size, "prefijo demasiado largo");
                return STATUS_LIMIT;
            }
            memcpy(name, object->key, base_length);
            name[base_length] = '/';
            name[base_length + 1U] = '\0';
            if (!prefix_already_added(result, name) &&
                manifest_add(result, name, NULL, 0U, 0U, 1) != 0) {
                set_error(error, error_size, "memoria insuficiente");
                return STATUS_INTERNAL_ERROR;
            }
        }
    }
    return STATUS_OK;
}

int storage_put_object(storage_t *storage, const char *bucket_name, const char *key,
                       uint64_t size, uint64_t mtime, int input_fd,
                       char *error, size_t error_size) {
    mock_bucket_t *bucket = find_bucket(storage, bucket_name);
    mock_object_t *object;
    mock_object_t *replacement;
    char template[] = "/tmp/aws-s3-mock-XXXXXX";
    int fd;
    size_t capacity;

    if (bucket == NULL) {
        set_error(error, error_size, "bucket no encontrado");
        return STATUS_NOT_FOUND;
    }
    fd = mkstemp(template);
    if (fd < 0) {
        snprintf(error, error_size, "no se pudo crear almacenamiento temporal: %s",
                 strerror(errno));
        return STATUS_IO_ERROR;
    }
    unlink(template);
    if (copy_exact(input_fd, fd, size) != 0) {
        close(fd);
        set_error(error, error_size, "falló la recepción del objeto");
        return STATUS_IO_ERROR;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        set_error(error, error_size, "falló el almacenamiento temporal");
        return STATUS_IO_ERROR;
    }
    object = find_object(bucket, key);
    if (object != NULL) {
        close(object->fd);
        object->fd = fd;
        object->size = size;
        object->mtime = mtime;
        return STATUS_OK;
    }
    if (bucket->count == bucket->capacity) {
        capacity = bucket->capacity == 0U ? 16U : bucket->capacity * 2U;
        replacement = realloc(bucket->objects, capacity * sizeof(*replacement));
        if (replacement == NULL) {
            close(fd);
            set_error(error, error_size, "memoria insuficiente");
            return STATUS_INTERNAL_ERROR;
        }
        bucket->objects = replacement;
        bucket->capacity = capacity;
    }
    object = &bucket->objects[bucket->count];
    memset(object, 0, sizeof(*object));
    object->key = strdup(key);
    if (object->key == NULL) {
        close(fd);
        set_error(error, error_size, "memoria insuficiente");
        return STATUS_INTERNAL_ERROR;
    }
    object->fd = fd;
    object->size = size;
    object->mtime = mtime;
    ++bucket->count;
    return STATUS_OK;
}

int storage_open_reader(storage_t *storage, const char *bucket_name, const char *key,
                        storage_reader_t *reader,
                        char *error, size_t error_size) {
    mock_bucket_t *bucket = find_bucket(storage, bucket_name);
    mock_object_t *object;

    if (bucket == NULL || (object = find_object(bucket, key)) == NULL) {
        set_error(error, error_size, "objeto no encontrado");
        return STATUS_NOT_FOUND;
    }
    reader->fd = dup(object->fd);
    if (reader->fd < 0) {
        set_error(error, error_size, "no se pudo abrir el objeto");
        return STATUS_IO_ERROR;
    }
    reader->offset = 0U;
    reader->size = object->size;
    reader->mtime = object->mtime;
    return STATUS_OK;
}

void storage_close_reader(storage_reader_t *reader) {
    if (reader->fd >= 0) {
        close(reader->fd);
    }
    reader->fd = -1;
}

int storage_copy_object(storage_t *storage,
                        const char *source_bucket, const char *source_key,
                        const char *destination_bucket, const char *destination_key,
                        char *error, size_t error_size) {
    storage_reader_t reader = {-1, 0U, 0U, 0U};
    int status = storage_open_reader(storage, source_bucket, source_key,
                                     &reader, error, error_size);
    if (status != STATUS_OK) {
        return status;
    }
    if (lseek(reader.fd, (off_t) reader.offset, SEEK_SET) < 0) {
        storage_close_reader(&reader);
        set_error(error, error_size, "no se pudo leer el objeto");
        return STATUS_IO_ERROR;
    }
    status = storage_put_object(storage, destination_bucket, destination_key,
                                reader.size, reader.mtime, reader.fd,
                                error, error_size);
    storage_close_reader(&reader);
    return status;
}

int storage_move_object(storage_t *storage,
                        const char *source_bucket, const char *source_key,
                        const char *destination_bucket, const char *destination_key,
                        char *error, size_t error_size) {
    if (strcmp(source_bucket, destination_bucket) == 0 &&
        strcmp(source_key, destination_key) == 0) {
        return STATUS_OK;
    }
    int status = storage_copy_object(storage, source_bucket, source_key,
                                     destination_bucket, destination_key,
                                     error, error_size);
    if (status != STATUS_OK) {
        return status;
    }
    return storage_delete_object(storage, source_bucket, source_key, 0,
                                 error, error_size);
}

int storage_delete_object(storage_t *storage, const char *bucket_name, const char *key,
                          int recursive, char *error, size_t error_size) {
    mock_bucket_t *bucket = find_bucket(storage, bucket_name);
    size_t index = 0U;
    size_t removed = 0U;

    if (bucket == NULL) {
        set_error(error, error_size, "bucket no encontrado");
        return STATUS_NOT_FOUND;
    }
    while (index < bucket->count) {
        int matches = recursive ? key_matches_prefix(bucket->objects[index].key, key)
                                : strcmp(bucket->objects[index].key, key) == 0;
        if (!matches) {
            ++index;
            continue;
        }
        destroy_object(&bucket->objects[index]);
        if (index + 1U < bucket->count) {
            memmove(&bucket->objects[index], &bucket->objects[index + 1U],
                    (bucket->count - index - 1U) * sizeof(bucket->objects[0]));
        }
        --bucket->count;
        ++removed;
    }
    if (removed == 0U) {
        set_error(error, error_size, "objeto o prefijo no encontrado");
        return STATUS_NOT_FOUND;
    }
    return STATUS_OK;
}
