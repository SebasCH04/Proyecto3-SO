#include "common.h"
#include "manifest.h"
#include "protocol.h"
#include "storage.h"
#include "uri.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int send_simple_response(int fd, uint16_t opcode) {
    protocol_header_t response = {opcode, 0U, STATUS_OK, 0U};
    return protocol_send_header(fd, &response);
}

static int send_manifest_response(int fd, uint16_t opcode, const manifest_t *manifest) {
    protocol_buffer_t payload;
    protocol_header_t response;
    size_t index;
    int result;

    protocol_buffer_init(&payload);
    if (manifest->count > UINT32_MAX ||
        protocol_buffer_put_u32(&payload, (uint32_t) manifest->count) != 0) {
        protocol_buffer_free(&payload);
        return -1;
    }
    for (index = 0U; index < manifest->count; ++index) {
        const manifest_entry_t *entry = &manifest->entries[index];
        if (protocol_buffer_put_string(&payload, entry->name) != 0 ||
            protocol_buffer_put_u64(&payload, entry->size) != 0 ||
            protocol_buffer_put_u64(&payload, entry->mtime) != 0 ||
            protocol_buffer_put_u32(&payload, entry->is_prefix ? 1U : 0U) != 0) {
            protocol_buffer_free(&payload);
            return -1;
        }
    }
    response.opcode = opcode;
    response.flags = 0U;
    response.status = STATUS_OK;
    response.payload_length = payload.length;
    result = protocol_send_header(fd, &response);
    if (result == 0) {
        result = write_full(fd, payload.data, payload.length);
    }
    protocol_buffer_free(&payload);
    return result;
}

static int parse_one_string(const unsigned char *payload, size_t length, char **value) {
    protocol_reader_t reader;
    protocol_reader_init(&reader, payload, length);
    return protocol_reader_get_string(&reader, value) == 0 &&
           protocol_reader_finished(&reader) ? 0 : -1;
}

static int validate_bucket_and_key(const char *bucket, const char *key,
                                   char *error, size_t error_size) {
    return validate_bucket_name(bucket, error, error_size) == 0 &&
           validate_object_key(key, error, error_size) == 0 ? 0 : -1;
}

static int handle_list_buckets(int fd, storage_t *storage,
                               const protocol_header_t *request) {
    manifest_t manifest;
    char error[AWS_S3_ERROR_SIZE] = "";
    int status;
    int result;

    if (request->payload_length != 0U) {
        return protocol_send_error(fd, request->opcode, STATUS_BAD_REQUEST,
                                   "LIST_BUCKETS no acepta payload");
    }
    manifest_init(&manifest);
    status = storage_list_buckets(storage, &manifest, error, sizeof(error));
    result = status == STATUS_OK
                 ? send_manifest_response(fd, request->opcode, &manifest)
                 : protocol_send_error(fd, request->opcode, (uint32_t) status, error);
    manifest_free(&manifest);
    return result;
}

static int handle_list_objects(int fd, storage_t *storage,
                               const protocol_header_t *request,
                               const unsigned char *payload, size_t payload_length) {
    protocol_reader_t reader;
    manifest_t manifest;
    char *bucket = NULL;
    char *prefix = NULL;
    char error[AWS_S3_ERROR_SIZE] = "";
    int status;
    int result;

    protocol_reader_init(&reader, payload, payload_length);
    if (protocol_reader_get_string(&reader, &bucket) != 0 ||
        protocol_reader_get_string(&reader, &prefix) != 0 ||
        !protocol_reader_finished(&reader) ||
        validate_bucket_and_key(bucket == NULL ? "" : bucket,
                                prefix == NULL ? "" : prefix,
                                error, sizeof(error)) != 0) {
        free(bucket);
        free(prefix);
        return protocol_send_error(fd, request->opcode, STATUS_BAD_REQUEST,
                                   *error == '\0' ? "payload inválido" : error);
    }
    manifest_init(&manifest);
    status = storage_list_objects(storage, bucket, prefix,
                                  (request->flags & FLAG_RECURSIVE) != 0U,
                                  &manifest, error, sizeof(error));
    result = status == STATUS_OK
                 ? send_manifest_response(fd, request->opcode, &manifest)
                 : protocol_send_error(fd, request->opcode, (uint32_t) status, error);
    manifest_free(&manifest);
    free(bucket);
    free(prefix);
    return result;
}

static int handle_bucket_command(int fd, storage_t *storage,
                                 const protocol_header_t *request,
                                 const unsigned char *payload, size_t payload_length) {
    char *bucket = NULL;
    char error[AWS_S3_ERROR_SIZE] = "";
    int status;

    if (parse_one_string(payload, payload_length, &bucket) != 0 ||
        validate_bucket_name(bucket == NULL ? "" : bucket,
                             error, sizeof(error)) != 0) {
        free(bucket);
        return protocol_send_error(fd, request->opcode, STATUS_BAD_REQUEST,
                                   *error == '\0' ? "payload inválido" : error);
    }
    if (request->opcode == OP_MAKE_BUCKET) {
        status = storage_make_bucket(storage, bucket, error, sizeof(error));
    } else {
        status = storage_remove_bucket(storage, bucket,
                                       (request->flags & FLAG_FORCE) != 0U,
                                       error, sizeof(error));
    }
    free(bucket);
    return status == STATUS_OK
               ? send_simple_response(fd, request->opcode)
               : protocol_send_error(fd, request->opcode, (uint32_t) status, error);
}

static int handle_get(int fd, storage_t *storage, const protocol_header_t *request,
                      const unsigned char *payload, size_t payload_length) {
    protocol_reader_t parser;
    char *bucket = NULL;
    char *key = NULL;
    char error[AWS_S3_ERROR_SIZE] = "";
    storage_reader_t reader = {-1, 0U, 0U, 0U};
    protocol_header_t response;
    uint64_t wire;
    int status;
    int result = -1;

    protocol_reader_init(&parser, payload, payload_length);
    if (protocol_reader_get_string(&parser, &bucket) != 0 ||
        protocol_reader_get_string(&parser, &key) != 0 ||
        !protocol_reader_finished(&parser) ||
        validate_bucket_and_key(bucket == NULL ? "" : bucket,
                                key == NULL ? "" : key,
                                error, sizeof(error)) != 0 ||
        key[0] == '\0') {
        protocol_send_error(fd, request->opcode, STATUS_BAD_REQUEST,
                            *error == '\0' ? "payload inválido" : error);
        goto cleanup;
    }
    status = storage_open_reader(storage, bucket, key, &reader, error, sizeof(error));
    if (status != STATUS_OK) {
        result = protocol_send_error(fd, request->opcode, (uint32_t) status, error);
        goto cleanup;
    }
    if (reader.size > UINT64_MAX - 16U) {
        result = protocol_send_error(fd, request->opcode, STATUS_LIMIT,
                                     "objeto demasiado grande");
        goto cleanup;
    }
    response.opcode = request->opcode;
    response.flags = 0U;
    response.status = STATUS_OK;
    response.payload_length = reader.size + 16U;
    if (protocol_send_header(fd, &response) != 0) {
        goto cleanup;
    }
    wire = host_to_be64(reader.mtime);
    if (write_full(fd, &wire, sizeof(wire)) != 0) {
        goto cleanup;
    }
    wire = host_to_be64(reader.size);
    if (write_full(fd, &wire, sizeof(wire)) != 0 ||
        copy_exact_at(reader.fd, reader.offset, fd, reader.size) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    storage_close_reader(&reader);
    free(bucket);
    free(key);
    return result;
}

static int handle_two_object_command(int fd, storage_t *storage,
                                     const protocol_header_t *request,
                                     const unsigned char *payload,
                                     size_t payload_length) {
    protocol_reader_t reader;
    char *source_bucket = NULL;
    char *source_key = NULL;
    char *destination_bucket = NULL;
    char *destination_key = NULL;
    char error[AWS_S3_ERROR_SIZE] = "";
    int status = STATUS_BAD_REQUEST;
    int valid;

    protocol_reader_init(&reader, payload, payload_length);
    valid = protocol_reader_get_string(&reader, &source_bucket) == 0 &&
            protocol_reader_get_string(&reader, &source_key) == 0 &&
            protocol_reader_get_string(&reader, &destination_bucket) == 0 &&
            protocol_reader_get_string(&reader, &destination_key) == 0 &&
            protocol_reader_finished(&reader) &&
            validate_bucket_and_key(source_bucket, source_key,
                                    error, sizeof(error)) == 0 &&
            validate_bucket_and_key(destination_bucket, destination_key,
                                    error, sizeof(error)) == 0 &&
            source_key[0] != '\0' && destination_key[0] != '\0';
    if (valid) {
        if (request->opcode == OP_COPY_OBJECT) {
            status = storage_copy_object(storage, source_bucket, source_key,
                                         destination_bucket, destination_key,
                                         error, sizeof(error));
        } else {
            status = storage_move_object(storage, source_bucket, source_key,
                                         destination_bucket, destination_key,
                                         error, sizeof(error));
        }
    } else if (*error == '\0') {
        snprintf(error, sizeof(error), "payload inválido");
    }
    free(source_bucket);
    free(source_key);
    free(destination_bucket);
    free(destination_key);
    return status == STATUS_OK
               ? send_simple_response(fd, request->opcode)
               : protocol_send_error(fd, request->opcode, (uint32_t) status, error);
}

static int handle_delete(int fd, storage_t *storage,
                         const protocol_header_t *request,
                         const unsigned char *payload, size_t payload_length) {
    protocol_reader_t reader;
    char *bucket = NULL;
    char *key = NULL;
    char error[AWS_S3_ERROR_SIZE] = "";
    int status = STATUS_BAD_REQUEST;
    int valid;

    protocol_reader_init(&reader, payload, payload_length);
    valid = protocol_reader_get_string(&reader, &bucket) == 0 &&
            protocol_reader_get_string(&reader, &key) == 0 &&
            protocol_reader_finished(&reader) &&
            validate_bucket_and_key(bucket, key, error, sizeof(error)) == 0;
    if (valid) {
        status = storage_delete_object(storage, bucket, key,
                                       (request->flags & FLAG_RECURSIVE) != 0U,
                                       error, sizeof(error));
    } else if (*error == '\0') {
        snprintf(error, sizeof(error), "payload inválido");
    }
    free(bucket);
    free(key);
    return status == STATUS_OK
               ? send_simple_response(fd, request->opcode)
               : protocol_send_error(fd, request->opcode, (uint32_t) status, error);
}

static int receive_u32(int fd, uint32_t *value) {
    uint32_t wire;
    if (read_full(fd, &wire, sizeof(wire)) != 1) {
        return -1;
    }
    *value = ntohl(wire);
    return 0;
}

static int receive_u64(int fd, uint64_t *value) {
    uint64_t wire;
    if (read_full(fd, &wire, sizeof(wire)) != 1) {
        return -1;
    }
    *value = be64_to_host(wire);
    return 0;
}

static int receive_stream_string(int fd, char **value, uint64_t *consumed) {
    uint32_t length;
    char *text;
    if (receive_u32(fd, &length) != 0 || length > AWS_S3_KEY_MAX + 1U) {
        return -1;
    }
    text = malloc((size_t) length + 1U);
    if (text == NULL || read_full(fd, text, length) != 1) {
        free(text);
        return -1;
    }
    text[length] = '\0';
    *value = text;
    *consumed += 4U + length;
    return 0;
}

static int handle_put(int fd, storage_t *storage, const protocol_header_t *request) {
    char *bucket = NULL;
    char *key = NULL;
    char error[AWS_S3_ERROR_SIZE] = "";
    uint64_t consumed = 0U;
    uint64_t mtime;
    uint64_t size;
    int status;

    if (receive_stream_string(fd, &bucket, &consumed) != 0 ||
        receive_stream_string(fd, &key, &consumed) != 0 ||
        receive_u64(fd, &mtime) != 0 || receive_u64(fd, &size) != 0) {
        free(bucket);
        free(key);
        return -1;
    }
    consumed += 16U;
    if (validate_bucket_and_key(bucket, key, error, sizeof(error)) != 0 ||
        key[0] == '\0' || size > UINT64_MAX - consumed ||
        request->payload_length != consumed + size) {
        free(bucket);
        free(key);
        protocol_send_error(fd, request->opcode, STATUS_BAD_REQUEST,
                            *error == '\0' ? "payload de subida inválido" : error);
        return -1;
    }
    status = storage_put_object(storage, bucket, key, size, mtime, fd,
                                error, sizeof(error));
    free(bucket);
    free(key);
    if (status != STATUS_OK) {
        protocol_send_error(fd, request->opcode, (uint32_t) status, error);
        return -1;
    }
    return send_simple_response(fd, request->opcode);
}

static int dispatch_request(int fd, storage_t *storage,
                            const protocol_header_t *request) {
    unsigned char *payload = NULL;
    int result;

    if (request->status != 0U) {
        return protocol_send_error(fd, request->opcode, STATUS_BAD_REQUEST,
                                   "una solicitud no puede contener estado");
    }
    if (request->opcode == OP_PUT_OBJECT) {
        return handle_put(fd, storage, request);
    }
    if (protocol_receive_control_payload(fd, request->payload_length, &payload) != 0) {
        return -1;
    }
    switch (request->opcode) {
        case OP_LIST_BUCKETS:
            result = handle_list_buckets(fd, storage, request);
            break;
        case OP_LIST_OBJECTS:
            result = handle_list_objects(fd, storage, request, payload,
                                         (size_t) request->payload_length);
            break;
        case OP_MAKE_BUCKET:
        case OP_REMOVE_BUCKET:
            result = handle_bucket_command(fd, storage, request, payload,
                                           (size_t) request->payload_length);
            break;
        case OP_GET_OBJECT:
            result = handle_get(fd, storage, request, payload,
                                (size_t) request->payload_length);
            break;
        case OP_COPY_OBJECT:
        case OP_MOVE_OBJECT:
            result = handle_two_object_command(fd, storage, request, payload,
                                               (size_t) request->payload_length);
            break;
        case OP_DELETE_OBJECT:
            result = handle_delete(fd, storage, request, payload,
                                   (size_t) request->payload_length);
            break;
        default:
            result = protocol_send_error(fd, request->opcode, STATUS_BAD_REQUEST,
                                         "operación desconocida");
            break;
    }
    free(payload);
    return result;
}

static void serve_client(int client_fd, storage_t *storage) {
    for (;;) {
        protocol_header_t request;
        int result = protocol_receive_header(client_fd, &request);
        if (result == 0) {
            return;
        }
        if (result < 0 || dispatch_request(client_fd, storage, &request) != 0) {
            return;
        }
    }
}

static int create_listener(const char *host, const char *port,
                           char *error, size_t error_size) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *current;
    int listener = -1;
    int enabled = 1;
    int code;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    code = getaddrinfo(host, port, &hints, &addresses);
    if (code != 0) {
        snprintf(error, error_size, "no se pudo resolver la dirección: %s",
                 gai_strerror(code));
        return -1;
    }
    for (current = addresses; current != NULL; current = current->ai_next) {
        listener = socket(current->ai_family, current->ai_socktype,
                          current->ai_protocol);
        if (listener < 0) {
            continue;
        }
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (bind(listener, current->ai_addr, current->ai_addrlen) == 0 &&
            listen(listener, 16) == 0) {
            break;
        }
        close(listener);
        listener = -1;
    }
    freeaddrinfo(addresses);
    if (listener < 0) {
        snprintf(error, error_size, "no se pudo escuchar en %s:%s: %s",
                 host == NULL ? "*" : host, port, strerror(errno));
    }
    return listener;
}

static void usage(FILE *stream) {
    fprintf(stream,
            "Uso: aws-s3_server [--host DIRECCION] [--port PUERTO] [--data DIRECTORIO]\n");
}

int main(int argc, char **argv) {
    const char *host = "0.0.0.0";
    const char *port = "9000";
    const char *data_root = "data";
    char error[AWS_S3_ERROR_SIZE] = "";
    storage_t *storage = NULL;
    int listener;
    int index;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--host") == 0 && index + 1 < argc) {
            host = argv[++index];
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            port = argv[++index];
        } else if (strcmp(argv[index], "--data") == 0 && index + 1 < argc) {
            data_root = argv[++index];
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    if (storage_create(&storage, data_root, error, sizeof(error)) != STATUS_OK) {
        fprintf(stderr, "aws-s3_server: %s\n", error);
        return EXIT_FAILURE;
    }
    listener = create_listener(host, port, error, sizeof(error));
    if (listener < 0) {
        fprintf(stderr, "aws-s3_server: %s\n", error);
        storage_destroy(storage);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "aws-s3_server escuchando en %s:%s (backend de prueba)\n",
            host, port);
    for (;;) {
        int client_fd = accept(listener, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "aws-s3_server: accept: %s\n", strerror(errno));
            break;
        }
        serve_client(client_fd, storage);
        close(client_fd);
    }
    close(listener);
    storage_destroy(storage);
    return EXIT_FAILURE;
}
