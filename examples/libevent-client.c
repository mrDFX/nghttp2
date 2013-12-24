/*
 * nghttp2 - HTTP/2.0 C Library
 *
 * Copyright (c) 2013 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <sys/types.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <err.h>
#include <signal.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <event.h>
#include <event2/event.h>
#include <event2/bufferevent_ssl.h>
#include <event2/dns.h>

#include <nghttp2/nghttp2.h>

#include "http-parser/http_parser.h"

#define ARRLEN(x) (sizeof(x)/sizeof(x[0]))

typedef struct {
  /* The NULL-terminated URI string to retreive. */
  const char *uri;
  /* Parsed result of the |uri| */
  struct http_parser_url *u;
  /* The authroity portion of the |uri|, NULL-terminated */
  char *authority;
  /* The path portion of the |uri|, including query, NULL-terminated */
  char *path;
  /* The length of the |authority| */
  size_t authoritylen;
  /* The length of the |path| */
  size_t pathlen;
  /* The stream ID of this stream */
  int32_t stream_id;
} http2_stream_data;

typedef struct {
  nghttp2_session *session;
  struct evdns_base *dnsbase;
  struct bufferevent *bev;
  http2_stream_data *stream_data;
} http2_session_data;

static http2_stream_data* create_http2_stream_data(const char *uri,
                                                   struct http_parser_url *u)
{
  http2_stream_data *stream_data = malloc(sizeof(http2_stream_data));

  stream_data->uri = uri;
  stream_data->u = u;
  stream_data->stream_id = -1;

  stream_data->authoritylen = u->field_data[UF_HOST].len;
  if(u->field_set & (1 << UF_PORT)) {
    /* MAX 5 digits (max 65535) + 1 ':' + 1 NULL (because of snprintf) */
    size_t extra = 7;
    stream_data->authority = malloc(stream_data->authoritylen + extra);
    stream_data->authoritylen +=
      snprintf(stream_data->authority + u->field_data[UF_HOST].len, extra,
               ":%u", u->port);
  }
  memcpy(stream_data->authority,
         &uri[u->field_data[UF_HOST].off], u->field_data[UF_HOST].len);

  if(u->field_set & (1 << UF_PATH)) {
    stream_data->pathlen = u->field_data[UF_PATH].len;
  }
  if(u->field_set & (1 << UF_QUERY)) {
    /* +1 for '?' character */
    stream_data->pathlen += u->field_data[UF_QUERY].len + 1;
  }
  stream_data->path = malloc(stream_data->pathlen);
  memcpy(stream_data->path,
         &uri[u->field_data[UF_PATH].off], u->field_data[UF_PATH].len);
  memcpy(stream_data->path + u->field_data[UF_PATH].len + 1,
         &uri[u->field_data[UF_QUERY].off], u->field_data[UF_QUERY].len);
  return stream_data;
}

static void delete_http2_stream_data(http2_stream_data *stream_data)
{
  free(stream_data->path);
  free(stream_data->authority);
  free(stream_data);
}

/* Initializes |session_data| */
static http2_session_data *create_http2_session_data(struct event_base *evbase)
{
  http2_session_data *session_data = malloc(sizeof(http2_session_data));

  memset(session_data, 0, sizeof(http2_session_data));
  session_data->dnsbase = evdns_base_new(evbase, 1);
  return session_data;
}

static void delete_http2_session_data(http2_session_data *session_data)
{
  SSL *ssl = bufferevent_openssl_get_ssl(session_data->bev);

  if(ssl) {
    SSL_shutdown(ssl);
  }
  bufferevent_free(session_data->bev);
  session_data->bev = NULL;
  evdns_base_free(session_data->dnsbase, 1);
  session_data->dnsbase = NULL;
  nghttp2_session_del(session_data->session);
  session_data->session = NULL;
  if(session_data->stream_data) {
    delete_http2_stream_data(session_data->stream_data);
    session_data->stream_data = NULL;
  }
}

/* Print HTTP headers to |f|. Please note that this function does not
   take into account that header name and value are sequence of
   octets, therefore they may contain non-printable characters. */
static void print_headers(FILE *f, nghttp2_nv *nva, size_t nvlen)
{
  size_t i;
  for(i = 0; i < nvlen; ++i) {
    fwrite(nva[i].name, nva[i].namelen, 1, stderr);
    fprintf(stderr, ": ");
    fwrite(nva[i].value, nva[i].valuelen, 1, stderr);
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "\n");
}

/* nghttp2_send_callback. Here we transmit the |data|, |length| bytes,
   to the network. Because we are using libevent bufferevent, we just
   write those bytes into bufferevent buffer. */
static ssize_t send_callback(nghttp2_session *session,
                             const uint8_t *data, size_t length,
                             int flags, void *user_data)
{
  http2_session_data *session_data = (http2_session_data*)user_data;
  struct bufferevent *bev = session_data->bev;
  bufferevent_write(bev, data, length);
  return length;
}

/* nghttp2_before_frame_send_callback: Called when nghttp2 library is
   about to send a frame. We use this callback to get stream ID of new
   stream. Since HEADERS in HTTP/2.0 has several roles, we check that
   it is a HTTP request HEADERS. */
static int before_frame_send_callback
(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
  http2_session_data *session_data = (http2_session_data*)user_data;
  http2_stream_data *stream_data;

  if(frame->hd.type == NGHTTP2_HEADERS &&
     frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
    stream_data =
      (http2_stream_data*)nghttp2_session_get_stream_user_data
      (session, frame->hd.stream_id);
    if(stream_data == session_data->stream_data) {
      stream_data->stream_id = frame->hd.stream_id;
    }
  }
  return 0;
}

/* nghttp2_on_frame_recv_callback: Called when nghttp2 library
   received a frame from the remote peer. */
static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data)
{
  http2_session_data *session_data = (http2_session_data*)user_data;
  switch(frame->hd.type) {
  case NGHTTP2_HEADERS:
    if(frame->headers.cat == NGHTTP2_HCAT_RESPONSE &&
       session_data->stream_data->stream_id == frame->hd.stream_id) {
      /* Print response headers for the initiated request. */
      fprintf(stderr, "Response headers:\n");
      print_headers(stderr, frame->headers.nva, frame->headers.nvlen);
    }
    break;
  }
  return 0;
}

/* nghttp2_on_data_chunk_recv_callback: Called when DATA frame is
   received from the remote peer. In this implementation, if the frame
   is meant to the stream we initiated, print the received data in
   stdout, so that the user can redirect its output to the file
   easily. */
static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,
                                       int32_t stream_id,
                                       const uint8_t *data, size_t len,
                                       void *user_data)
{
  http2_session_data *session_data = (http2_session_data*)user_data;
  if(session_data->stream_data->stream_id == stream_id) {
    fwrite(data, len, 1, stdout);
  }
  return 0;
}

/* nghttp2_on_stream_close_callback: Called when a stream is about to
   closed. This example program only deals with 1 HTTP request (1
   stream), if it is closed, we send GOAWAY and tear down the
   session */
static int on_stream_close_callback(nghttp2_session *session,
                                    int32_t stream_id,
                                    nghttp2_error_code error_code,
                                    void *user_data)
{
  http2_session_data *session_data = (http2_session_data*)user_data;
  if(session_data->stream_data->stream_id == stream_id) {
    fprintf(stderr, "Stream %d closed with error_code=%d\n",
            stream_id, error_code);
    nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE, NGHTTP2_NO_ERROR,
                          NULL, 0);
  }
  return 0;
}

/* NPN TLS extension client callback. We check that server advertised
   the HTTP/2.0 protocol the nghttp2 library supports. If not, exit
   the program. */
static int select_next_proto_cb(SSL* ssl,
                                unsigned char **out, unsigned char *outlen,
                                const unsigned char *in, unsigned int inlen,
                                void *arg)
{
  if(nghttp2_select_next_protocol(out, outlen, in, inlen) <= 0) {
    errx(1, "Server did not advertise " NGHTTP2_PROTO_VERSION_ID);
  }
  return SSL_TLSEXT_ERR_OK;
}

/* Create SSL_CTX. */
static SSL_CTX* create_ssl_ctx(void)
{
  SSL_CTX *ssl_ctx;
  ssl_ctx = SSL_CTX_new(SSLv23_client_method());
  if(!ssl_ctx) {
    errx(1, "Could not create SSL/TLS context: %s",
         ERR_error_string(ERR_get_error(), NULL));
  }
  SSL_CTX_set_options(ssl_ctx,
                      SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_COMPRESSION |
                      SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
  SSL_CTX_set_next_proto_select_cb(ssl_ctx, select_next_proto_cb, NULL);
  return ssl_ctx;
}

/* Create SSL object */
static SSL* create_ssl(SSL_CTX *ssl_ctx)
{
  SSL *ssl;
  ssl = SSL_new(ssl_ctx);
  if(!ssl) {
    errx(1, "Could not create SSL/TLS session object: %s",
         ERR_error_string(ERR_get_error(), NULL));
  }
  return ssl;
}

static void initialize_nghttp2_session(http2_session_data *session_data)
{
  nghttp2_session_callbacks callbacks = {0};

  callbacks.send_callback = send_callback;
  callbacks.before_frame_send_callback = before_frame_send_callback;
  callbacks.on_frame_recv_callback = on_frame_recv_callback;
  callbacks.on_data_chunk_recv_callback = on_data_chunk_recv_callback;
  callbacks.on_stream_close_callback = on_stream_close_callback;
  nghttp2_session_client_new(&session_data->session, &callbacks, session_data);
}

static void send_client_connection_header(http2_session_data *session_data)
{
  nghttp2_settings_entry iv[1] = {
    { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 }
  };
  bufferevent_write(session_data->bev,
                    NGHTTP2_CLIENT_CONNECTION_HEADER,
                    NGHTTP2_CLIENT_CONNECTION_HEADER_LEN);
  nghttp2_submit_settings(session_data->session, NGHTTP2_FLAG_NONE,
                          iv, ARRLEN(iv));
}

#define MAKE_NV(NAME, VALUE, VALUELEN)                                  \
  { (uint8_t*)NAME, (uint8_t*)VALUE, sizeof(NAME) - 1, VALUELEN }

#define MAKE_NV2(NAME, VALUE)                                           \
  { (uint8_t*)NAME, (uint8_t*)VALUE, sizeof(NAME) - 1, sizeof(VALUE) - 1 }

/* Send HTTP request to the remote peer */
static void submit_request(http2_session_data *session_data)
{
  http2_stream_data *stream_data = session_data->stream_data;
  const char *uri = stream_data->uri;
  const struct http_parser_url *u = stream_data->u;
  nghttp2_nv hdrs[] = {
    MAKE_NV2(":method", "GET"),
    MAKE_NV(":scheme",
            &uri[u->field_data[UF_SCHEMA].off], u->field_data[UF_SCHEMA].len),
    MAKE_NV(":authority", stream_data->authority, stream_data->authoritylen),
    MAKE_NV(":path", stream_data->path, stream_data->pathlen)
  };
  fprintf(stderr, "Request headers:\n");
  print_headers(stderr, hdrs, ARRLEN(hdrs));
  nghttp2_submit_request(session_data->session, NGHTTP2_PRI_DEFAULT,
                         hdrs, ARRLEN(hdrs), NULL, stream_data);
}

/* Serialize the frame and send (or buffer) the data to
   bufferevent. */
static void session_send(http2_session_data *session_data)
{
  nghttp2_session_send(session_data->session);
}

/* readcb for bufferevent. Here we get the data from the input buffer
   of bufferevent and feed them to nghttp2 library. This may invoke
   nghttp2 callbacks. It may also queues the frame in nghttp2 session
   context. To send them, we call session_send() in the end. */
static void readcb(struct bufferevent *bev, void *ptr)
{
  http2_session_data *session_data = (http2_session_data*)ptr;
  int rv;
  struct evbuffer *input = bufferevent_get_input(bev);
  size_t datalen = evbuffer_get_length(input);
  unsigned char *data = evbuffer_pullup(input, -1);
  rv = nghttp2_session_mem_recv(session_data->session, data, datalen);
  if(rv < 0) {
    warnx("Fatal error: %s", nghttp2_strerror(rv));
    delete_http2_session_data(session_data);
  } else {
    evbuffer_drain(input, rv);
  }
  session_send(session_data);
}

/* writecb for bufferevent. To greaceful shutdown after sending or
   receiving GOAWAY, we check the some conditions on the nghttp2
   library and output buffer of bufferevent. If it indicates we have
   no business to this session, tear down the connection. */
static void writecb(struct bufferevent *bev, void *ptr)
{
  http2_session_data *session_data = (http2_session_data*)ptr;
  if(nghttp2_session_want_read(session_data->session) == 0 &&
     nghttp2_session_want_write(session_data->session) == 0 &&
     evbuffer_get_length(bufferevent_get_output(session_data->bev)) == 0) {
    delete_http2_session_data(session_data);
  }
}

/* eventcb for bufferevent. For the purpose of simplicity and
   readability of the example program, we omitted the certificate and
   peer verification. After SSL/TLS handshake is over, initialize
   nghttp2 library session, and send client connection header. Then
   send HTTP request. */
static void eventcb(struct bufferevent *bev, short events, void *ptr)
{
  http2_session_data *session_data = (http2_session_data*)ptr;
  if(events & BEV_EVENT_CONNECTED) {
    int fd = bufferevent_getfd(bev);
    int val = 1;
    fprintf(stderr, "Connected\n");
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&val, sizeof(val));
    initialize_nghttp2_session(session_data);
    send_client_connection_header(session_data);
    submit_request(session_data);
    session_send(session_data);
    return;
  }
  if(events & BEV_EVENT_EOF) {
    warnx("Disconnected from the remote host");
  } else if(events & BEV_EVENT_ERROR) {
    warnx("Network error");
  } else if(events & BEV_EVENT_TIMEOUT) {
    warnx("Timeout");
  }
  delete_http2_session_data(session_data);
}

/* Start connecting to the remote peer |host:port| */
static void initiate_connection(struct event_base *evbase,
                                SSL_CTX *ssl_ctx,
                                const char *host, uint16_t port,
                                http2_session_data *session_data)
{
  int rv;
  struct bufferevent *bev;
  SSL *ssl;

  ssl = create_ssl(ssl_ctx);
  bev = bufferevent_openssl_socket_new(evbase, -1, ssl,
                                       BUFFEREVENT_SSL_CONNECTING,
                                       BEV_OPT_DEFER_CALLBACKS |
                                       BEV_OPT_CLOSE_ON_FREE);
  bufferevent_setcb(bev, readcb, writecb, eventcb, session_data);
  rv = bufferevent_socket_connect_hostname(bev, session_data->dnsbase,
                                           AF_UNSPEC, host, port);

  if(rv != 0) {
    errx(1, "Could not connect to the remote host %s", host);
  }
  session_data->bev = bev;
}

/* Get resource denoted by the |uri|. The debug and error messages are
   printed in stderr, while the response body is printed in stdout. */
static void run(const char *uri)
{
  struct http_parser_url u;
  char *host;
  uint16_t port;
  int rv;
  SSL_CTX *ssl_ctx;
  struct event_base *evbase;
  http2_session_data *session_data;

  /* Parse the |uri| and stores its components in |u| */
  rv = http_parser_parse_url(uri, strlen(uri), 0, &u);
  if(rv != 0) {
    errx(1, "Could not parse URI %s", uri);
  }
  host = strndup(&uri[u.field_data[UF_HOST].off], u.field_data[UF_HOST].len);
  if(!(u.field_set & (1 << UF_PORT))) {
    port = 443;
  } else {
    port = u.port;
  }

  ssl_ctx = create_ssl_ctx();

  evbase = event_base_new();

  session_data = create_http2_session_data(evbase);
  session_data->stream_data = create_http2_stream_data(uri, &u);

  initiate_connection(evbase, ssl_ctx, host, port, session_data);
  free(host);
  host = NULL;

  event_base_loop(evbase, 0);

  event_base_free(evbase);
  SSL_CTX_free(ssl_ctx);
}

int main(int argc, char **argv)
{
  struct sigaction act;

  if(argc < 2) {
    fprintf(stderr, "Usage: libevent-client HTTPS_URI\n");
    exit(EXIT_FAILURE);
  }

  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &act, NULL);

  SSL_load_error_strings();
  SSL_library_init();

  run(argv[1]);
  return 0;
}