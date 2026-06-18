#ifndef AWS_S3_URI_H
#define AWS_S3_URI_H

#include <stddef.h>

#define AWS_S3_BUCKET_MAX 63U
#define AWS_S3_KEY_MAX 511U

//separa una direccion s3 en bucket, clave y tipo de destino
typedef struct {
    char bucket[AWS_S3_BUCKET_MAX + 1U];
    char key[AWS_S3_KEY_MAX + 1U];
    int key_had_trailing_slash;
} s3_uri_t;

int is_s3_uri(const char *text);
int parse_s3_uri(const char *text, s3_uri_t *uri, char *error, size_t error_size);
int validate_bucket_name(const char *bucket, char *error, size_t error_size);
int validate_object_key(const char *key, char *error, size_t error_size);
int join_object_key(char *output, size_t capacity, const char *prefix, const char *name);

#endif
