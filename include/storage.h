#ifndef AWS_S3_STORAGE_H
#define AWS_S3_STORAGE_H

#include "manifest.h"

#include <stddef.h>
#include <stdint.h>

typedef struct storage storage_t;

typedef struct {
    int fd;
    uint64_t offset;
    uint64_t size;
    uint64_t mtime;
} storage_reader_t;

/*
 * Contrato que debe implementar el motor persistente. Todas las funciones
 * devuelven un valor de enum protocol_status.
 */
int storage_create(storage_t **storage, const char *root,
                   char *error, size_t error_size);
void storage_destroy(storage_t *storage);
int storage_list_buckets(storage_t *storage, manifest_t *result,
                         char *error, size_t error_size);
int storage_make_bucket(storage_t *storage, const char *bucket,
                        char *error, size_t error_size);
int storage_remove_bucket(storage_t *storage, const char *bucket, int force,
                          char *error, size_t error_size);
int storage_list_objects(storage_t *storage, const char *bucket,
                         const char *prefix, int recursive, manifest_t *result,
                         char *error, size_t error_size);
int storage_put_object(storage_t *storage, const char *bucket, const char *key,
                       uint64_t size, uint64_t mtime, int input_fd,
                       char *error, size_t error_size);
int storage_open_reader(storage_t *storage, const char *bucket, const char *key,
                        storage_reader_t *reader,
                        char *error, size_t error_size);
void storage_close_reader(storage_reader_t *reader);
int storage_copy_object(storage_t *storage,
                        const char *source_bucket, const char *source_key,
                        const char *destination_bucket, const char *destination_key,
                        char *error, size_t error_size);
int storage_move_object(storage_t *storage,
                        const char *source_bucket, const char *source_key,
                        const char *destination_bucket, const char *destination_key,
                        char *error, size_t error_size);
int storage_delete_object(storage_t *storage, const char *bucket, const char *key,
                          int recursive, char *error, size_t error_size);

#endif
