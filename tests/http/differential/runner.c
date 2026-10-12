#include "runtime/iron_runtime.h"
#include "stdlib/iron_http.h"
#include "picohttpparser.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint8_t *decode_hex(const char *line, size_t text_len, size_t *len) {
    if ((text_len & 1) != 0) return NULL;
    *len = text_len / 2;
    uint8_t *out = (uint8_t *)malloc(*len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < *len; i++) {
        int high = hex_digit((unsigned char)line[i * 2]);
        int low = hex_digit((unsigned char)line[i * 2 + 1]);
        if (high < 0 || low < 0) { free(out); return NULL; }
        out[i] = (uint8_t)((high << 4) | low);
    }
    out[*len] = 0;
    return out;
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

static void print_hex(const char *data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        putchar(digits[c >> 4]);
        putchar(digits[c & 15]);
    }
}

static void print_header(const char *name, size_t name_len,
                         const char *value, size_t value_len) {
    while (value_len && (*value == ' ' || *value == '\t')) {
        value++;
        value_len--;
    }
    while (value_len && (value[value_len - 1] == ' ' ||
                         value[value_len - 1] == '\t')) value_len--;
    putchar(' ');
    print_hex(name, name_len);
    putchar('=');
    print_hex(value, value_len);
}

static void print_iron_headers(Iron_String headers) {
    const char *data = iron_string_cstr(&headers);
    size_t len = iron_string_byte_len(&headers);
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end + 1 < len && !(data[end] == '\r' && data[end + 1] == '\n')) end++;
        if (end + 1 >= len) end = len;
        size_t colon = pos;
        while (colon < end && data[colon] != ':') colon++;
        print_header(data + pos, colon - pos, data + colon + 1,
                     end - colon - 1);
        pos = end == len ? len : end + 2;
    }
}

static void parse_iron(const uint8_t *message, size_t len) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
        !write_all(pair[0], message, len)) {
        puts("ERR");
        return;
    }
    (void)shutdown(pair[0], SHUT_WR);
    Iron_HttpConnection connection = {pair[1]};
    Iron_HttpRequest request = Iron_httpconnection_read_request(
        connection, 16384, 16384, 1000);
    if (request.error != 0) {
        puts("ERR");
    } else {
        printf("OK ");
        print_hex(iron_string_cstr(&request.method),
                  iron_string_byte_len(&request.method));
        putchar(' ');
        print_hex(iron_string_cstr(&request.target),
                  iron_string_byte_len(&request.target));
        printf(" %c", iron_string_cstr(&request.version)[7]);
        print_iron_headers(request.headers);
        putchar('\n');
    }
    Iron_httprequest_release(request);
    close(pair[0]);
    Iron_httpconnection_close(connection);
}

static void parse_pico(const uint8_t *message, size_t len) {
    const char *method = NULL;
    const char *target = NULL;
    size_t method_len = 0, target_len = 0;
    int minor = 0;
    struct phr_header headers[64];
    size_t count = sizeof(headers) / sizeof(headers[0]);
    int consumed = phr_parse_request((const char *)message, len,
        &method, &method_len, &target, &target_len, &minor,
        headers, &count, 0);
    if (consumed < 0 || (minor != 0 && minor != 1)) {
        puts("ERR");
        return;
    }
    int host_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (headers[i].name_len == 4 &&
            strncasecmp(headers[i].name, "Host", 4) == 0) host_count++;
    }
    if ((minor == 1 && host_count != 1) || (minor == 0 && host_count > 1)) {
        puts("ERR");
        return;
    }
    printf("OK ");
    print_hex(method, method_len);
    putchar(' ');
    print_hex(target, target_len);
    printf(" %d", minor);
    for (size_t i = 0; i < count; i++)
        print_header(headers[i].name, headers[i].name_len,
                     headers[i].value, headers[i].value_len);
    putchar('\n');
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "iron") != 0 &&
                      strcmp(argv[1], "pico") != 0)) {
        fprintf(stderr, "usage: %s iron|pico messages.hex\n", argv[0]);
        return 2;
    }
    FILE *input = fopen(argv[2], "rb");
    if (!input) { perror(argv[2]); return 2; }
    iron_runtime_init(0, NULL);
    char *line = NULL;
    size_t capacity = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &capacity, input)) >= 0) {
        while (line_len > 0 && (line[line_len - 1] == '\n' ||
                                line[line_len - 1] == '\r')) line_len--;
        size_t message_len = 0;
        uint8_t *message = decode_hex(line, (size_t)line_len, &message_len);
        if (!message) { puts("ERR"); continue; }
        if (strcmp(argv[1], "iron") == 0) parse_iron(message, message_len);
        else parse_pico(message, message_len);
        free(message);
    }
    free(line);
    fclose(input);
    iron_runtime_shutdown();
    return 0;
}
