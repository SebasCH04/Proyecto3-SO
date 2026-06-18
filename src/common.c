#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int read_full(int fd, void *buffer, size_t length) {
    unsigned char *cursor = buffer;
    size_t completed = 0U;

    while (completed < length) {
        ssize_t amount = read(fd, cursor + completed, length - completed);
        if (amount == 0) {
            return 0;
        }
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        completed += (size_t) amount;
    }
    return 1;
}

int write_full(int fd, const void *buffer, size_t length) {
    const unsigned char *cursor = buffer;
    size_t completed = 0U;

    while (completed < length) {
        ssize_t amount = write(fd, cursor + completed, length - completed);
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

int copy_exact(int input_fd, int output_fd, uint64_t length) {
    unsigned char buffer[AWS_S3_IO_CHUNK];
    uint64_t remaining = length;

    while (remaining > 0U) {
        size_t requested = sizeof(buffer);
        if (remaining < (uint64_t) requested) {
            requested = (size_t) remaining;
        }
        int result = read_full(input_fd, buffer, requested);
        if (result != 1 || write_full(output_fd, buffer, requested) != 0) {
            return -1;
        }
        remaining -= requested;
    }
    return 0;
}

int copy_exact_at(int input_fd, uint64_t offset, int output_fd, uint64_t length) {
    unsigned char buffer[AWS_S3_IO_CHUNK];
    uint64_t remaining = length;
    uint64_t position = offset;

    while (remaining > 0U) {
        size_t requested = sizeof(buffer);
        ssize_t amount;
        if (remaining < (uint64_t) requested) {
            requested = (size_t) remaining;
        }
        amount = pread(input_fd, buffer, requested, (off_t) position);
        if (amount == 0) {
            return -1;
        }
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (write_full(output_fd, buffer, (size_t) amount) != 0) {
            return -1;
        }
        position += (uint64_t) amount;
        remaining -= (uint64_t) amount;
    }
    return 0;
}

uint64_t host_to_be64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t) htonl((uint32_t) (value >> 32U))) |
           ((uint64_t) htonl((uint32_t) value) << 32U);
#else
    return value;
#endif
}

uint64_t be64_to_host(uint64_t value) {
    return host_to_be64(value);
}

const char *path_basename_const(const char *path) {
    const char *end;
    const char *slash;

    if (path == NULL || *path == '\0') {
        return "";
    }
    end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/') {
        --end;
    }
    slash = end;
    while (slash > path && slash[-1] != '/') {
        --slash;
    }
    return slash;
}

int join_path(char *output, size_t capacity, const char *left, const char *right) {
    size_t left_length = strlen(left);
    int needs_slash = left_length > 0U && left[left_length - 1U] != '/';
    int written = snprintf(output, capacity, "%s%s%s", left,
                           needs_slash ? "/" : "", right);
    return written >= 0 && (size_t) written < capacity ? 0 : -1;
}

int ensure_parent_directories(const char *path) {
    char *copy = strdup(path);
    char *cursor;

    if (copy == NULL) {
        return -1;
    }
    for (cursor = copy + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
            free(copy);
            return -1;
        }
        *cursor = '/';
    }
    free(copy);
    return 0;
}

int connect_tcp(const char *host, const char *port, char *error, size_t error_size) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *current;
    int socket_fd = -1;
    int code;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    code = getaddrinfo(host, port, &hints, &addresses);
    if (code != 0) {
        snprintf(error, error_size, "no se pudo resolver %s: %s",
                 host, gai_strerror(code));
        return -1;
    }
    for (current = addresses; current != NULL; current = current->ai_next) {
        socket_fd = socket(current->ai_family, current->ai_socktype,
                           current->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        if (connect(socket_fd, current->ai_addr, current->ai_addrlen) == 0) {
            break;
        }
        close(socket_fd);
        socket_fd = -1;
    }
    if (socket_fd < 0) {
        snprintf(error, error_size, "no se pudo conectar con %s:%s: %s",
                 host, port, strerror(errno));
    }
    freeaddrinfo(addresses);
    return socket_fd;
}
