#ifndef IRON_TLS_H
#define IRON_TLS_H

#include <stdbool.h>
#include <stdint.h>

#include "iron_net.h"

typedef struct Iron_TlsStream Iron_TlsStream;
typedef struct Iron_TlsServerContext Iron_TlsServerContext;

typedef struct {
    Iron_TlsStream *stream;
    Iron_NetError error;
} Iron_TlsStreamResult;

typedef struct {
    Iron_TlsServerContext *context;
    Iron_NetError error;
} Iron_TlsContextResult;

/* Low-level TLS results contain handles plus numeric NetError values and own
 * no strings. Close/free a successful handle; no result release is needed.
 * The higher-level HTTPS models in iron_http.h do own diagnostic strings and
 * provide matching Iron_*_release helpers. */

/* Wrap an already-connected nonblocking TCP socket. `ca_file == ""` uses the
 * platform/OpenSSL default trust roots. Hostname/IP verification is mandatory
 * unless `insecure` is explicitly true. */
Iron_TlsStreamResult iron_tls_client_connect(Iron_TcpSocket socket,
                                              Iron_String host,
                                              Iron_String ca_file,
                                              bool insecure,
                                              Iron_Deadline deadline);

Iron_TlsContextResult iron_tls_server_context_new(Iron_String certificate_file,
                                                   Iron_String private_key_file);
Iron_TlsStreamResult iron_tls_server_accept(Iron_TlsServerContext *context,
                                             Iron_TcpSocket socket,
                                             Iron_Deadline deadline);
bool iron_tls_server_context_retain(Iron_TlsServerContext *context);

Iron_Result_Int_Error iron_tls_read(Iron_TlsStream *stream, uint8_t *buffer,
                                     int64_t capacity, Iron_Deadline deadline);
Iron_Result_Int_Error iron_tls_write(Iron_TlsStream *stream,
                                      const uint8_t *buffer, int64_t length,
                                      Iron_Deadline deadline);
void iron_tls_stream_close(Iron_TlsStream *stream);
void iron_tls_server_context_free(Iron_TlsServerContext *context);
bool iron_tls_is_available(void);

#endif
