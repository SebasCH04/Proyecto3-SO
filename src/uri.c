#include "uri.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int is_s3_uri(const char *text) {
    return text != NULL && strncmp(text, "s3://", 5U) == 0;
}

int validate_bucket_name(const char *bucket, char *error, size_t error_size) {
    size_t index;
    size_t length = strlen(bucket);

    if (length == 0U || length > AWS_S3_BUCKET_MAX) {
        snprintf(error, error_size, "el nombre del bucket debe tener entre 1 y %u caracteres",
                 (unsigned int) AWS_S3_BUCKET_MAX);
        return -1;
    }
    for (index = 0U; index < length; ++index) {
        unsigned char character = (unsigned char) bucket[index];
        if (!(isalnum(character) || character == '-' || character == '_' ||
              character == '.')) {
            snprintf(error, error_size, "carácter inválido en el bucket: '%c'", character);
            return -1;
        }
    }
    if (strcmp(bucket, ".") == 0 || strcmp(bucket, "..") == 0) {
        snprintf(error, error_size, "nombre de bucket inválido");
        return -1;
    }
    return 0;
}

int validate_object_key(const char *key, char *error, size_t error_size) {
    const char *component = key;
    const char *cursor;
    size_t length = strlen(key);

    if (length > AWS_S3_KEY_MAX) {
        snprintf(error, error_size, "la clave supera %u caracteres",
                 (unsigned int) AWS_S3_KEY_MAX);
        return -1;
    }
    if (length > 0U && key[0] == '/') {
        snprintf(error, error_size, "la clave no puede iniciar con '/'");
        return -1;
    }
    for (cursor = key;; ++cursor) {
        if (*cursor == '/' || *cursor == '\0') {
            size_t component_length = (size_t) (cursor - component);
            if ((component_length == 1U && component[0] == '.') ||
                (component_length == 2U && component[0] == '.' && component[1] == '.')) {
                snprintf(error, error_size, "la clave no puede contener componentes '.' o '..'");
                return -1;
            }
            if (*cursor == '\0') {
                break;
            }
            component = cursor + 1;
        }
    }
    return 0;
}

int parse_s3_uri(const char *text, s3_uri_t *uri, char *error, size_t error_size) {
    const char *bucket;
    const char *slash;
    size_t bucket_length;
    size_t key_length;

    memset(uri, 0, sizeof(*uri));
    if (!is_s3_uri(text)) {
        snprintf(error, error_size, "se esperaba una URI s3://");
        return -1;
    }
    bucket = text + 5U;
    slash = strchr(bucket, '/');
    bucket_length = slash == NULL ? strlen(bucket) : (size_t) (slash - bucket);
    if (bucket_length > AWS_S3_BUCKET_MAX) {
        snprintf(error, error_size, "nombre de bucket demasiado largo");
        return -1;
    }
    memcpy(uri->bucket, bucket, bucket_length);
    uri->bucket[bucket_length] = '\0';
    if (validate_bucket_name(uri->bucket, error, error_size) != 0) {
        return -1;
    }
    if (slash == NULL) {
        return 0;
    }
    key_length = strlen(slash + 1U);
    uri->key_had_trailing_slash = key_length > 0U && slash[1U + key_length - 1U] == '/';
    while (key_length > 0U && slash[1U + key_length - 1U] == '/') {
        --key_length;
    }
    if (key_length > AWS_S3_KEY_MAX) {
        snprintf(error, error_size, "clave de objeto demasiado larga");
        return -1;
    }
    memcpy(uri->key, slash + 1U, key_length);
    uri->key[key_length] = '\0';
    return validate_object_key(uri->key, error, error_size);
}

int join_object_key(char *output, size_t capacity, const char *prefix, const char *name) {
    size_t prefix_length = strlen(prefix);
    int written;

    while (prefix_length > 0U && prefix[prefix_length - 1U] == '/') {
        --prefix_length;
    }
    if (prefix_length == 0U) {
        written = snprintf(output, capacity, "%s", name);
    } else {
        written = snprintf(output, capacity, "%.*s/%s",
                           (int) prefix_length, prefix, name);
    }
    return written >= 0 && (size_t) written < capacity ? 0 : -1;
}

