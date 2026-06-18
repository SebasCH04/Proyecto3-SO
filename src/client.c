#include "common.h"
#include "manifest.h"
#include "protocol.h"
#include "uri.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
} client_t;

typedef struct {
    int recursive;
    int delete_extra;
    int force;
} command_options_t;

static int send_request(client_t *client, uint16_t opcode, uint32_t flags,
                        const protocol_buffer_t *payload) {
    protocol_header_t header;
    header.opcode = opcode;
    header.flags = flags;
    header.status = 0U;
    header.payload_length = payload == NULL ? 0U : payload->length;
    if (protocol_send_header(client->fd, &header) != 0) {
        return -1;
    }
    if (payload != NULL && payload->length > 0U &&
        write_full(client->fd, payload->data, payload->length) != 0) {
        return -1;
    }
    return 0;
}

static int receive_response_header(client_t *client, uint16_t opcode,
                                   protocol_header_t *response) {
    unsigned char *payload = NULL;
    protocol_reader_t reader;
    char *message = NULL;
    int received = protocol_receive_header(client->fd, response);

    if (received != 1 || response->opcode != opcode) {
        fprintf(stderr, "aws-s3: respuesta inválida o conexión interrumpida\n");
        return -1;
    }
    if (response->status == STATUS_OK) {
        return 0;
    }
    if (protocol_receive_control_payload(client->fd, response->payload_length,
                                         &payload) == 0) {
        protocol_reader_init(&reader, payload, (size_t) response->payload_length);
        if (protocol_reader_get_string(&reader, &message) != 0) {
            free(message);
            message = NULL;
        }
    }
    fprintf(stderr, "aws-s3: %s\n",
            message == NULL || *message == '\0' ? "el servidor rechazó la operación"
                                                : message);
    free(message);
    free(payload);
    return -1;
}

static int receive_empty_response(client_t *client, uint16_t opcode) {
    protocol_header_t response;
    if (receive_response_header(client, opcode, &response) != 0) {
        return -1;
    }
    if (response.payload_length != 0U) {
        fprintf(stderr, "aws-s3: respuesta inesperada del servidor\n");
        return -1;
    }
    return 0;
}

static int request_one_string(client_t *client, uint16_t opcode, uint32_t flags,
                              const char *value) {
    protocol_buffer_t payload;
    int result;
    protocol_buffer_init(&payload);
    result = protocol_buffer_put_string(&payload, value);
    if (result == 0) {
        result = send_request(client, opcode, flags, &payload);
    }
    protocol_buffer_free(&payload);
    return result == 0 ? receive_empty_response(client, opcode) : -1;
}

static int request_object_pair(client_t *client, uint16_t opcode, uint32_t flags,
                               const char *bucket, const char *key) {
    protocol_buffer_t payload;
    int result;
    protocol_buffer_init(&payload);
    result = protocol_buffer_put_string(&payload, bucket);
    if (result == 0) {
        result = protocol_buffer_put_string(&payload, key);
    }
    if (result == 0) {
        result = send_request(client, opcode, flags, &payload);
    }
    protocol_buffer_free(&payload);
    return result == 0 ? receive_empty_response(client, opcode) : -1;
}

static int remote_list(client_t *client, const char *bucket, const char *prefix,
                       int recursive, manifest_t *manifest) {
    protocol_buffer_t payload;
    protocol_header_t response;
    protocol_reader_t reader;
    unsigned char *wire = NULL;
    uint32_t count;
    uint32_t index;
    int result = -1;

    protocol_buffer_init(&payload);
    if (*bucket == '\0') {
        if (send_request(client, OP_LIST_BUCKETS, 0U, NULL) != 0 ||
            receive_response_header(client, OP_LIST_BUCKETS, &response) != 0) {
            goto cleanup;
        }
    } else {
        if (protocol_buffer_put_string(&payload, bucket) != 0 ||
            protocol_buffer_put_string(&payload, prefix) != 0 ||
            send_request(client, OP_LIST_OBJECTS,
                         recursive ? FLAG_RECURSIVE : 0U, &payload) != 0 ||
            receive_response_header(client, OP_LIST_OBJECTS, &response) != 0) {
            goto cleanup;
        }
    }
    if (protocol_receive_control_payload(client->fd, response.payload_length, &wire) != 0) {
        fprintf(stderr, "aws-s3: manifiesto remoto inválido\n");
        goto cleanup;
    }
    protocol_reader_init(&reader, wire, (size_t) response.payload_length);
    if (protocol_reader_get_u32(&reader, &count) != 0) {
        goto cleanup;
    }
    for (index = 0U; index < count; ++index) {
        char *name = NULL;
        uint64_t size;
        uint64_t mtime;
        uint32_t is_prefix;
        if (protocol_reader_get_string(&reader, &name) != 0 ||
            protocol_reader_get_u64(&reader, &size) != 0 ||
            protocol_reader_get_u64(&reader, &mtime) != 0 ||
            protocol_reader_get_u32(&reader, &is_prefix) != 0 ||
            manifest_add(manifest, name, NULL, size, mtime, is_prefix != 0U) != 0) {
            free(name);
            goto cleanup;
        }
        free(name);
    }
    if (!protocol_reader_finished(&reader)) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (result != 0 && wire != NULL) {
        fprintf(stderr, "aws-s3: no se pudo interpretar el manifiesto remoto\n");
    }
    free(wire);
    protocol_buffer_free(&payload);
    return result;
}

static int remote_put(client_t *client, const char *local_path,
                      const char *bucket, const char *key) {
    struct stat status;
    protocol_buffer_t metadata;
    protocol_header_t request;
    int input_fd = -1;
    int result = -1;

    if (lstat(local_path, &status) != 0 || !S_ISREG(status.st_mode) ||
        S_ISLNK(status.st_mode)) {
        fprintf(stderr, "aws-s3: no es un archivo regular: %s\n", local_path);
        return -1;
    }
    input_fd = open(local_path, O_RDONLY);
    if (input_fd < 0) {
        fprintf(stderr, "aws-s3: no se pudo abrir %s: %s\n",
                local_path, strerror(errno));
        return -1;
    }
    protocol_buffer_init(&metadata);
    if (protocol_buffer_put_string(&metadata, bucket) != 0 ||
        protocol_buffer_put_string(&metadata, key) != 0 ||
        protocol_buffer_put_u64(&metadata, (uint64_t) status.st_mtime) != 0 ||
        protocol_buffer_put_u64(&metadata, (uint64_t) status.st_size) != 0 ||
        (uint64_t) status.st_size > UINT64_MAX - metadata.length) {
        fprintf(stderr, "aws-s3: no se pudo construir la solicitud\n");
        goto cleanup;
    }
    request.opcode = OP_PUT_OBJECT;
    request.flags = 0U;
    request.status = 0U;
    request.payload_length = metadata.length + (uint64_t) status.st_size;
    if (protocol_send_header(client->fd, &request) != 0 ||
        write_full(client->fd, metadata.data, metadata.length) != 0 ||
        copy_exact(input_fd, client->fd, (uint64_t) status.st_size) != 0) {
        fprintf(stderr, "aws-s3: falló la transferencia de %s\n", local_path);
        goto cleanup;
    }
    result = receive_empty_response(client, OP_PUT_OBJECT);

cleanup:
    protocol_buffer_free(&metadata);
    close(input_fd);
    return result;
}

static int set_file_mtime(const char *path, uint64_t mtime) {
    struct timespec times[2];
    times[0].tv_sec = (time_t) mtime;
    times[0].tv_nsec = 0;
    times[1] = times[0];
    return utimensat(AT_FDCWD, path, times, 0);
}

static int remote_get(client_t *client, const char *bucket, const char *key,
                      const char *local_path) {
    protocol_buffer_t payload;
    protocol_header_t response;
    uint64_t wire_mtime;
    uint64_t wire_size;
    uint64_t mtime;
    uint64_t size;
    int output_fd = -1;
    int result = -1;

    protocol_buffer_init(&payload);
    if (protocol_buffer_put_string(&payload, bucket) != 0 ||
        protocol_buffer_put_string(&payload, key) != 0 ||
        send_request(client, OP_GET_OBJECT, 0U, &payload) != 0 ||
        receive_response_header(client, OP_GET_OBJECT, &response) != 0) {
        goto cleanup;
    }
    if (response.payload_length < 16U ||
        read_full(client->fd, &wire_mtime, sizeof(wire_mtime)) != 1 ||
        read_full(client->fd, &wire_size, sizeof(wire_size)) != 1) {
        fprintf(stderr, "aws-s3: respuesta de descarga incompleta\n");
        goto cleanup;
    }
    mtime = be64_to_host(wire_mtime);
    size = be64_to_host(wire_size);
    if (response.payload_length != size + 16U) {
        fprintf(stderr, "aws-s3: tamaño de descarga inválido\n");
        goto cleanup;
    }
    if (ensure_parent_directories(local_path) != 0) {
        fprintf(stderr, "aws-s3: no se pudo crear el directorio de %s\n", local_path);
        goto cleanup;
    }
    output_fd = open(local_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output_fd < 0 || copy_exact(client->fd, output_fd, size) != 0) {
        fprintf(stderr, "aws-s3: no se pudo guardar %s: %s\n",
                local_path, strerror(errno));
        if (output_fd >= 0) {
            close(output_fd);
            output_fd = -1;
        }
        unlink(local_path);
        goto cleanup;
    }
    if (close(output_fd) != 0) {
        output_fd = -1;
        goto cleanup;
    }
    output_fd = -1;
    if (set_file_mtime(local_path, mtime) != 0) {
        fprintf(stderr, "aws-s3: aviso: no se pudo conservar la fecha de %s\n",
                local_path);
    }
    result = 0;

cleanup:
    if (output_fd >= 0) {
        close(output_fd);
    }
    protocol_buffer_free(&payload);
    return result;
}

static int remote_copy_or_move(client_t *client, int move,
                               const char *source_bucket, const char *source_key,
                               const char *destination_bucket,
                               const char *destination_key) {
    protocol_buffer_t payload;
    uint16_t opcode = move ? OP_MOVE_OBJECT : OP_COPY_OBJECT;
    int result;
    protocol_buffer_init(&payload);
    result = protocol_buffer_put_string(&payload, source_bucket);
    if (result == 0) {
        result = protocol_buffer_put_string(&payload, source_key);
    }
    if (result == 0) {
        result = protocol_buffer_put_string(&payload, destination_bucket);
    }
    if (result == 0) {
        result = protocol_buffer_put_string(&payload, destination_key);
    }
    if (result == 0) {
        result = send_request(client, opcode, 0U, &payload);
    }
    protocol_buffer_free(&payload);
    return result == 0 ? receive_empty_response(client, opcode) : -1;
}

static int remote_delete(client_t *client, const char *bucket, const char *key,
                         int recursive) {
    return request_object_pair(client, OP_DELETE_OBJECT,
                               recursive ? FLAG_RECURSIVE : 0U, bucket, key);
}

static int ensure_directory(const char *path) {
    char marker[4096];
    struct stat status;
    int written;
    if (stat(path, &status) == 0) {
        return S_ISDIR(status.st_mode) ? 0 : -1;
    }
    written = snprintf(marker, sizeof(marker), "%s/.aws-s3-dir", path);
    if (written < 0 || (size_t) written >= sizeof(marker) ||
        ensure_parent_directories(marker) != 0) {
        return -1;
    }
    return 0;
}

static int destination_local_path(char *output, size_t capacity,
                                  const char *destination, const char *name) {
    struct stat status;
    size_t length = strlen(destination);
    int as_directory = (stat(destination, &status) == 0 && S_ISDIR(status.st_mode)) ||
                       (length > 0U && destination[length - 1U] == '/');
    if (as_directory) {
        return join_path(output, capacity, destination, name);
    }
    return snprintf(output, capacity, "%s", destination) >= 0 &&
           strlen(destination) < capacity ? 0 : -1;
}

static int destination_key(char *output, size_t capacity, const s3_uri_t *destination,
                           const char *source_name) {
    if (destination->key[0] == '\0' || destination->key_had_trailing_slash) {
        return join_object_key(output, capacity, destination->key, source_name);
    }
    return snprintf(output, capacity, "%s", destination->key) >= 0 &&
           strlen(destination->key) < capacity ? 0 : -1;
}

static const char *relative_remote_name(const char *key, const char *prefix) {
    size_t length = strlen(prefix);
    if (length == 0U) {
        return key;
    }
    if (strcmp(key, prefix) == 0) {
        return path_basename_const(key);
    }
    if (strncmp(key, prefix, length) == 0 && key[length] == '/') {
        return key + length + 1U;
    }
    return key;
}

static int remove_empty_directories(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *item;
    int result = 0;

    if (directory == NULL) {
        return -1;
    }
    while ((item = readdir(directory)) != NULL) {
        char child[4096];
        struct stat status;
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (join_path(child, sizeof(child), path, item->d_name) != 0 ||
            lstat(child, &status) != 0) {
            result = -1;
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            if (remove_empty_directories(child) != 0) {
                result = -1;
            }
        } else {
            result = -1;
        }
    }
    if (closedir(directory) != 0) {
        result = -1;
    }
    if (result == 0 && rmdir(path) != 0) {
        return -1;
    }
    return result;
}

static int copy_local_to_s3(client_t *client, const char *source,
                            const s3_uri_t *destination, int recursive, int move) {
    struct stat status;
    char key[AWS_S3_KEY_MAX + 1U];

    if (lstat(source, &status) != 0 || S_ISLNK(status.st_mode)) {
        fprintf(stderr, "aws-s3: origen local inválido: %s\n", source);
        return -1;
    }
    if (S_ISREG(status.st_mode)) {
        if (destination_key(key, sizeof(key), destination,
                            path_basename_const(source)) != 0 ||
            remote_put(client, source, destination->bucket, key) != 0) {
            return -1;
        }
        return !move || unlink(source) == 0 ? 0 : -1;
    }
    if (!S_ISDIR(status.st_mode) || !recursive) {
        fprintf(stderr, "aws-s3: use --recursive para copiar un directorio\n");
        return -1;
    }
    {
        manifest_t local;
        char error[AWS_S3_ERROR_SIZE];
        size_t index;
        manifest_init(&local);
        if (manifest_collect_local(source, &local, error, sizeof(error)) != 0) {
            fprintf(stderr, "aws-s3: %s\n", error);
            manifest_free(&local);
            return -1;
        }
        for (index = 0U; index < local.count; ++index) {
            if (join_object_key(key, sizeof(key), destination->key,
                                local.entries[index].name) != 0 ||
                remote_put(client, local.entries[index].path,
                           destination->bucket, key) != 0 ||
                (move && unlink(local.entries[index].path) != 0)) {
                fprintf(stderr, "aws-s3: operación detenida tras %zu archivo(s)\n", index);
                manifest_free(&local);
                return -1;
            }
        }
        manifest_free(&local);
    }
    if (move) {
        remove_empty_directories(source);
    }
    return 0;
}

static int copy_s3_to_local(client_t *client, const s3_uri_t *source,
                            const char *destination, int recursive, int move) {
    if (!recursive) {
        char path[4096];
        if (source->key[0] == '\0') {
            fprintf(stderr, "aws-s3: se requiere --recursive para un bucket o prefijo\n");
            return -1;
        }
        if (destination_local_path(path, sizeof(path), destination,
                                   path_basename_const(source->key)) != 0 ||
            remote_get(client, source->bucket, source->key, path) != 0) {
            return -1;
        }
        return !move || remote_delete(client, source->bucket, source->key, 0) == 0
                   ? 0 : -1;
    }
    {
        manifest_t remote;
        size_t index;
        manifest_init(&remote);
        if (remote_list(client, source->bucket, source->key, 1, &remote) != 0) {
            manifest_free(&remote);
            return -1;
        }
        for (index = 0U; index < remote.count; ++index) {
            char path[4096];
            const char *relative = relative_remote_name(remote.entries[index].name,
                                                        source->key);
            if (remote.entries[index].is_prefix) {
                continue;
            }
            if (join_path(path, sizeof(path), destination, relative) != 0 ||
                remote_get(client, source->bucket, remote.entries[index].name, path) != 0 ||
                (move && remote_delete(client, source->bucket,
                                       remote.entries[index].name, 0) != 0)) {
                fprintf(stderr, "aws-s3: operación detenida tras %zu objeto(s)\n", index);
                manifest_free(&remote);
                return -1;
            }
        }
        manifest_free(&remote);
    }
    return 0;
}

static int copy_s3_to_s3(client_t *client, const s3_uri_t *source,
                         const s3_uri_t *destination, int recursive, int move) {
    char key[AWS_S3_KEY_MAX + 1U];
    if (!recursive) {
        if (source->key[0] == '\0') {
            fprintf(stderr, "aws-s3: falta la clave del objeto origen\n");
            return -1;
        }
        if (destination_key(key, sizeof(key), destination,
                            path_basename_const(source->key)) != 0) {
            return -1;
        }
        return remote_copy_or_move(client, move, source->bucket, source->key,
                                   destination->bucket, key);
    }
    {
        manifest_t remote;
        size_t index;
        manifest_init(&remote);
        if (remote_list(client, source->bucket, source->key, 1, &remote) != 0) {
            manifest_free(&remote);
            return -1;
        }
        for (index = 0U; index < remote.count; ++index) {
            const char *relative = relative_remote_name(remote.entries[index].name,
                                                        source->key);
            if (remote.entries[index].is_prefix) {
                continue;
            }
            if (join_object_key(key, sizeof(key), destination->key, relative) != 0 ||
                remote_copy_or_move(client, move, source->bucket,
                                    remote.entries[index].name,
                                    destination->bucket, key) != 0) {
                fprintf(stderr, "aws-s3: operación detenida tras %zu objeto(s)\n", index);
                manifest_free(&remote);
                return -1;
            }
        }
        manifest_free(&remote);
    }
    return 0;
}

static int command_copy(client_t *client, const char *source_text,
                        const char *destination_text, int recursive, int move) {
    int source_remote = is_s3_uri(source_text);
    int destination_remote = is_s3_uri(destination_text);
    s3_uri_t source;
    s3_uri_t destination;
    char error[AWS_S3_ERROR_SIZE];

    if (!source_remote && !destination_remote) {
        fprintf(stderr, "aws-s3: al menos una ubicación debe ser S3\n");
        return -1;
    }
    if (source_remote &&
        parse_s3_uri(source_text, &source, error, sizeof(error)) != 0) {
        fprintf(stderr, "aws-s3: %s\n", error);
        return -1;
    }
    if (destination_remote &&
        parse_s3_uri(destination_text, &destination, error, sizeof(error)) != 0) {
        fprintf(stderr, "aws-s3: %s\n", error);
        return -1;
    }
    if (!source_remote) {
        return copy_local_to_s3(client, source_text, &destination, recursive, move);
    }
    if (!destination_remote) {
        return copy_s3_to_local(client, &source, destination_text, recursive, move);
    }
    return copy_s3_to_s3(client, &source, &destination, recursive, move);
}

static int sync_local_to_s3(client_t *client, const char *source,
                            const s3_uri_t *destination, int delete_extra) {
    manifest_t local;
    manifest_t remote;
    char error[AWS_S3_ERROR_SIZE];
    char key[AWS_S3_KEY_MAX + 1U];
    size_t index;

    manifest_init(&local);
    manifest_init(&remote);
    if (manifest_collect_local(source, &local, error, sizeof(error)) != 0) {
        fprintf(stderr, "aws-s3: %s\n", error);
        goto failure;
    }
    if (remote_list(client, destination->bucket, destination->key, 1, &remote) != 0) {
        goto failure;
    }
    for (index = 0U; index < local.count; ++index) {
        const manifest_entry_t *existing;
        if (join_object_key(key, sizeof(key), destination->key,
                            local.entries[index].name) != 0) {
            goto failure;
        }
        existing = manifest_find(&remote, key);
        if (existing == NULL || existing->size != local.entries[index].size ||
            existing->mtime != local.entries[index].mtime) {
            if (remote_put(client, local.entries[index].path,
                           destination->bucket, key) != 0) {
                goto failure;
            }
        }
    }
    if (delete_extra) {
        for (index = 0U; index < remote.count; ++index) {
            const char *relative = relative_remote_name(remote.entries[index].name,
                                                        destination->key);
            if (!remote.entries[index].is_prefix &&
                manifest_find(&local, relative) == NULL &&
                remote_delete(client, destination->bucket,
                              remote.entries[index].name, 0) != 0) {
                goto failure;
            }
        }
    }
    manifest_free(&local);
    manifest_free(&remote);
    return 0;

failure:
    manifest_free(&local);
    manifest_free(&remote);
    return -1;
}

static int sync_s3_to_local(client_t *client, const s3_uri_t *source,
                            const char *destination, int delete_extra) {
    manifest_t remote;
    manifest_t local;
    char error[AWS_S3_ERROR_SIZE] = "";
    size_t index;

    manifest_init(&remote);
    manifest_init(&local);
    if (ensure_directory(destination) != 0) {
        fprintf(stderr, "aws-s3: no se pudo crear %s\n", destination);
        goto failure;
    }
    if (remote_list(client, source->bucket, source->key, 1, &remote) != 0 ||
        manifest_collect_local(destination, &local, error, sizeof(error)) != 0) {
        if (*error != '\0') {
            fprintf(stderr, "aws-s3: %s\n", error);
        }
        goto failure;
    }
    for (index = 0U; index < remote.count; ++index) {
        const char *relative;
        const manifest_entry_t *existing;
        char path[4096];
        if (remote.entries[index].is_prefix) {
            continue;
        }
        relative = relative_remote_name(remote.entries[index].name, source->key);
        existing = manifest_find(&local, relative);
        if (existing == NULL || existing->size != remote.entries[index].size ||
            existing->mtime != remote.entries[index].mtime) {
            if (join_path(path, sizeof(path), destination, relative) != 0 ||
                remote_get(client, source->bucket,
                           remote.entries[index].name, path) != 0) {
                goto failure;
            }
        }
    }
    if (delete_extra) {
        for (index = 0U; index < local.count; ++index) {
            char key[AWS_S3_KEY_MAX + 1U];
            if (join_object_key(key, sizeof(key), source->key,
                                local.entries[index].name) != 0) {
                goto failure;
            }
            if (manifest_find(&remote, key) == NULL &&
                unlink(local.entries[index].path) != 0) {
                fprintf(stderr, "aws-s3: no se pudo eliminar %s\n",
                        local.entries[index].path);
                goto failure;
            }
        }
    }
    manifest_free(&remote);
    manifest_free(&local);
    return 0;

failure:
    manifest_free(&remote);
    manifest_free(&local);
    return -1;
}

static int command_sync(client_t *client, const char *source_text,
                        const char *destination_text, int delete_extra) {
    int source_remote = is_s3_uri(source_text);
    int destination_remote = is_s3_uri(destination_text);
    s3_uri_t uri;
    char error[AWS_S3_ERROR_SIZE];

    if (source_remote == destination_remote) {
        fprintf(stderr, "aws-s3: sync requiere una ruta local y una ruta S3\n");
        return -1;
    }
    if (source_remote) {
        if (parse_s3_uri(source_text, &uri, error, sizeof(error)) != 0) {
            fprintf(stderr, "aws-s3: %s\n", error);
            return -1;
        }
        return sync_s3_to_local(client, &uri, destination_text, delete_extra);
    }
    if (parse_s3_uri(destination_text, &uri, error, sizeof(error)) != 0) {
        fprintf(stderr, "aws-s3: %s\n", error);
        return -1;
    }
    return sync_local_to_s3(client, source_text, &uri, delete_extra);
}

static void print_manifest(const manifest_t *manifest) {
    size_t index;
    for (index = 0U; index < manifest->count; ++index) {
        const manifest_entry_t *entry = &manifest->entries[index];
        if (entry->is_prefix) {
            printf("PRE  %s\n", entry->name);
        } else {
            printf("%10llu  %s\n", (unsigned long long) entry->size, entry->name);
        }
    }
}

static int command_ls(client_t *client, int argument_count, char **arguments,
                      int recursive) {
    manifest_t manifest;
    s3_uri_t uri;
    char error[AWS_S3_ERROR_SIZE];
    int result;

    manifest_init(&manifest);
    if (argument_count == 0) {
        result = remote_list(client, "", "", 0, &manifest);
    } else if (argument_count == 1 &&
               parse_s3_uri(arguments[0], &uri, error, sizeof(error)) == 0) {
        result = remote_list(client, uri.bucket, uri.key, recursive, &manifest);
    } else {
        if (argument_count == 1) {
            fprintf(stderr, "aws-s3: %s\n", error);
        } else {
            fprintf(stderr, "aws-s3: ls acepta como máximo una URI\n");
        }
        manifest_free(&manifest);
        return -1;
    }
    if (result == 0) {
        print_manifest(&manifest);
    }
    manifest_free(&manifest);
    return result;
}

static int parse_uri_command(const char *text, s3_uri_t *uri, int require_empty_key) {
    char error[AWS_S3_ERROR_SIZE];
    if (parse_s3_uri(text, uri, error, sizeof(error)) != 0) {
        fprintf(stderr, "aws-s3: %s\n", error);
        return -1;
    }
    if (require_empty_key && uri->key[0] != '\0') {
        fprintf(stderr, "aws-s3: la operación requiere una URI de bucket\n");
        return -1;
    }
    return 0;
}

static int execute_command(client_t *client, const char *command,
                           int argument_count, char **arguments,
                           const command_options_t *options) {
    s3_uri_t uri;
    if (strcmp(command, "ls") == 0) {
        return command_ls(client, argument_count, arguments, options->recursive);
    }
    if (strcmp(command, "mb") == 0) {
        if (argument_count != 1 || parse_uri_command(arguments[0], &uri, 1) != 0) {
            return -1;
        }
        return request_one_string(client, OP_MAKE_BUCKET, 0U, uri.bucket);
    }
    if (strcmp(command, "rb") == 0) {
        if (argument_count != 1 || parse_uri_command(arguments[0], &uri, 1) != 0) {
            return -1;
        }
        return request_one_string(client, OP_REMOVE_BUCKET,
                                  options->force ? FLAG_FORCE : 0U, uri.bucket);
    }
    if (strcmp(command, "rm") == 0) {
        if (argument_count != 1 || parse_uri_command(arguments[0], &uri, 0) != 0 ||
            (uri.key[0] == '\0' && !options->recursive)) {
            fprintf(stderr, "aws-s3: rm requiere un objeto o --recursive\n");
            return -1;
        }
        return remote_delete(client, uri.bucket, uri.key, options->recursive);
    }
    if (strcmp(command, "cp") == 0 || strcmp(command, "mv") == 0) {
        if (argument_count != 2) {
            fprintf(stderr, "aws-s3: %s requiere origen y destino\n", command);
            return -1;
        }
        return command_copy(client, arguments[0], arguments[1],
                            options->recursive, strcmp(command, "mv") == 0);
    }
    if (strcmp(command, "sync") == 0) {
        if (argument_count != 2) {
            fprintf(stderr, "aws-s3: sync requiere origen y destino\n");
            return -1;
        }
        return command_sync(client, arguments[0], arguments[1],
                            options->delete_extra);
    }
    fprintf(stderr, "aws-s3: comando desconocido: %s\n", command);
    return -1;
}

static void usage(FILE *stream) {
    fprintf(stream,
            "Uso: aws-s3 [--host HOST] [--port PUERTO] COMANDO [ARGUMENTOS] [OPCIONES]\n"
            "Comandos: ls, mb, cp, mv, rm, sync, rb\n"
            "Opciones: --recursive, --delete, --force\n");
}

int main(int argc, char **argv) {
    const char *host = getenv("AWS_S3_HOST");
    const char *port = getenv("AWS_S3_PORT");
    const char *command;
    char **arguments;
    int argument_count = 0;
    int index = 1;
    char error[AWS_S3_ERROR_SIZE];
    command_options_t options = {0, 0, 0};
    client_t client;
    int result;

    if (host == NULL || *host == '\0') {
        host = "127.0.0.1";
    }
    if (port == NULL || *port == '\0') {
        port = "9000";
    }
    while (index < argc) {
        if (strcmp(argv[index], "--host") == 0 && index + 1 < argc) {
            host = argv[index + 1];
            index += 2;
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            port = argv[index + 1];
            index += 2;
        } else {
            break;
        }
    }
    if (index >= argc) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    command = argv[index++];
    arguments = calloc((size_t) (argc - index + 1), sizeof(*arguments));
    if (arguments == NULL) {
        return EXIT_FAILURE;
    }
    for (; index < argc; ++index) {
        if (strcmp(argv[index], "--recursive") == 0) {
            options.recursive = 1;
        } else if (strcmp(argv[index], "--delete") == 0) {
            options.delete_extra = 1;
        } else if (strcmp(argv[index], "--force") == 0) {
            options.force = 1;
        } else if (strncmp(argv[index], "--", 2U) == 0) {
            fprintf(stderr, "aws-s3: opción desconocida: %s\n", argv[index]);
            free(arguments);
            return EXIT_FAILURE;
        } else {
            arguments[argument_count++] = argv[index];
        }
    }
    signal(SIGPIPE, SIG_IGN);
    client.fd = connect_tcp(host, port, error, sizeof(error));
    if (client.fd < 0) {
        fprintf(stderr, "aws-s3: %s\n", error);
        free(arguments);
        return EXIT_FAILURE;
    }
    result = execute_command(&client, command, argument_count, arguments, &options);
    close(client.fd);
    free(arguments);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
