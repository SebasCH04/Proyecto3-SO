#ifndef AWS_S3_COMMON_H
#define AWS_S3_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define AWS_S3_IO_CHUNK (64U * 1024U)
#define AWS_S3_ERROR_SIZE 256U

int read_full(int fd, void *buffer, size_t length);
int write_full(int fd, const void *buffer, size_t length);
int copy_exact(int input_fd, int output_fd, uint64_t length);
int copy_exact_at(int input_fd, uint64_t offset, int output_fd, uint64_t length);
uint64_t host_to_be64(uint64_t value);
uint64_t be64_to_host(uint64_t value);
const char *path_basename_const(const char *path);
int join_path(char *output, size_t capacity, const char *left, const char *right);
int ensure_parent_directories(const char *path);
int connect_tcp(const char *host, const char *port, char *error, size_t error_size);

#endif

