#include "runtime/iron_runtime.h"
#include "stdlib/iron_http.h"
#include "stdlib/iron_http_bench.h"
#include "picohttpparser.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct Workload {
    uint8_t *data;
    size_t len;
    size_t messages;
} Workload;

static const char small_request[] =
    "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";

static const char browser_request[] =
    "GET /products/42?currency=USD&view=full HTTP/1.1\r\n"
    "Host: shop.example.test\r\n"
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/129 Safari/537.36\r\n"
    "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8\r\n"
    "Accept-Language: en-US,en;q=0.9,fr;q=0.7\r\n"
    "Accept-Encoding: gzip, deflate, br\r\n"
    "Cache-Control: max-age=0\r\n"
    "Sec-Fetch-Dest: document\r\n"
    "Sec-Fetch-Mode: navigate\r\n"
    "Sec-Fetch-Site: same-origin\r\n"
    "Upgrade-Insecure-Requests: 1\r\n"
    "Cookie: session=0123456789abcdef; theme=dark; experiment=http-baseline\r\n"
    "X-Request-ID: 4fcb52ce-2db5-4a57-8996-739598bc3644\r\n"
    "Connection: keep-alive\r\n\r\n";

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static int write_all(int fd, const uint8_t *data, size_t len) {
    while (len) {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        data += (size_t)n;
        len -= (size_t)n;
    }
    return 1;
}

static int read_all(int fd, uint8_t *data, size_t len) {
    while (len) {
        ssize_t n = recv(fd, data, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        data += (size_t)n;
        len -= (size_t)n;
    }
    return 1;
}

static Workload make_workload(const char *name) {
    Workload out;
    memset(&out, 0, sizeof(out));
    if (strcmp(name, "small") == 0 || strcmp(name, "browser") == 0) {
        const char *source = strcmp(name, "small") == 0
            ? small_request : browser_request;
        out.len = strlen(source);
        out.data = (uint8_t *)malloc(out.len);
        if (out.data) memcpy(out.data, source, out.len);
        out.messages = 1;
        return out;
    }
    if (strcmp(name, "pipeline") == 0) {
        out.messages = 128;
        out.len = strlen(small_request) * out.messages;
        out.data = (uint8_t *)malloc(out.len);
        if (!out.data) return out;
        for (size_t i = 0; i < out.messages; i++)
            memcpy(out.data + i * strlen(small_request), small_request,
                   strlen(small_request));
        return out;
    }
    if (strcmp(name, "chunked") == 0) {
        const size_t chunk_count = 16;
        const size_t chunk_size = 65536;
        const char *head = "POST /upload HTTP/1.1\r\nHost: localhost\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n";
        size_t capacity = strlen(head) + chunk_count * (chunk_size + 64) + 64;
        out.data = (uint8_t *)malloc(capacity);
        if (!out.data) return out;
        size_t used = 0;
        memcpy(out.data + used, head, strlen(head));
        used += strlen(head);
        for (size_t i = 0; i < chunk_count; i++) {
            int n = snprintf((char *)out.data + used, capacity - used,
                             "%zx;part=%zu\r\n", chunk_size, i);
            used += (size_t)n;
            memset(out.data + used, 'a' + (int)(i % 26), chunk_size);
            used += chunk_size;
            memcpy(out.data + used, "\r\n", 2);
            used += 2;
        }
        const char *trailer = "0\r\nX-Checksum: baseline\r\n\r\n";
        memcpy(out.data + used, trailer, strlen(trailer));
        used += strlen(trailer);
        out.len = used;
        out.messages = 1;
    }
    return out;
}

static int header_is(const struct phr_header *header, const char *name) {
    size_t len = strlen(name);
    return header->name_len == len && strncasecmp(header->name, name, len) == 0;
}

static uint64_t pico_parse(const Workload *workload, size_t repetitions,
                           int64_t *error) {
    uint64_t checksum = 0;
    uint8_t *scratch = (uint8_t *)malloc(workload->len + 1);
    *error = scratch ? 0 : -1;
    if (!scratch) return 0;
    for (size_t repetition = 0; repetition < repetitions; repetition++) {
        size_t offset = 0;
        for (size_t message = 0; message < workload->messages; message++) {
            const char *method = NULL, *target = NULL;
            size_t method_len = 0, target_len = 0;
            int minor = 0;
            struct phr_header headers[64];
            size_t count = sizeof(headers) / sizeof(headers[0]);
            int consumed = phr_parse_request(
                (const char *)workload->data + offset,
                workload->len - offset, &method, &method_len,
                &target, &target_len, &minor, headers, &count, 0);
            if (consumed < 0) { *error = consumed; free(scratch); return checksum; }
            size_t total = (size_t)consumed;
            for (size_t i = 0; i < count; i++) {
                if (header_is(&headers[i], "Transfer-Encoding") &&
                    headers[i].value_len == 7 &&
                    strncasecmp(headers[i].value, "chunked", 7) == 0) {
                    size_t encoded = workload->len - offset - total;
                    memcpy(scratch, workload->data + offset + total, encoded);
                    struct phr_chunked_decoder decoder;
                    memset(&decoder, 0, sizeof(decoder));
                    decoder.consume_trailer = 1;
                    size_t decoded = encoded;
                    ssize_t left = phr_decode_chunked(
                        &decoder, (char *)scratch, &decoded);
                    if (left < 0) { *error = left; free(scratch); return checksum; }
                    total += encoded - (size_t)left;
                    checksum += decoded * 11u;
                }
            }
            checksum += method_len * 3u + target_len * 5u + count * 7u +
                        (uint64_t)minor;
            offset += total;
        }
    }
    free(scratch);
    return checksum;
}

static int run_parse(const char *implementation, const char *name,
                     size_t repetitions) {
    Workload workload = make_workload(name);
    if (!workload.data) return 2;
    int64_t error = 0;
    uint64_t start = monotonic_ns();
    uint64_t checksum = strcmp(implementation, "iron") == 0
        ? iron_http_bench_parse_requests(workload.data, workload.len,
              workload.messages, repetitions, &error)
        : pico_parse(&workload, repetitions, &error);
    uint64_t elapsed = monotonic_ns() - start;
    printf("SAMPLE %s parse %s %llu %llu %llu %llu\n", implementation,
           name, (unsigned long long)(workload.len * repetitions),
           (unsigned long long)(workload.messages * repetitions),
           (unsigned long long)elapsed, (unsigned long long)checksum);
    free(workload.data);
    return error == 0 ? 0 : 1;
}

static Workload make_body(const char *name) {
    Workload body;
    memset(&body, 0, sizeof(body));
    if (strcmp(name, "small") == 0) body.len = 2;
    else if (strcmp(name, "json4k") == 0) body.len = 4096;
    else if (strcmp(name, "large") == 0) body.len = 1024 * 1024;
    body.data = (uint8_t *)malloc(body.len ? body.len : 1);
    if (body.data) memset(body.data, 'x', body.len);
    return body;
}

static uint64_t c_serialize(const Workload *body, size_t repetitions,
                            uint64_t *bytes, int64_t *error) {
    const char *headers = "Content-Type: application/octet-stream\r\n";
    size_t capacity = body->len + 256;
    uint8_t *output = (uint8_t *)malloc(capacity);
    if (!output) { *error = -1; return 0; }
    uint64_t checksum = 0;
    *bytes = 0;
    *error = 0;
    for (size_t i = 0; i < repetitions; i++) {
        int n = snprintf((char *)output, capacity,
            "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: keep-alive\r\n%s\r\n",
            body->len, headers);
        memcpy(output + (size_t)n, body->data, body->len);
        size_t used = (size_t)n + body->len;
        *bytes += used;
        checksum += used + output[used - 1];
    }
    free(output);
    return checksum;
}

static int run_serialize(const char *implementation, const char *name,
                         size_t repetitions) {
    Workload body = make_body(name);
    if (!body.data) return 2;
    const char *headers = "Content-Type: application/octet-stream";
    uint64_t bytes = 0;
    int64_t error = 0;
    uint64_t start = monotonic_ns();
    uint64_t checksum = strcmp(implementation, "iron") == 0
        ? iron_http_bench_serialize_response(200, headers, strlen(headers),
              body.data, body.len, repetitions, &bytes, &error)
        : c_serialize(&body, repetitions, &bytes, &error);
    uint64_t elapsed = monotonic_ns() - start;
    printf("SAMPLE %s serialize %s %llu %llu %llu %llu\n",
           implementation, name, (unsigned long long)bytes,
           (unsigned long long)repetitions, (unsigned long long)elapsed,
           (unsigned long long)checksum);
    free(body.data);
    return error == 0 ? 0 : 1;
}

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0)
        return -1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0) { close(fd); return -1; }
    return fd;
}

static int serve_iron(int listener, size_t total) {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) return 1;
    Iron_HttpConnection connection = {fd};
    Iron_HttpResponse response = Iron_http_text_response(
        200, iron_string_from_literal("ok", 2));
    for (size_t i = 0; i < total; i++) {
        Iron_HttpRequest request = Iron_httpconnection_read_request(
            connection, 65536, 1024, 10000);
        if (request.error != 0 ||
            Iron_httpconnection_send_response_keep_alive(
                connection, response, true, 10000) != 0) {
            Iron_httprequest_dispose(request);
            Iron_httpresponse_dispose(response);
            Iron_httpconnection_close(connection);
            return 1;
        }
        Iron_httprequest_dispose(request);
    }
    Iron_httpresponse_dispose(response);
    Iron_httpconnection_close(connection);
    return 0;
}

static int serve_pico(int listener, size_t total) {
    static const char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n\r\nok";
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) return 1;
    char buffer[65536];
    size_t used = 0;
    for (size_t i = 0; i < total; i++) {
        int consumed = -2;
        while (consumed == -2) {
            ssize_t n = recv(fd, buffer + used, sizeof(buffer) - used, 0);
            if (n <= 0) { close(fd); return 1; }
            used += (size_t)n;
            const char *method = NULL, *target = NULL;
            size_t method_len = 0, target_len = 0;
            int minor = 0;
            struct phr_header headers[32];
            size_t count = 32;
            consumed = phr_parse_request(buffer, used, &method, &method_len,
                &target, &target_len, &minor, headers, &count, 0);
        }
        if (consumed < 0 || !write_all(fd, (const uint8_t *)response,
                                        strlen(response))) {
            close(fd);
            return 1;
        }
        memmove(buffer, buffer + consumed, used - (size_t)consumed);
        used -= (size_t)consumed;
    }
    close(fd);
    return 0;
}

static int run_server(const char *implementation, int port, size_t total) {
    int listener = listen_on(port);
    if (listener < 0) return 2;
    puts("READY");
    fflush(stdout);
    int result = strcmp(implementation, "iron") == 0
        ? serve_iron(listener, total) : serve_pico(listener, total);
    close(listener);
    return result;
}

static int connect_to(int port) {
    for (int attempt = 0; attempt < 200; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons((uint16_t)port);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&address,
                              sizeof(address)) == 0) return fd;
        if (fd >= 0) close(fd);
        usleep(10000);
    }
    return -1;
}

static int exchange_one(int fd) {
    enum { RESPONSE_LEN = 105 };
    static const char request[] =
        "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    uint8_t response[RESPONSE_LEN];
    return write_all(fd, (const uint8_t *)request, strlen(request)) &&
           read_all(fd, response, RESPONSE_LEN);
}

static int run_load(const char *implementation, int port,
                    size_t warmup, size_t count) {
    int fd = connect_to(port);
    if (fd < 0) return 2;
    for (size_t i = 0; i < warmup; i++)
        if (!exchange_one(fd)) { close(fd); return 1; }
    uint64_t start = monotonic_ns();
    for (size_t i = 0; i < count; i++)
        if (!exchange_one(fd)) { close(fd); return 1; }
    uint64_t elapsed = monotonic_ns() - start;
    close(fd);
    printf("SAMPLE %s e2e minimal 0 %llu %llu %llu\n", implementation,
           (unsigned long long)count, (unsigned long long)elapsed,
           (unsigned long long)(count * 17u));
    return 0;
}

static size_t number(const char *text) {
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (!end || *end != 0 || value > SIZE_MAX) return 0;
    return (size_t)value;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    iron_runtime_init(0, NULL);
    int result = 2;
    if (argc == 5 && strcmp(argv[1], "parse") == 0)
        result = run_parse(argv[2], argv[3], number(argv[4]));
    else if (argc == 5 && strcmp(argv[1], "serialize") == 0)
        result = run_serialize(argv[2], argv[3], number(argv[4]));
    else if (argc == 5 && strcmp(argv[1], "serve") == 0)
        result = run_server(argv[2], (int)number(argv[3]), number(argv[4]));
    else if (argc == 6 && strcmp(argv[1], "load") == 0)
        result = run_load(argv[2], (int)number(argv[3]),
                          number(argv[4]), number(argv[5]));
    else
        fprintf(stderr, "usage: %s parse|serialize impl workload repetitions\n"
                        "       %s serve impl port requests\n"
                        "       %s load impl port warmup requests\n",
                argv[0], argv[0], argv[0]);
    iron_runtime_shutdown();
    return result;
}
