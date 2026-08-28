#include "iron_tls.h"
#include "runtime/iron_errors.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef IRON_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef POLLIN
#define POLLIN 0x0100
#endif
#ifndef POLLOUT
#define POLLOUT 0x0010
#endif
#else
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#endif

struct Iron_TlsStream {
    SSL_CTX *context;
    SSL *ssl;
    int64_t fd;
#ifdef _WIN32
    CRITICAL_SECTION io_lock;
#else
    pthread_mutex_t io_lock;
#endif
    int lock_initialized;
};

struct Iron_TlsServerContext {
    SSL_CTX *context;
    atomic_uint_fast64_t reference_count;
};

static int tls_lock_init(Iron_TlsStream *stream) {
#ifdef _WIN32
    InitializeCriticalSection(&stream->io_lock);
    stream->lock_initialized = 1;
    return 1;
#else
    if (pthread_mutex_init(&stream->io_lock, NULL) != 0) return 0;
    stream->lock_initialized = 1;
    return 1;
#endif
}

static void tls_lock(Iron_TlsStream *stream) {
#ifdef _WIN32
    EnterCriticalSection(&stream->io_lock);
#else
    pthread_mutex_lock(&stream->io_lock);
#endif
}

static void tls_unlock(Iron_TlsStream *stream) {
#ifdef _WIN32
    LeaveCriticalSection(&stream->io_lock);
#else
    pthread_mutex_unlock(&stream->io_lock);
#endif
}

static void tls_lock_destroy(Iron_TlsStream *stream) {
    if (!stream->lock_initialized) return;
#ifdef _WIN32
    DeleteCriticalSection(&stream->io_lock);
#else
    pthread_mutex_destroy(&stream->io_lock);
#endif
    stream->lock_initialized = 0;
}

static Iron_NetError tls_error(int64_t code) {
    Iron_NetError out;
    out.code = code;
    return out;
}

static int tls_wait(int64_t fd, int want_write, Iron_Deadline deadline) {
    for (;;) {
        int remaining = Iron_deadline_remaining_ms(deadline);
        if (remaining <= 0) return IRON_ERR_NET_TIMEOUT;
#ifdef _WIN32
        WSAPOLLFD descriptor;
        descriptor.fd = (SOCKET)fd;
        descriptor.events = want_write ? POLLOUT : POLLIN;
        descriptor.revents = 0;
        int result = WSAPoll(&descriptor, 1, remaining);
        if (result == SOCKET_ERROR) return IRON_ERR_TLS_IO;
#else
        struct pollfd descriptor;
        descriptor.fd = (int)fd;
        descriptor.events = want_write ? POLLOUT : POLLIN;
        descriptor.revents = 0;
        int result = poll(&descriptor, 1, remaining);
        if (result < 0) {
            if (errno == EINTR) continue;
            return IRON_ERR_TLS_IO;
        }
#endif
        if (result == 0) return IRON_ERR_NET_TIMEOUT;
        return 0;
    }
}

static int tls_handshake(Iron_TlsStream *stream, int server,
                         Iron_Deadline deadline) {
    for (;;) {
        ERR_clear_error();
        int result = server ? SSL_accept(stream->ssl) : SSL_connect(stream->ssl);
        if (result == 1) return 0;
        int error = SSL_get_error(stream->ssl, result);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            int wait_error = tls_wait(stream->fd,
                                      error == SSL_ERROR_WANT_WRITE, deadline);
            if (wait_error) return wait_error;
            continue;
        }
        if (error == SSL_ERROR_ZERO_RETURN) return IRON_ERR_TLS_CLOSED;
        return IRON_ERR_TLS_HANDSHAKE;
    }
}

static int string_to_cstr(Iron_String input, char **output) {
    size_t length = iron_string_byte_len(&input);
    const char *bytes = iron_string_cstr(&input);
    if (length == 0 || memchr(bytes, '\0', length) != NULL) return 0;
    char *copy = (char *)malloc(length + 1);
    if (!copy) return -1;
    memcpy(copy, bytes, length);
    copy[length] = '\0';
    *output = copy;
    return 1;
}

Iron_TlsStreamResult iron_tls_client_connect(Iron_TcpSocket socket,
                                              Iron_String host,
                                              Iron_String ca_file,
                                              bool insecure,
                                              Iron_Deadline deadline) {
    Iron_TlsStreamResult out = { NULL, tls_error(0) };
    char *host_text = NULL;
    int host_status = string_to_cstr(host, &host_text);
    if (socket.fd < 0 || host_status == 0) {
        out.error = tls_error(IRON_ERR_TLS_INVALID_ARGUMENT);
        return out;
    }
    if (host_status < 0) {
        out.error = tls_error(IRON_ERR_NET_NO_MEMORY);
        return out;
    }

    SSL_CTX *context = SSL_CTX_new(TLS_client_method());
    if (!context) {
        free(host_text);
        out.error = tls_error(IRON_ERR_TLS_CONTEXT);
        return out;
    }
    if (SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1) {
        free(host_text);
        SSL_CTX_free(context);
        out.error = tls_error(IRON_ERR_TLS_CONTEXT);
        return out;
    }
    SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(context, SSL_MODE_ENABLE_PARTIAL_WRITE |
                              SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (!insecure) {
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
        size_t ca_length = iron_string_byte_len(&ca_file);
        if (ca_length > 0) {
            char *ca_text = NULL;
            int ca_status = string_to_cstr(ca_file, &ca_text);
            if (ca_status <= 0 || SSL_CTX_load_verify_locations(
                    context, ca_text, NULL) != 1) {
                free(ca_text);
                free(host_text);
                SSL_CTX_free(context);
                out.error = tls_error(ca_status < 0 ? IRON_ERR_NET_NO_MEMORY
                                                     : IRON_ERR_TLS_TRUST_STORE);
                return out;
            }
            free(ca_text);
        } else if (SSL_CTX_set_default_verify_paths(context) != 1) {
            free(host_text);
            SSL_CTX_free(context);
            out.error = tls_error(IRON_ERR_TLS_TRUST_STORE);
            return out;
        }
    } else {
        SSL_CTX_set_verify(context, SSL_VERIFY_NONE, NULL);
    }

    SSL *ssl = SSL_new(context);
    Iron_TlsStream *stream = (Iron_TlsStream *)calloc(1, sizeof(*stream));
    if (!ssl || !stream) {
        SSL_free(ssl);
        SSL_CTX_free(context);
        free(stream);
        free(host_text);
        out.error = tls_error(IRON_ERR_NET_NO_MEMORY);
        return out;
    }
    stream->context = context;
    stream->ssl = ssl;
    stream->fd = socket.fd;
    if (!tls_lock_init(stream)) {
        SSL_free(ssl);
        SSL_CTX_free(context);
        free(stream);
        free(host_text);
        out.error = tls_error(IRON_ERR_NET_NO_MEMORY);
        return out;
    }
    if (SSL_set_fd(ssl, (int)socket.fd) != 1) {
        iron_tls_stream_close(stream);
        free(host_text);
        out.error = tls_error(IRON_ERR_TLS_CONTEXT);
        return out;
    }

    if (!insecure) {
        unsigned char raw[16];
        X509_VERIFY_PARAM *verify = SSL_get0_param(ssl);
        if (inet_pton(AF_INET, host_text, raw) == 1 ||
            inet_pton(AF_INET6, host_text, raw) == 1) {
            if (X509_VERIFY_PARAM_set1_ip_asc(verify, host_text) != 1) {
                iron_tls_stream_close(stream);
                free(host_text);
                out.error = tls_error(IRON_ERR_TLS_VERIFY);
                return out;
            }
        } else {
            if (SSL_set1_host(ssl, host_text) != 1 ||
                SSL_set_tlsext_host_name(ssl, host_text) != 1) {
                iron_tls_stream_close(stream);
                free(host_text);
                out.error = tls_error(IRON_ERR_TLS_VERIFY);
                return out;
            }
        }
    } else {
        unsigned char raw[16];
        if (inet_pton(AF_INET, host_text, raw) != 1 &&
            inet_pton(AF_INET6, host_text, raw) != 1) {
            if (SSL_set_tlsext_host_name(ssl, host_text) != 1) {
                iron_tls_stream_close(stream);
                free(host_text);
                out.error = tls_error(IRON_ERR_TLS_CONTEXT);
                return out;
            }
        }
    }
    free(host_text);

    int error = tls_handshake(stream, 0, deadline);
    if (error == IRON_ERR_TLS_HANDSHAKE && !insecure &&
        SSL_get_verify_result(ssl) != X509_V_OK) {
        error = IRON_ERR_TLS_VERIFY;
    }
    if (!error && !insecure && SSL_get_verify_result(ssl) != X509_V_OK) {
        error = IRON_ERR_TLS_VERIFY;
    }
    if (error) {
        iron_tls_stream_close(stream);
        out.error = tls_error(error);
        return out;
    }
    out.stream = stream;
    return out;
}

Iron_TlsContextResult iron_tls_server_context_new(Iron_String certificate_file,
                                                   Iron_String private_key_file) {
    Iron_TlsContextResult out = { NULL, tls_error(0) };
    char *certificate = NULL;
    char *private_key = NULL;
    int cert_status = string_to_cstr(certificate_file, &certificate);
    int key_status = string_to_cstr(private_key_file, &private_key);
    if (cert_status <= 0 || key_status <= 0) {
        free(certificate);
        free(private_key);
        out.error = tls_error(cert_status < 0 || key_status < 0
            ? IRON_ERR_NET_NO_MEMORY : IRON_ERR_TLS_INVALID_ARGUMENT);
        return out;
    }
    SSL_CTX *ssl_context = SSL_CTX_new(TLS_server_method());
    Iron_TlsServerContext *context =
        (Iron_TlsServerContext *)calloc(1, sizeof(*context));
    if (!ssl_context || !context) {
        SSL_CTX_free(ssl_context);
        free(context);
        free(certificate);
        free(private_key);
        out.error = tls_error(IRON_ERR_NET_NO_MEMORY);
        return out;
    }
    context->context = ssl_context;
    atomic_init(&context->reference_count, 1);
    if (SSL_CTX_set_min_proto_version(ssl_context, TLS1_2_VERSION) != 1) {
        free(certificate);
        free(private_key);
        iron_tls_server_context_free(context);
        out.error = tls_error(IRON_ERR_TLS_CONTEXT);
        return out;
    }
    SSL_CTX_set_options(ssl_context, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ssl_context, SSL_MODE_ENABLE_PARTIAL_WRITE |
                                  SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (SSL_CTX_use_certificate_chain_file(ssl_context, certificate) != 1) {
        out.error = tls_error(IRON_ERR_TLS_CERTIFICATE);
    } else if (SSL_CTX_use_PrivateKey_file(ssl_context, private_key,
                                           SSL_FILETYPE_PEM) != 1 ||
               SSL_CTX_check_private_key(ssl_context) != 1) {
        out.error = tls_error(IRON_ERR_TLS_PRIVATE_KEY);
    }
    free(certificate);
    free(private_key);
    if (out.error.code != 0) {
        iron_tls_server_context_free(context);
        return out;
    }
    out.context = context;
    return out;
}

Iron_TlsStreamResult iron_tls_server_accept(Iron_TlsServerContext *context,
                                             Iron_TcpSocket socket,
                                             Iron_Deadline deadline) {
    Iron_TlsStreamResult out = { NULL, tls_error(0) };
    if (!context || !context->context || socket.fd < 0) {
        out.error = tls_error(IRON_ERR_TLS_INVALID_ARGUMENT);
        return out;
    }
    Iron_TlsStream *stream = (Iron_TlsStream *)calloc(1, sizeof(*stream));
    SSL *ssl = SSL_new(context->context);
    if (!stream || !ssl) {
        free(stream);
        SSL_free(ssl);
        out.error = tls_error(IRON_ERR_NET_NO_MEMORY);
        return out;
    }
    stream->ssl = ssl;
    stream->fd = socket.fd;
    if (!tls_lock_init(stream)) {
        free(stream);
        SSL_free(ssl);
        out.error = tls_error(IRON_ERR_NET_NO_MEMORY);
        return out;
    }
    if (SSL_set_fd(ssl, (int)socket.fd) != 1) {
        iron_tls_stream_close(stream);
        out.error = tls_error(IRON_ERR_TLS_CONTEXT);
        return out;
    }
    int error = tls_handshake(stream, 1, deadline);
    if (error) {
        iron_tls_stream_close(stream);
        out.error = tls_error(error);
        return out;
    }
    out.stream = stream;
    return out;
}

Iron_Result_Int_Error iron_tls_read(Iron_TlsStream *stream, uint8_t *buffer,
                                     int64_t capacity, Iron_Deadline deadline) {
    Iron_Result_Int_Error out = { 0, tls_error(0) };
    if (!stream || !stream->ssl || !buffer || capacity <= 0 || capacity > INT_MAX) {
        out.v1 = tls_error(IRON_ERR_TLS_INVALID_ARGUMENT);
        return out;
    }
    for (;;) {
        size_t count = 0;
        tls_lock(stream);
        ERR_clear_error();
        int result = SSL_read_ex(stream->ssl, buffer, (size_t)capacity, &count);
        if (result == 1) {
            tls_unlock(stream);
            out.v0 = (int64_t)count;
            return out;
        }
        int error = SSL_get_error(stream->ssl, result);
        tls_unlock(stream);
        if (error == SSL_ERROR_ZERO_RETURN) return out;
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            int wait_error = tls_wait(stream->fd,
                                      error == SSL_ERROR_WANT_WRITE, deadline);
            if (wait_error) out.v1 = tls_error(wait_error);
            if (wait_error) return out;
            continue;
        }
        out.v1 = tls_error(IRON_ERR_TLS_IO);
        return out;
    }
}

Iron_Result_Int_Error iron_tls_write(Iron_TlsStream *stream,
                                      const uint8_t *buffer, int64_t length,
                                      Iron_Deadline deadline) {
    Iron_Result_Int_Error out = { 0, tls_error(0) };
    if (!stream || !stream->ssl || (!buffer && length != 0) ||
        length < 0 || length > INT_MAX) {
        out.v1 = tls_error(IRON_ERR_TLS_INVALID_ARGUMENT);
        return out;
    }
    if (length == 0) return out;
    for (;;) {
        size_t count = 0;
        tls_lock(stream);
        ERR_clear_error();
        int result = SSL_write_ex(stream->ssl, buffer, (size_t)length, &count);
        if (result == 1) {
            tls_unlock(stream);
            out.v0 = (int64_t)count;
            return out;
        }
        int error = SSL_get_error(stream->ssl, result);
        tls_unlock(stream);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            int wait_error = tls_wait(stream->fd,
                                      error == SSL_ERROR_WANT_WRITE, deadline);
            if (wait_error) out.v1 = tls_error(wait_error);
            if (wait_error) return out;
            continue;
        }
        if (error == SSL_ERROR_ZERO_RETURN) {
            out.v1 = tls_error(IRON_ERR_TLS_CLOSED);
        } else {
            out.v1 = tls_error(IRON_ERR_TLS_IO);
        }
        return out;
    }
}

void iron_tls_stream_close(Iron_TlsStream *stream) {
    if (!stream) return;
    if (stream->ssl) {
        SSL_shutdown(stream->ssl);
        SSL_free(stream->ssl);
    }
    if (stream->context) SSL_CTX_free(stream->context);
    tls_lock_destroy(stream);
    free(stream);
}

void iron_tls_server_context_free(Iron_TlsServerContext *context) {
    if (!context) return;
    if (atomic_fetch_sub_explicit(&context->reference_count, 1,
                                  memory_order_acq_rel) != 1) return;
    SSL_CTX_free(context->context);
    free(context);
}

bool iron_tls_server_context_retain(Iron_TlsServerContext *context) {
    if (!context || !context->context) return false;
    atomic_fetch_add_explicit(&context->reference_count, 1,
                              memory_order_relaxed);
    return true;
}

bool iron_tls_is_available(void) { return true; }

#else

struct Iron_TlsStream { int unused; };
struct Iron_TlsServerContext { int unused; };

static Iron_NetError unavailable(void) {
    Iron_NetError error;
    error.code = IRON_ERR_TLS_UNAVAILABLE;
    return error;
}

Iron_TlsStreamResult iron_tls_client_connect(Iron_TcpSocket socket,
                                              Iron_String host,
                                              Iron_String ca_file,
                                              bool insecure,
                                              Iron_Deadline deadline) {
    (void)socket; (void)host; (void)ca_file; (void)insecure; (void)deadline;
    Iron_TlsStreamResult out = { NULL, unavailable() };
    return out;
}

Iron_TlsContextResult iron_tls_server_context_new(Iron_String certificate_file,
                                                   Iron_String private_key_file) {
    (void)certificate_file; (void)private_key_file;
    Iron_TlsContextResult out = { NULL, unavailable() };
    return out;
}

Iron_TlsStreamResult iron_tls_server_accept(Iron_TlsServerContext *context,
                                             Iron_TcpSocket socket,
                                             Iron_Deadline deadline) {
    (void)context; (void)socket; (void)deadline;
    Iron_TlsStreamResult out = { NULL, unavailable() };
    return out;
}

Iron_Result_Int_Error iron_tls_read(Iron_TlsStream *stream, uint8_t *buffer,
                                     int64_t capacity, Iron_Deadline deadline) {
    (void)stream; (void)buffer; (void)capacity; (void)deadline;
    Iron_Result_Int_Error out = { 0, unavailable() };
    return out;
}

Iron_Result_Int_Error iron_tls_write(Iron_TlsStream *stream,
                                      const uint8_t *buffer, int64_t length,
                                      Iron_Deadline deadline) {
    (void)stream; (void)buffer; (void)length; (void)deadline;
    Iron_Result_Int_Error out = { 0, unavailable() };
    return out;
}

void iron_tls_stream_close(Iron_TlsStream *stream) { (void)stream; }
void iron_tls_server_context_free(Iron_TlsServerContext *context) { (void)context; }
bool iron_tls_server_context_retain(Iron_TlsServerContext *context) {
    (void)context;
    return false;
}
bool iron_tls_is_available(void) { return false; }

#endif
