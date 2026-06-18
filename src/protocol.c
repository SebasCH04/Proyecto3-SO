#include "protocol.h"

#include "common.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

//aumenta el buffer solo cuando el espacio actual no alcanza
static int buffer_reserve(protocol_buffer_t *buffer, size_t additional) {
    size_t required;
    size_t capacity;
    unsigned char *replacement;

    if (additional > SIZE_MAX - buffer->length) {
        return -1;
    }
    required = buffer->length + additional;
    if (required <= buffer->capacity) {
        return 0;
    }
    capacity = buffer->capacity == 0U ? 128U : buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    replacement = realloc(buffer->data, capacity);
    if (replacement == NULL) {
        return -1;
    }
    buffer->data = replacement;
    buffer->capacity = capacity;
    return 0;
}

//construye manualmente el encabezado fijo de 24 bytes
int protocol_send_header(int fd, const protocol_header_t *header) {
    unsigned char wire[PROTOCOL_HEADER_SIZE];
    uint32_t value32;
    uint16_t value16;
    uint64_t value64;

    value32 = htonl(PROTOCOL_MAGIC);
    memcpy(wire, &value32, sizeof(value32));
    value16 = htons(PROTOCOL_VERSION);
    memcpy(wire + 4U, &value16, sizeof(value16));
    value16 = htons(header->opcode);
    memcpy(wire + 6U, &value16, sizeof(value16));
    value32 = htonl(header->flags);
    memcpy(wire + 8U, &value32, sizeof(value32));
    value32 = htonl(header->status);
    memcpy(wire + 12U, &value32, sizeof(value32));
    value64 = host_to_be64(header->payload_length);
    memcpy(wire + 16U, &value64, sizeof(value64));
    return write_full(fd, wire, sizeof(wire));
}

//valida la firma y version antes de aceptar el encabezado
int protocol_receive_header(int fd, protocol_header_t *header) {
    unsigned char wire[PROTOCOL_HEADER_SIZE];
    uint32_t value32;
    uint16_t value16;
    uint64_t value64;
    int result = read_full(fd, wire, sizeof(wire));

    if (result != 1) {
        return result;
    }
    memcpy(&value32, wire, sizeof(value32));
    if (ntohl(value32) != PROTOCOL_MAGIC) {
        return -2;
    }
    memcpy(&value16, wire + 4U, sizeof(value16));
    if (ntohs(value16) != PROTOCOL_VERSION) {
        return -2;
    }
    memcpy(&value16, wire + 6U, sizeof(value16));
    header->opcode = ntohs(value16);
    memcpy(&value32, wire + 8U, sizeof(value32));
    header->flags = ntohl(value32);
    memcpy(&value32, wire + 12U, sizeof(value32));
    header->status = ntohl(value32);
    memcpy(&value64, wire + 16U, sizeof(value64));
    header->payload_length = be64_to_host(value64);
    return 1;
}

//envia los errores con el mismo formato que una respuesta normal
int protocol_send_error(int fd, uint16_t opcode, uint32_t status, const char *message) {
    protocol_buffer_t payload;
    protocol_header_t header;
    int result;

    protocol_buffer_init(&payload);
    if (protocol_buffer_put_string(&payload, message == NULL ? "" : message) != 0) {
        protocol_buffer_free(&payload);
        return -1;
    }
    header.opcode = opcode;
    header.flags = 0U;
    header.status = status;
    header.payload_length = payload.length;
    result = protocol_send_header(fd, &header);
    if (result == 0) {
        result = write_full(fd, payload.data, payload.length);
    }
    protocol_buffer_free(&payload);
    return result;
}

//limita los mensajes de control para evitar reservas excesivas
int protocol_receive_control_payload(int fd, uint64_t length, unsigned char **payload) {
    unsigned char *data;

    *payload = NULL;
    if (length > PROTOCOL_MAX_CONTROL_PAYLOAD || length > SIZE_MAX) {
        return -1;
    }
    if (length == 0U) {
        return 0;
    }
    data = malloc((size_t) length);
    if (data == NULL) {
        return -1;
    }
    if (read_full(fd, data, (size_t) length) != 1) {
        free(data);
        return -1;
    }
    *payload = data;
    return 0;
}

void protocol_buffer_init(protocol_buffer_t *buffer) {
    memset(buffer, 0, sizeof(*buffer));
}

void protocol_buffer_free(protocol_buffer_t *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

int protocol_buffer_put_u32(protocol_buffer_t *buffer, uint32_t value) {
    uint32_t wire = htonl(value);
    if (buffer_reserve(buffer, sizeof(wire)) != 0) {
        return -1;
    }
    memcpy(buffer->data + buffer->length, &wire, sizeof(wire));
    buffer->length += sizeof(wire);
    return 0;
}

int protocol_buffer_put_u64(protocol_buffer_t *buffer, uint64_t value) {
    uint64_t wire = host_to_be64(value);
    if (buffer_reserve(buffer, sizeof(wire)) != 0) {
        return -1;
    }
    memcpy(buffer->data + buffer->length, &wire, sizeof(wire));
    buffer->length += sizeof(wire);
    return 0;
}

//serializa una cadena como longitud seguida por sus bytes
int protocol_buffer_put_string(protocol_buffer_t *buffer, const char *value) {
    size_t length = strlen(value);
    if (length > UINT32_MAX ||
        protocol_buffer_put_u32(buffer, (uint32_t) length) != 0 ||
        buffer_reserve(buffer, length) != 0) {
        return -1;
    }
    memcpy(buffer->data + buffer->length, value, length);
    buffer->length += length;
    return 0;
}

void protocol_reader_init(protocol_reader_t *reader, const void *data, size_t length) {
    reader->data = data;
    reader->length = length;
    reader->position = 0U;
}

int protocol_reader_get_u32(protocol_reader_t *reader, uint32_t *value) {
    uint32_t wire;
    if (reader->length - reader->position < sizeof(wire)) {
        return -1;
    }
    memcpy(&wire, reader->data + reader->position, sizeof(wire));
    reader->position += sizeof(wire);
    *value = ntohl(wire);
    return 0;
}

int protocol_reader_get_u64(protocol_reader_t *reader, uint64_t *value) {
    uint64_t wire;
    if (reader->length - reader->position < sizeof(wire)) {
        return -1;
    }
    memcpy(&wire, reader->data + reader->position, sizeof(wire));
    reader->position += sizeof(wire);
    *value = be64_to_host(wire);
    return 0;
}

//reserva una copia terminada en nulo para el texto recibido
int protocol_reader_get_string(protocol_reader_t *reader, char **value) {
    uint32_t length;
    char *text;

    *value = NULL;
    if (protocol_reader_get_u32(reader, &length) != 0 ||
        reader->length - reader->position < length) {
        return -1;
    }
    text = malloc((size_t) length + 1U);
    if (text == NULL) {
        return -1;
    }
    memcpy(text, reader->data + reader->position, length);
    text[length] = '\0';
    reader->position += length;
    *value = text;
    return 0;
}

int protocol_reader_finished(const protocol_reader_t *reader) {
    return reader->position == reader->length;
}
