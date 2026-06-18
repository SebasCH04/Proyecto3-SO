CC ?= gcc
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -O2
CPPFLAGS ?= -Iinclude
LDFLAGS ?=

BUILD_DIR := build
BIN_DIR := bin

COMMON_SRC := src/common.c src/protocol.c src/uri.c src/manifest.c
CLIENT_SRC := src/client.c $(COMMON_SRC)
SERVER_SRC := src/server.c src/storage_mock.c $(COMMON_SRC)

CLIENT_OBJ := $(CLIENT_SRC:src/%.c=$(BUILD_DIR)/%.o)
SERVER_OBJ := $(SERVER_SRC:src/%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean test

all: $(BIN_DIR)/aws-s3 $(BIN_DIR)/aws-s3_server

$(BIN_DIR)/aws-s3: $(CLIENT_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/aws-s3_server: $(SERVER_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_uri: tests/test_uri.c src/uri.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test_protocol: tests/test_protocol.c src/common.c src/protocol.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

test: all $(BUILD_DIR)/test_uri $(BUILD_DIR)/test_protocol
	$(BUILD_DIR)/test_uri
	$(BUILD_DIR)/test_protocol
	sh tests/integration.sh

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

