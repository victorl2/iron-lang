#ifndef IRON_HTTP_BENCH_H
#define IRON_HTTP_BENCH_H

#include <stddef.h>
#include <stdint.h>

/* Internal benchmark entry points. They run the production parser and
 * serializer over an in-memory transport so parse-only measurements do not
 * include socket system calls. */
uint64_t iron_http_bench_parse_requests(
    const uint8_t *data, size_t len, size_t message_count,
    size_t repetitions, int64_t *error_out);

uint64_t iron_http_bench_serialize_response(
    int64_t status, const char *headers, size_t headers_len,
    const uint8_t *body, size_t body_len, size_t repetitions,
    uint64_t *bytes_out, int64_t *error_out);

#endif
