#include "uri.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    s3_uri_t uri;
    char error[256];
    char key[512];

    assert(parse_s3_uri("s3://mi-bucket", &uri, error, sizeof(error)) == 0);
    assert(strcmp(uri.bucket, "mi-bucket") == 0);
    assert(strcmp(uri.key, "") == 0);

    assert(parse_s3_uri("s3://mi-bucket/fotos/uno.jpg", &uri,
                        error, sizeof(error)) == 0);
    assert(strcmp(uri.key, "fotos/uno.jpg") == 0);
    assert(uri.key_had_trailing_slash == 0);

    assert(parse_s3_uri("s3://mi-bucket/fotos/", &uri,
                        error, sizeof(error)) == 0);
    assert(strcmp(uri.key, "fotos") == 0);
    assert(uri.key_had_trailing_slash == 1);

    assert(parse_s3_uri("archivo.txt", &uri, error, sizeof(error)) != 0);
    assert(parse_s3_uri("s3:///archivo.txt", &uri, error, sizeof(error)) != 0);
    assert(parse_s3_uri("s3://bucket/a/../b", &uri, error, sizeof(error)) != 0);
    assert(validate_bucket_name("mal nombre", error, sizeof(error)) != 0);

    assert(join_object_key(key, sizeof(key), "base", "sub/a.txt") == 0);
    assert(strcmp(key, "base/sub/a.txt") == 0);
    assert(join_object_key(key, sizeof(key), "", "a.txt") == 0);
    assert(strcmp(key, "a.txt") == 0);

    puts("test_uri: OK");
    return 0;
}

