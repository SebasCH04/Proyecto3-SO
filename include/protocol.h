#ifndef AWS_S3_PROTOCOL_H
#define AWS_S3_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_MAGIC 0x53334653U
#define PROTOCOL_VERSION 1U
#define PROTOCOL_HEADER_SIZE 24U
#define PROTOCOL_MAX_CONTROL_PAYLOAD (64U * 1024U * 1024U)

enum protocol_opcode {
    OP_LIST_BUCKETS = 1,
    OP_LIST_OBJECTS = 2,
    OP_MAKE_BUCKET = 3,
    OP_REMOVE_BUCKET = 4,
    OP_PUT_OBJECT = 5,
    OP_GET_OBJECT = 6,
    OP_COPY_OBJECT = 7,
    OP_MOVE_OBJECT = 8,
    OP_DELETE_OBJECT = 9
};

enum protocol_flags {
    FLAG_RECURSIVE = 1U << 0,
    FLAG_DELETE = 1U << 1,
    FLAG_FORCE = 1U << 2
};

enum protocol_status {
    STATUS_OK = 0,
    STATUS_BAD_REQUEST = 1,
    STATUS_NOT_FOUND = 2,
    STATUS_ALREADY_EXISTS = 3,
    STATUS_NOT_EMPTY = 4,
    STATUS_LIMIT = 5,
    STATUS_IO_ERROR = 6,
    STATUS_INTERNAL_ERROR = 7
};

typedef struct {
    uint16_t opcode;
    uint32_t flags;
    uint32_t status;
    uint64_t payload_length;
} protocol_header_t;

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} protocol_buffer_t;

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t position;
} protocol_reader_t;

int protocol_send_header(int fd, const protocol_header_t *header);
int protocol_receive_header(int fd, protocol_header_t *header);
int protocol_send_error(int fd, uint16_t opcode, uint32_t status, const char *message);
int protocol_receive_control_payload(int fd, uint64_t length, unsigned char **payload);

void protocol_buffer_init(protocol_buffer_t *buffer);
void protocol_buffer_free(protocol_buffer_t *buffer);
int protocol_buffer_put_u32(protocol_buffer_t *buffer, uint32_t value);
int protocol_buffer_put_u64(protocol_buffer_t *buffer, uint64_t value);
int protocol_buffer_put_string(protocol_buffer_t *buffer, const char *value);

void protocol_reader_init(protocol_reader_t *reader, const void *data, size_t length);
int protocol_reader_get_u32(protocol_reader_t *reader, uint32_t *value);
int protocol_reader_get_u64(protocol_reader_t *reader, uint64_t *value);
int protocol_reader_get_string(protocol_reader_t *reader, char **value);
int protocol_reader_finished(const protocol_reader_t *reader);

#endif

