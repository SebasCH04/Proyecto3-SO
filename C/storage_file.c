#include "storage.h"

#include "common.h"
#include "protocol.h"
#include "uri.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BUCKET_MAGIC "S3BUCK01"
#define BUCKET_MAGIC_SIZE 8U
#define BUCKET_VERSION 1U
#define BUCKET_METADATA_SIZE (1024U * 1024U)
#define BUCKET_MAX_OBJECTS 1024U
#define BUCKET_MAX_FREE_EXTENTS 1024U
#define BUCKET_HEADER_SIZE 64U
#define BUCKET_OBJECT_RECORD_SIZE 544U
#define BUCKET_FREE_RECORD_SIZE 16U
#define BUCKET_OBJECT_TABLE_OFFSET BUCKET_HEADER_SIZE
#define BUCKET_FREE_TABLE_OFFSET \
    (BUCKET_OBJECT_TABLE_OFFSET + BUCKET_MAX_OBJECTS * BUCKET_OBJECT_RECORD_SIZE)
#define BUCKET_SUFFIX ".s3b"
#define BUCKET_SUFFIX_SIZE 4U

//representa un objeto registrado dentro del bloque de directorio
typedef struct {
    char key[AWS_S3_KEY_MAX + 1U];
    uint64_t offset;
    uint64_t size;
    uint64_t mtime;
} object_entry_t;

//describe una region disponible para futuras escrituras
typedef struct {
    uint64_t offset;
    uint64_t size;
} free_extent_t;

//mantiene en memoria los metadatos de un bucket abierto
typedef struct {
    int fd;
    object_entry_t *objects;
    free_extent_t *free_extents;
    uint32_t object_count;
    uint32_t free_count;
    uint64_t data_end;
} bucket_file_t;

struct storage {
    char *root;
};

static void set_error(char *error, size_t error_size, const char *message) {
    snprintf(error, error_size, "%s", message);
}

static void set_errno_error(char *error, size_t error_size,
                            const char *operation, const char *path) {
    snprintf(error, error_size, "%s %s: %s", operation, path, strerror(errno));
}

//serializa enteros sin depender del relleno de las estructuras
static void encode_u16(unsigned char *output, uint16_t value) {
    uint16_t wire = htons(value);
    memcpy(output, &wire, sizeof(wire));
}

static void encode_u32(unsigned char *output, uint32_t value) {
    uint32_t wire = htonl(value);
    memcpy(output, &wire, sizeof(wire));
}

static void encode_u64(unsigned char *output, uint64_t value) {
    uint64_t wire = host_to_be64(value);
    memcpy(output, &wire, sizeof(wire));
}

static uint16_t decode_u16(const unsigned char *input) {
    uint16_t wire;
    memcpy(&wire, input, sizeof(wire));
    return ntohs(wire);
}

static uint32_t decode_u32(const unsigned char *input) {
    uint32_t wire;
    memcpy(&wire, input, sizeof(wire));
    return ntohl(wire);
}

static uint64_t decode_u64(const unsigned char *input) {
    uint64_t wire;
    memcpy(&wire, input, sizeof(wire));
    return be64_to_host(wire);
}

static int pread_full_at(int fd, void *buffer, size_t length, uint64_t offset) {
    unsigned char *cursor = buffer;
    size_t completed = 0U;

    while (completed < length) {
        ssize_t amount = pread(fd, cursor + completed, length - completed,
                               (off_t) (offset + completed));
        if (amount == 0) {
            return -1;
        }
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        completed += (size_t) amount;
    }
    return 0;
}

static int pwrite_full_at(int fd, const void *buffer, size_t length, uint64_t offset) {
    const unsigned char *cursor = buffer;
    size_t completed = 0U;

    while (completed < length) {
        ssize_t amount = pwrite(fd, cursor + completed, length - completed,
                                (off_t) (offset + completed));
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        completed += (size_t) amount;
    }
    return 0;
}

//escribe el archivo recibido directamente en la posicion asignada
static int stream_to_file_at(int input_fd, int output_fd,
                             uint64_t offset, uint64_t size) {
    unsigned char buffer[AWS_S3_IO_CHUNK];
    uint64_t remaining = size;
    uint64_t position = offset;

    while (remaining > 0U) {
        size_t amount = sizeof(buffer);
        if (remaining < (uint64_t) amount) {
            amount = (size_t) remaining;
        }
        if (read_full(input_fd, buffer, amount) != 1 ||
            pwrite_full_at(output_fd, buffer, amount, position) != 0) {
            return -1;
        }
        remaining -= amount;
        position += amount;
    }
    return 0;
}

static void best_effort_truncate(int fd, uint64_t size) {
    int result = ftruncate(fd, (off_t) size);
    (void) result;
}

//construye la ruta fisica de un bucket validado previamente
static char *bucket_path(const storage_t *storage, const char *bucket) {
    size_t root_length = strlen(storage->root);
    size_t bucket_length = strlen(bucket);
    size_t capacity;
    char *path;
    int needs_slash = root_length > 0U && storage->root[root_length - 1U] != '/';

    if (root_length > SIZE_MAX - bucket_length - BUCKET_SUFFIX_SIZE - 2U) {
        return NULL;
    }
    capacity = root_length + (size_t) needs_slash + bucket_length +
               BUCKET_SUFFIX_SIZE + 1U;
    path = malloc(capacity);
    if (path == NULL) {
        return NULL;
    }
    snprintf(path, capacity, "%s%s%s%s", storage->root,
             needs_slash ? "/" : "", bucket, BUCKET_SUFFIX);
    return path;
}

static void bucket_file_init(bucket_file_t *bucket) {
    memset(bucket, 0, sizeof(*bucket));
    bucket->fd = -1;
    bucket->data_end = BUCKET_METADATA_SIZE;
}

static int bucket_file_allocate(bucket_file_t *bucket,
                                char *error, size_t error_size) {
    bucket->objects = calloc(BUCKET_MAX_OBJECTS, sizeof(*bucket->objects));
    bucket->free_extents = calloc(BUCKET_MAX_FREE_EXTENTS,
                                  sizeof(*bucket->free_extents));
    if (bucket->objects == NULL || bucket->free_extents == NULL) {
        set_error(error, error_size, "memoria insuficiente");
        return -1;
    }
    return 0;
}

//descarta descriptores y memoria asociados al bucket abierto
static void bucket_file_close(bucket_file_t *bucket) {
    if (bucket->fd >= 0) {
        close(bucket->fd);
    }
    free(bucket->objects);
    free(bucket->free_extents);
    bucket_file_init(bucket);
}

static int ranges_overlap(uint64_t first_offset, uint64_t first_size,
                          uint64_t second_offset, uint64_t second_size) {
    uint64_t first_end;
    uint64_t second_end;

    if (first_size == 0U || second_size == 0U) {
        return 0;
    }
    first_end = first_offset + first_size;
    second_end = second_offset + second_size;
    return first_offset < second_end && second_offset < first_end;
}

//comprueba limites, duplicados y superposiciones en los metadatos
static int validate_loaded_bucket(const bucket_file_t *bucket,
                                  uint64_t physical_size,
                                  char *error, size_t error_size) {
    uint32_t first;
    uint32_t second;

    if (bucket->data_end < BUCKET_METADATA_SIZE ||
        bucket->data_end > physical_size) {
        set_error(error, error_size, "archivo de bucket truncado o inconsistente");
        return -1;
    }
    for (first = 0U; first < bucket->object_count; ++first) {
        const object_entry_t *object = &bucket->objects[first];
        uint64_t end;

        if (object->key[0] == '\0' ||
            validate_object_key(object->key, error, error_size) != 0 ||
            object->offset < BUCKET_METADATA_SIZE ||
            object->size > UINT64_MAX - object->offset) {
            set_error(error, error_size, "registro de objeto inválido");
            return -1;
        }
        end = object->offset + object->size;
        if (end > bucket->data_end) {
            set_error(error, error_size, "objeto fuera del área de datos");
            return -1;
        }
        for (second = first + 1U; second < bucket->object_count; ++second) {
            if (strcmp(object->key, bucket->objects[second].key) == 0 ||
                ranges_overlap(object->offset, object->size,
                               bucket->objects[second].offset,
                               bucket->objects[second].size)) {
                set_error(error, error_size, "tabla de objetos corrupta");
                return -1;
            }
        }
    }
    for (first = 0U; first < bucket->free_count; ++first) {
        const free_extent_t *extent = &bucket->free_extents[first];
        uint64_t end;

        if (extent->size == 0U || extent->offset < BUCKET_METADATA_SIZE ||
            extent->size > UINT64_MAX - extent->offset) {
            set_error(error, error_size, "espacio libre inválido");
            return -1;
        }
        end = extent->offset + extent->size;
        if (end > bucket->data_end ||
            (first > 0U &&
             bucket->free_extents[first - 1U].offset +
                     bucket->free_extents[first - 1U].size >= extent->offset)) {
            set_error(error, error_size, "lista de espacios libres corrupta");
            return -1;
        }
        for (second = 0U; second < bucket->object_count; ++second) {
            if (ranges_overlap(extent->offset, extent->size,
                               bucket->objects[second].offset,
                               bucket->objects[second].size)) {
                set_error(error, error_size,
                          "un objeto ocupa un espacio marcado como libre");
                return -1;
            }
        }
    }
    return 0;
}

//reconstruye las tablas desde el primer mib del archivo
static int load_bucket_fd(int fd, bucket_file_t *bucket,
                          char *error, size_t error_size) {
    unsigned char *metadata = NULL;
    struct stat status;
    uint32_t index;
    int result = -1;

    if (fstat(fd, &status) != 0 || status.st_size < (off_t) BUCKET_METADATA_SIZE) {
        set_error(error, error_size, "archivo de bucket demasiado pequeño");
        return -1;
    }
    metadata = malloc(BUCKET_METADATA_SIZE);
    if (metadata == NULL || bucket_file_allocate(bucket, error, error_size) != 0) {
        free(metadata);
        return -1;
    }
    if (pread_full_at(fd, metadata, BUCKET_METADATA_SIZE, 0U) != 0) {
        set_error(error, error_size, "no se pudieron leer los metadatos del bucket");
        goto cleanup;
    }
    if (memcmp(metadata, BUCKET_MAGIC, BUCKET_MAGIC_SIZE) != 0 ||
        decode_u32(metadata + 8U) != BUCKET_VERSION ||
        decode_u32(metadata + 12U) != BUCKET_METADATA_SIZE ||
        decode_u32(metadata + 16U) != BUCKET_MAX_OBJECTS ||
        decode_u32(metadata + 20U) != BUCKET_MAX_FREE_EXTENTS) {
        set_error(error, error_size, "formato de bucket desconocido");
        goto cleanup;
    }
    bucket->object_count = decode_u32(metadata + 24U);
    bucket->free_count = decode_u32(metadata + 28U);
    bucket->data_end = decode_u64(metadata + 32U);
    if (bucket->object_count > BUCKET_MAX_OBJECTS ||
        bucket->free_count > BUCKET_MAX_FREE_EXTENTS) {
        set_error(error, error_size, "conteos de metadatos inválidos");
        goto cleanup;
    }
    for (index = 0U; index < bucket->object_count; ++index) {
        const unsigned char *record =
            metadata + BUCKET_OBJECT_TABLE_OFFSET +
            (size_t) index * BUCKET_OBJECT_RECORD_SIZE;
        object_entry_t *object = &bucket->objects[index];
        uint16_t key_length;

        if (record[0] != 1U) {
            set_error(error, error_size, "registro de objeto inactivo");
            goto cleanup;
        }
        key_length = decode_u16(record + 2U);
        if (key_length == 0U || key_length > AWS_S3_KEY_MAX) {
            set_error(error, error_size, "longitud de clave inválida");
            goto cleanup;
        }
        memcpy(object->key, record + 32U, key_length);
        object->key[key_length] = '\0';
        object->offset = decode_u64(record + 8U);
        object->size = decode_u64(record + 16U);
        object->mtime = decode_u64(record + 24U);
    }
    for (index = 0U; index < bucket->free_count; ++index) {
        const unsigned char *record =
            metadata + BUCKET_FREE_TABLE_OFFSET +
            (size_t) index * BUCKET_FREE_RECORD_SIZE;
        bucket->free_extents[index].offset = decode_u64(record);
        bucket->free_extents[index].size = decode_u64(record + 8U);
    }
    if (validate_loaded_bucket(bucket, (uint64_t) status.st_size,
                               error, error_size) != 0) {
        goto cleanup;
    }
    bucket->fd = fd;
    result = 0;

cleanup:
    free(metadata);
    if (result != 0) {
        free(bucket->objects);
        free(bucket->free_extents);
        bucket->objects = NULL;
        bucket->free_extents = NULL;
    }
    return result;
}

//reescribe el bloque de directorio y solicita persistencia en disco
static int save_bucket_metadata(bucket_file_t *bucket,
                                char *error, size_t error_size) {
    unsigned char *metadata = calloc(1U, BUCKET_METADATA_SIZE);
    uint32_t index;
    int result = -1;

    if (metadata == NULL) {
        set_error(error, error_size, "memoria insuficiente");
        return -1;
    }
    memcpy(metadata, BUCKET_MAGIC, BUCKET_MAGIC_SIZE);
    encode_u32(metadata + 8U, BUCKET_VERSION);
    encode_u32(metadata + 12U, BUCKET_METADATA_SIZE);
    encode_u32(metadata + 16U, BUCKET_MAX_OBJECTS);
    encode_u32(metadata + 20U, BUCKET_MAX_FREE_EXTENTS);
    encode_u32(metadata + 24U, bucket->object_count);
    encode_u32(metadata + 28U, bucket->free_count);
    encode_u64(metadata + 32U, bucket->data_end);

    for (index = 0U; index < bucket->object_count; ++index) {
        unsigned char *record =
            metadata + BUCKET_OBJECT_TABLE_OFFSET +
            (size_t) index * BUCKET_OBJECT_RECORD_SIZE;
        const object_entry_t *object = &bucket->objects[index];
        size_t key_length = strlen(object->key);

        record[0] = 1U;
        encode_u16(record + 2U, (uint16_t) key_length);
        encode_u64(record + 8U, object->offset);
        encode_u64(record + 16U, object->size);
        encode_u64(record + 24U, object->mtime);
        memcpy(record + 32U, object->key, key_length);
    }
    for (index = 0U; index < bucket->free_count; ++index) {
        unsigned char *record =
            metadata + BUCKET_FREE_TABLE_OFFSET +
            (size_t) index * BUCKET_FREE_RECORD_SIZE;
        encode_u64(record, bucket->free_extents[index].offset);
        encode_u64(record + 8U, bucket->free_extents[index].size);
    }
    if (pwrite_full_at(bucket->fd, metadata, BUCKET_METADATA_SIZE, 0U) != 0 ||
        fsync(bucket->fd) != 0) {
        set_error(error, error_size, "no se pudieron guardar los metadatos");
        goto cleanup;
    }
    result = 0;

cleanup:
    free(metadata);
    return result;
}

//abre y valida un bucket antes de permitir cualquier operacion
static int open_bucket(storage_t *storage, const char *bucket_name,
                       int writable, bucket_file_t *bucket,
                       char *error, size_t error_size) {
    char *path = bucket_path(storage, bucket_name);
    int flags = writable ? O_RDWR : O_RDONLY;
    int fd;

    if (path == NULL) {
        set_error(error, error_size, "memoria insuficiente");
        return STATUS_INTERNAL_ERROR;
    }
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags);
    if (fd < 0) {
        int saved_errno = errno;
        if (saved_errno == ENOENT) {
            set_error(error, error_size, "bucket no encontrado");
            free(path);
            return STATUS_NOT_FOUND;
        }
        set_errno_error(error, error_size, "no se pudo abrir", path);
        free(path);
        return STATUS_IO_ERROR;
    }
    free(path);
    if (load_bucket_fd(fd, bucket, error, error_size) != 0) {
        close(fd);
        return STATUS_IO_ERROR;
    }
    return STATUS_OK;
}

static int find_object_index(const bucket_file_t *bucket, const char *key) {
    uint32_t index;
    for (index = 0U; index < bucket->object_count; ++index) {
        if (strcmp(bucket->objects[index].key, key) == 0) {
            return (int) index;
        }
    }
    return -1;
}

static int key_matches_prefix(const char *key, const char *prefix) {
    size_t length = strlen(prefix);
    return length == 0U || strcmp(key, prefix) == 0 ||
           (strncmp(key, prefix, length) == 0 && key[length] == '/');
}

static int prefix_already_added(const manifest_t *manifest, const char *name) {
    return manifest_find(manifest, name) != NULL;
}

static int compare_extents(const void *left, const void *right) {
    const free_extent_t *first = left;
    const free_extent_t *second = right;
    if (first->offset < second->offset) {
        return -1;
    }
    return first->offset > second->offset ? 1 : 0;
}

//ordena, une espacios contiguos y rechaza superposiciones
static int add_free_extent(bucket_file_t *bucket, uint64_t offset, uint64_t size) {
    free_extent_t temporary[BUCKET_MAX_FREE_EXTENTS + 1U];
    uint32_t input_count;
    uint32_t index;
    uint32_t output_count = 0U;

    if (size == 0U) {
        return 0;
    }
    input_count = bucket->free_count + 1U;
    memcpy(temporary, bucket->free_extents,
           bucket->free_count * sizeof(temporary[0]));
    temporary[bucket->free_count].offset = offset;
    temporary[bucket->free_count].size = size;
    qsort(temporary, input_count, sizeof(temporary[0]), compare_extents);

    for (index = 0U; index < input_count; ++index) {
        free_extent_t *previous;
        uint64_t previous_end;

        if (output_count == 0U) {
            temporary[output_count++] = temporary[index];
            continue;
        }
        previous = &temporary[output_count - 1U];
        previous_end = previous->offset + previous->size;
        if (temporary[index].offset < previous_end) {
            return -2;
        }
        if (temporary[index].offset == previous_end) {
            if (temporary[index].size > UINT64_MAX - previous->size) {
                return -2;
            }
            previous->size += temporary[index].size;
            continue;
        }
        if (output_count >= BUCKET_MAX_FREE_EXTENTS) {
            return -1;
        }
        temporary[output_count++] = temporary[index];
    }
    memcpy(bucket->free_extents, temporary,
           output_count * sizeof(temporary[0]));
    bucket->free_count = output_count;
    return 0;
}

//reduce el final logico mientras existan bloques libres al cierre
static void trim_free_tail(bucket_file_t *bucket) {
    while (bucket->free_count > 0U) {
        free_extent_t *last = &bucket->free_extents[bucket->free_count - 1U];
        if (last->offset + last->size != bucket->data_end) {
            break;
        }
        bucket->data_end = last->offset;
        --bucket->free_count;
    }
}

static void normalize_zero_sized_objects(bucket_file_t *bucket) {
    uint32_t index;
    for (index = 0U; index < bucket->object_count; ++index) {
        if (bucket->objects[index].size == 0U) {
            bucket->objects[index].offset = bucket->data_end;
        }
    }
}

//utiliza el primer bloque libre con capacidad suficiente
static uint64_t allocate_first_fit(bucket_file_t *bucket, uint64_t size) {
    uint32_t index;

    if (size == 0U) {
        return bucket->data_end;
    }
    for (index = 0U; index < bucket->free_count; ++index) {
        free_extent_t *extent = &bucket->free_extents[index];
        uint64_t offset;

        if (extent->size < size) {
            continue;
        }
        offset = extent->offset;
        if (extent->size == size) {
            if (index + 1U < bucket->free_count) {
                memmove(&bucket->free_extents[index],
                        &bucket->free_extents[index + 1U],
                        (bucket->free_count - index - 1U) *
                            sizeof(bucket->free_extents[0]));
            }
            --bucket->free_count;
        } else {
            extent->offset += size;
            extent->size -= size;
        }
        return offset;
    }
    {
        uint64_t offset = bucket->data_end;
        bucket->data_end += size;
        return offset;
    }
}

static void remove_object_at(bucket_file_t *bucket, uint32_t index) {
    if (index + 1U < bucket->object_count) {
        memmove(&bucket->objects[index], &bucket->objects[index + 1U],
                (bucket->object_count - index - 1U) *
                    sizeof(bucket->objects[0]));
    }
    --bucket->object_count;
    memset(&bucket->objects[bucket->object_count], 0,
           sizeof(bucket->objects[0]));
}

//crea el directorio donde se almacenan los archivos de bucket
int storage_create(storage_t **storage, const char *root,
                   char *error, size_t error_size) {
    struct stat status;
    storage_t *created;

    *storage = NULL;
    if (mkdir(root, 0777) != 0 && errno != EEXIST) {
        set_errno_error(error, error_size, "no se pudo crear", root);
        return STATUS_IO_ERROR;
    }
    if (stat(root, &status) != 0 || !S_ISDIR(status.st_mode)) {
        set_error(error, error_size, "la ruta de datos no es un directorio");
        return STATUS_IO_ERROR;
    }
    created = calloc(1U, sizeof(*created));
    if (created == NULL || (created->root = strdup(root)) == NULL) {
        free(created);
        set_error(error, error_size, "memoria insuficiente");
        return STATUS_INTERNAL_ERROR;
    }
    *storage = created;
    return STATUS_OK;
}

void storage_destroy(storage_t *storage) {
    if (storage == NULL) {
        return;
    }
    free(storage->root);
    free(storage);
}

int storage_list_buckets(storage_t *storage, manifest_t *result,
                         char *error, size_t error_size) {
    DIR *directory = opendir(storage->root);
    struct dirent *item;

    if (directory == NULL) {
        set_errno_error(error, error_size, "no se pudo abrir", storage->root);
        return STATUS_IO_ERROR;
    }
    while ((item = readdir(directory)) != NULL) {
        size_t length = strlen(item->d_name);
        char bucket[AWS_S3_BUCKET_MAX + 1U];
        char *path;
        struct stat status;

        if (length <= BUCKET_SUFFIX_SIZE ||
            strcmp(item->d_name + length - BUCKET_SUFFIX_SIZE,
                   BUCKET_SUFFIX) != 0 ||
            length - BUCKET_SUFFIX_SIZE > AWS_S3_BUCKET_MAX) {
            continue;
        }
        memcpy(bucket, item->d_name, length - BUCKET_SUFFIX_SIZE);
        bucket[length - BUCKET_SUFFIX_SIZE] = '\0';
        if (validate_bucket_name(bucket, error, error_size) != 0) {
            continue;
        }
        path = bucket_path(storage, bucket);
        if (path == NULL) {
            closedir(directory);
            set_error(error, error_size, "memoria insuficiente");
            return STATUS_INTERNAL_ERROR;
        }
        if (lstat(path, &status) == 0 && S_ISREG(status.st_mode) &&
            manifest_add(result, bucket, NULL, 0U, 0U, 1) != 0) {
            free(path);
            closedir(directory);
            set_error(error, error_size, "memoria insuficiente");
            return STATUS_INTERNAL_ERROR;
        }
        free(path);
    }
    if (closedir(directory) != 0) {
        set_error(error, error_size, "no se pudo cerrar el directorio de datos");
        return STATUS_IO_ERROR;
    }
    return STATUS_OK;
}

//crea un archivo vacio con un mib reservado para metadatos
int storage_make_bucket(storage_t *storage, const char *bucket_name,
                        char *error, size_t error_size) {
    char *path = bucket_path(storage, bucket_name);
    bucket_file_t bucket;
    int flags = O_RDWR | O_CREAT | O_EXCL;
    int result = STATUS_IO_ERROR;

    bucket_file_init(&bucket);
    if (path == NULL) {
        set_error(error, error_size, "memoria insuficiente");
        return STATUS_INTERNAL_ERROR;
    }
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    bucket.fd = open(path, flags, 0666);
    if (bucket.fd < 0) {
        if (errno == EEXIST) {
            set_error(error, error_size, "el bucket ya existe");
            result = STATUS_ALREADY_EXISTS;
        } else {
            set_errno_error(error, error_size, "no se pudo crear", path);
        }
        free(path);
        return result;
    }
    if (bucket_file_allocate(&bucket, error, error_size) != 0) {
        result = STATUS_INTERNAL_ERROR;
        goto failure;
    }
    if (ftruncate(bucket.fd, (off_t) BUCKET_METADATA_SIZE) != 0 ||
        save_bucket_metadata(&bucket, error, error_size) != 0) {
        goto failure;
    }
    bucket_file_close(&bucket);
    free(path);
    return STATUS_OK;

failure:
    bucket_file_close(&bucket);
    unlink(path);
    free(path);
    return result;
}

int storage_remove_bucket(storage_t *storage, const char *bucket_name, int force,
                          char *error, size_t error_size) {
    char *path = bucket_path(storage, bucket_name);
    bucket_file_t bucket;
    int status;

    bucket_file_init(&bucket);
    if (path == NULL) {
        set_error(error, error_size, "memoria insuficiente");
        return STATUS_INTERNAL_ERROR;
    }
    status = open_bucket(storage, bucket_name, 0, &bucket, error, error_size);
    if (status != STATUS_OK) {
        free(path);
        return status;
    }
    if (bucket.object_count > 0U && !force) {
        bucket_file_close(&bucket);
        free(path);
        set_error(error, error_size, "el bucket no está vacío");
        return STATUS_NOT_EMPTY;
    }
    bucket_file_close(&bucket);
    if (unlink(path) != 0) {
        set_errno_error(error, error_size, "no se pudo eliminar", path);
        free(path);
        return STATUS_IO_ERROR;
    }
    free(path);
    return STATUS_OK;
}

//muestra objetos completos o prefijos inmediatos segun la solicitud
int storage_list_objects(storage_t *storage, const char *bucket_name,
                         const char *prefix, int recursive, manifest_t *result,
                         char *error, size_t error_size) {
    bucket_file_t bucket;
    uint32_t index;
    size_t prefix_length = strlen(prefix);
    int status;

    bucket_file_init(&bucket);
    status = open_bucket(storage, bucket_name, 0, &bucket, error, error_size);
    if (status != STATUS_OK) {
        return status;
    }
    for (index = 0U; index < bucket.object_count; ++index) {
        const object_entry_t *object = &bucket.objects[index];
        const char *relative;
        const char *slash;

        if (!key_matches_prefix(object->key, prefix)) {
            continue;
        }
        if (recursive) {
            if (manifest_add(result, object->key, NULL, object->size,
                             object->mtime, 0) != 0) {
                bucket_file_close(&bucket);
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
                bucket_file_close(&bucket);
                set_error(error, error_size, "memoria insuficiente");
                return STATUS_INTERNAL_ERROR;
            }
        } else {
            char name[AWS_S3_KEY_MAX + 2U];
            size_t base_length = (size_t) (slash - object->key);
            if (base_length + 1U >= sizeof(name)) {
                bucket_file_close(&bucket);
                set_error(error, error_size, "prefijo demasiado largo");
                return STATUS_LIMIT;
            }
            memcpy(name, object->key, base_length);
            name[base_length] = '/';
            name[base_length + 1U] = '\0';
            if (!prefix_already_added(result, name) &&
                manifest_add(result, name, NULL, 0U, 0U, 1) != 0) {
                bucket_file_close(&bucket);
                set_error(error, error_size, "memoria insuficiente");
                return STATUS_INTERNAL_ERROR;
            }
        }
    }
    bucket_file_close(&bucket);
    return STATUS_OK;
}

//sobrescribe en sitio o asigna una nueva region segun el tamaño
int storage_put_object(storage_t *storage, const char *bucket_name, const char *key,
                       uint64_t size, uint64_t mtime, int input_fd,
                       char *error, size_t error_size) {
    bucket_file_t bucket;
    int status;
    int object_index;
    object_entry_t *object;
    uint64_t offset;
    uint64_t original_data_end;
    int appended = 0;

    bucket_file_init(&bucket);
    status = open_bucket(storage, bucket_name, 1, &bucket, error, error_size);
    if (status != STATUS_OK) {
        return status;
    }
    object_index = find_object_index(&bucket, key);
    //el mismo tamaño permite conservar la posicion anterior
    if (object_index >= 0 && bucket.objects[object_index].size == size) {
        object = &bucket.objects[object_index];
        if (stream_to_file_at(input_fd, bucket.fd, object->offset, size) != 0) {
            bucket_file_close(&bucket);
            set_error(error, error_size, "no se pudo escribir el objeto");
            return STATUS_IO_ERROR;
        }
        object->mtime = mtime;
        if (save_bucket_metadata(&bucket, error, error_size) != 0) {
            bucket_file_close(&bucket);
            return STATUS_IO_ERROR;
        }
        bucket_file_close(&bucket);
        return STATUS_OK;
    }
    if (object_index < 0 && bucket.object_count >= BUCKET_MAX_OBJECTS) {
        bucket_file_close(&bucket);
        set_error(error, error_size, "el bucket alcanzó el máximo de objetos");
        return STATUS_LIMIT;
    }

    original_data_end = bucket.data_end;
    //un reemplazo de tamaño distinto siempre se agrega al final
    if (object_index >= 0) {
        object = &bucket.objects[object_index];
        if (add_free_extent(&bucket, object->offset, object->size) != 0) {
            bucket_file_close(&bucket);
            set_error(error, error_size, "la lista de espacios libres está llena");
            return STATUS_LIMIT;
        }
        if (size > (uint64_t) INT64_MAX - bucket.data_end) {
            bucket_file_close(&bucket);
            set_error(error, error_size, "objeto demasiado grande");
            return STATUS_LIMIT;
        }
        offset = bucket.data_end;
        bucket.data_end += size;
        appended = size > 0U;
    } else {
        uint64_t before_allocation = bucket.data_end;
        if (size > (uint64_t) INT64_MAX - bucket.data_end) {
            bucket_file_close(&bucket);
            set_error(error, error_size, "objeto demasiado grande");
            return STATUS_LIMIT;
        }
        offset = allocate_first_fit(&bucket, size);
        appended = bucket.data_end != before_allocation;
        object_index = (int) bucket.object_count;
        ++bucket.object_count;
    }
    object = &bucket.objects[object_index];
    memset(object, 0, sizeof(*object));
    snprintf(object->key, sizeof(object->key), "%s", key);
    object->offset = offset;
    object->size = size;
    object->mtime = mtime;

    if (stream_to_file_at(input_fd, bucket.fd, offset, size) != 0) {
        if (appended) {
            best_effort_truncate(bucket.fd, original_data_end);
        }
        bucket_file_close(&bucket);
        set_error(error, error_size, "no se pudo escribir el objeto");
        return STATUS_IO_ERROR;
    }
    if (save_bucket_metadata(&bucket, error, error_size) != 0) {
        if (appended) {
            best_effort_truncate(bucket.fd, original_data_end);
        }
        bucket_file_close(&bucket);
        return STATUS_IO_ERROR;
    }
    bucket_file_close(&bucket);
    return STATUS_OK;
}

//devuelve un descriptor y rango sin copiar el contenido a memoria
int storage_open_reader(storage_t *storage, const char *bucket_name, const char *key,
                        storage_reader_t *reader,
                        char *error, size_t error_size) {
    bucket_file_t bucket;
    int status;
    int index;

    bucket_file_init(&bucket);
    reader->fd = -1;
    status = open_bucket(storage, bucket_name, 0, &bucket, error, error_size);
    if (status != STATUS_OK) {
        return status;
    }
    index = find_object_index(&bucket, key);
    if (index < 0) {
        bucket_file_close(&bucket);
        set_error(error, error_size, "objeto no encontrado");
        return STATUS_NOT_FOUND;
    }
    reader->fd = bucket.fd;
    reader->offset = bucket.objects[index].offset;
    reader->size = bucket.objects[index].size;
    reader->mtime = bucket.objects[index].mtime;
    bucket.fd = -1;
    bucket_file_close(&bucket);
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
    int status;

    if (strcmp(source_bucket, destination_bucket) == 0 &&
        strcmp(source_key, destination_key) == 0) {
        return STATUS_OK;
    }
    status = storage_open_reader(storage, source_bucket, source_key,
                                 &reader, error, error_size);
    if (status != STATUS_OK) {
        return status;
    }
    if (lseek(reader.fd, (off_t) reader.offset, SEEK_SET) < 0) {
        storage_close_reader(&reader);
        set_error(error, error_size, "no se pudo posicionar el objeto origen");
        return STATUS_IO_ERROR;
    }
    status = storage_put_object(storage, destination_bucket, destination_key,
                                reader.size, reader.mtime, reader.fd,
                                error, error_size);
    storage_close_reader(&reader);
    return status;
}

//copia primero para conservar el origen si ocurre un error
int storage_move_object(storage_t *storage,
                        const char *source_bucket, const char *source_key,
                        const char *destination_bucket, const char *destination_key,
                        char *error, size_t error_size) {
    int status;

    if (strcmp(source_bucket, destination_bucket) == 0 &&
        strcmp(source_key, destination_key) == 0) {
        return STATUS_OK;
    }
    status = storage_copy_object(storage, source_bucket, source_key,
                                 destination_bucket, destination_key,
                                 error, error_size);
    if (status != STATUS_OK) {
        return status;
    }
    return storage_delete_object(storage, source_bucket, source_key, 0,
                                 error, error_size);
}

//libera regiones, une huecos y reduce el final del archivo
int storage_delete_object(storage_t *storage, const char *bucket_name, const char *key,
                          int recursive, char *error, size_t error_size) {
    bucket_file_t bucket;
    uint32_t index;
    uint32_t removed = 0U;
    int status;

    bucket_file_init(&bucket);
    status = open_bucket(storage, bucket_name, 1, &bucket, error, error_size);
    if (status != STATUS_OK) {
        return status;
    }
    for (index = 0U; index < bucket.object_count; ++index) {
        int matches = recursive ? key_matches_prefix(bucket.objects[index].key, key)
                                : strcmp(bucket.objects[index].key, key) == 0;
        int add_result;
        if (!matches) {
            continue;
        }
        add_result = add_free_extent(&bucket, bucket.objects[index].offset,
                                     bucket.objects[index].size);
        if (add_result != 0) {
            bucket_file_close(&bucket);
            set_error(error, error_size,
                      add_result == -1 ? "la lista de espacios libres está llena"
                                       : "se detectó corrupción en espacios libres");
            return add_result == -1 ? STATUS_LIMIT : STATUS_IO_ERROR;
        }
    }
    index = 0U;
    while (index < bucket.object_count) {
        int matches = recursive ? key_matches_prefix(bucket.objects[index].key, key)
                                : strcmp(bucket.objects[index].key, key) == 0;
        if (!matches) {
            ++index;
            continue;
        }
        remove_object_at(&bucket, index);
        ++removed;
    }
    if (removed == 0U) {
        bucket_file_close(&bucket);
        set_error(error, error_size, "objeto o prefijo no encontrado");
        return STATUS_NOT_FOUND;
    }
    trim_free_tail(&bucket);
    normalize_zero_sized_objects(&bucket);
    if (save_bucket_metadata(&bucket, error, error_size) != 0) {
        bucket_file_close(&bucket);
        return STATUS_IO_ERROR;
    }
    if (ftruncate(bucket.fd, (off_t) bucket.data_end) != 0) {
        bucket_file_close(&bucket);
        set_error(error, error_size, "no se pudo truncar el bucket");
        return STATUS_IO_ERROR;
    }
    bucket_file_close(&bucket);
    return STATUS_OK;
}
