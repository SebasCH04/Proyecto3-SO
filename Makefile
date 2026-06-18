CC ?= gcc
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -O2
CPPFLAGS ?= -IH
LDFLAGS ?=

BUILD_DIR := build
BIN_DIR := bin

COMMON_SRC := C/common.c C/protocol.c C/uri.c C/manifest.c
CLIENT_SRC := C/client.c $(COMMON_SRC)
SERVER_SRC := C/server.c C/storage_file.c $(COMMON_SRC)

CLIENT_OBJ := $(CLIENT_SRC:C/%.c=$(BUILD_DIR)/%.o)
SERVER_OBJ := $(SERVER_SRC:C/%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean

all: $(BIN_DIR)/aws-s3 $(BIN_DIR)/aws-s3_server

$(BIN_DIR)/aws-s3: $(CLIENT_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/aws-s3_server: $(SERVER_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: C/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
