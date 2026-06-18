#include "common.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_buffer(void) {
    protocol_buffer_t buffer;
    protocol_reader_t reader;
    uint32_t value32;
    uint64_t value64;
    char *text = NULL;

    protocol_buffer_init(&buffer);
    assert(protocol_buffer_put_u32(&buffer, 42U) == 0);
    assert(protocol_buffer_put_u64(&buffer, UINT64_C(0x1122334455667788)) == 0);
    assert(protocol_buffer_put_string(&buffer, "archivo con espacios.bin") == 0);

    protocol_reader_init(&reader, buffer.data, buffer.length);
    assert(protocol_reader_get_u32(&reader, &value32) == 0 && value32 == 42U);
    assert(protocol_reader_get_u64(&reader, &value64) == 0 &&
           value64 == UINT64_C(0x1122334455667788));
    assert(protocol_reader_get_string(&reader, &text) == 0);
    assert(strcmp(text, "archivo con espacios.bin") == 0);
    assert(protocol_reader_finished(&reader));

    free(text);
    protocol_buffer_free(&buffer);
}

static void build_wire_header(unsigned char wire[PROTOCOL_HEADER_SIZE]) {
    uint32_t value32;
    uint16_t value16;
    uint64_t value64;

    value32 = htonl(PROTOCOL_MAGIC);
    memcpy(wire, &value32, sizeof(value32));
    value16 = htons(PROTOCOL_VERSION);
    memcpy(wire + 4U, &value16, sizeof(value16));
    value16 = htons(OP_PUT_OBJECT);
    memcpy(wire + 6U, &value16, sizeof(value16));
    value32 = htonl(FLAG_RECURSIVE);
    memcpy(wire + 8U, &value32, sizeof(value32));
    value32 = htonl(STATUS_OK);
    memcpy(wire + 12U, &value32, sizeof(value32));
    value64 = host_to_be64(UINT64_C(9876543210));
    memcpy(wire + 16U, &value64, sizeof(value64));
}

static void test_fragmented_header(void) {
    int sockets[2];
    pid_t child;
    protocol_header_t header;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        unsigned char wire[PROTOCOL_HEADER_SIZE];
        size_t index;
        close(sockets[0]);
        build_wire_header(wire);
        for (index = 0U; index < sizeof(wire); ++index) {
            if (write_full(sockets[1], wire + index, 1U) != 0) {
                _exit(2);
            }
        }
        close(sockets[1]);
        _exit(0);
    }
    close(sockets[1]);
    assert(protocol_receive_header(sockets[0], &header) == 1);
    assert(header.opcode == OP_PUT_OBJECT);
    assert(header.flags == FLAG_RECURSIVE);
    assert(header.status == STATUS_OK);
    assert(header.payload_length == UINT64_C(9876543210));
    close(sockets[0]);
    assert(waitpid(child, NULL, 0) == child);
}

int main(void) {
    test_buffer();
    test_fragmented_header();
    puts("test_protocol: OK");
    return 0;
}

