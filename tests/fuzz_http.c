#include "http.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > 65536) return 0;

    char *buf = malloc(size + 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    http_request_t req;
    size_t header_end = 0;
    http_parse_result_t result = http_parse_request(
        buf,
        size,
        16 * 1024,
        &req,
        &header_end
    );

    if (result == HTTP_PARSE_OK) {
        char forward[32 * 1024];
        size_t forward_len = 0;
        (void)http_build_forward_request(
            &req,
            "127.0.0.1",
            forward,
            sizeof(forward),
            &forward_len
        );
    }

    free(buf);
    return 0;
}
