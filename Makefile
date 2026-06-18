CC ?= gcc
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -O2
CPPFLAGS ?= -Iinclude
LDFLAGS ?=

BUILD_DIR := build
BIN_DIR := bin

COMMON_SRC := src/common.c src/protocol.c src/uri.c src/manifest.c
CLIENT_SRC := src/client.c $(COMMON_SRC)
SERVER_SRC := src/server.c src/storage_file.c $(COMMON_SRC)

CLIENT_OBJ := $(CLIENT_SRC:src/%.c=$(BUILD_DIR)/%.o)
SERVER_OBJ := $(SERVER_SRC:src/%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean

all: $(BIN_DIR)/aws-s3 $(BIN_DIR)/aws-s3_server

$(BIN_DIR)/aws-s3: $(CLIENT_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/aws-s3_server: $(SERVER_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
