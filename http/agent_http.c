#define _POSIX_C_SOURCE 200112L

#include "agent_http.h"
#include "agent_conf.h"
#include "agent_tlsf.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netdb.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef AGENT_HTTP_WITH_OPENSSL
#define AGENT_HTTP_WITH_OPENSSL 1
#endif

#if AGENT_HTTP_WITH_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#define AGENT_HTTP_HOST_MAX      256
#define AGENT_HTTP_SCHEME_MAX    8
#define AGENT_HTTP_READ_CHUNK    4096
#define AGENT_HTTP_HEADER_LIMIT  (64 * 1024)

typedef int agent_socket_t;
#define AGENT_INVALID_SOCKET (-1)

static bool agent_socket_is_valid(agent_socket_t fd)
{
    return fd >= 0;
}

static int agent_socket_last_error(void)
{
    return errno;
}

static const char *agent_socket_error_text(int err)
{
    return strerror(err);
}

static void agent_socket_close(agent_socket_t fd)
{
    if(!agent_socket_is_valid(fd))
    {
        return;
    }
    close(fd);
}

typedef struct
{
    char scheme[AGENT_HTTP_SCHEME_MAX];
    char host[AGENT_HTTP_HOST_MAX];
    int port;
    bool tls;
    char *path;
} agent_url_t;

typedef struct
{
    char *data;
    size_t len;
    size_t cap;
} byte_buf_t;

typedef struct
{
    agent_socket_t fd;
    bool tls;
#if AGENT_HTTP_WITH_OPENSSL
    SSL_CTX *ctx;
    SSL *ssl;
#endif
} agent_conn_t;

static int is_url_unreserved(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

char *agent_http_url_escape(const char *value)
{
    static const char hex[] = "0123456789ABCDEF";

    if(!value)
    {
        return NULL;
    }

    size_t len = strlen(value);
    if(len > (((size_t) -1) - 1) / 3)
    {
        return NULL;
    }

    char *out = agent_malloc(len * 3 + 1);
    if(!out)
    {
        return NULL;
    }

    size_t off = 0;
    for(size_t i = 0; i < len; i++)
    {
        unsigned char ch = (unsigned char)value[i];
        if(is_url_unreserved(ch))
        {
            out[off++] = (char)ch;
        }
        else
        {
            out[off++] = '%';
            out[off++] = hex[(ch >> 4) & 0x0f];
            out[off++] = hex[ch & 0x0f];
        }
    }
    out[off] = '\0';
    return out;
}

static void agent_url_free(agent_url_t *url)
{
    if(!url)
    {
        return;
    }
    agent_free(url->path);
    url->path = NULL;
}

static int parse_port(const char *text, size_t len)
{
    int port = 0;
    if(len == 0 || len > 5)
    {
        return -1;
    }

    for(size_t i = 0; i < len; i++)
    {
        if(text[i] < '0' || text[i] > '9')
        {
            return -1;
        }
        port = port * 10 + (text[i] - '0');
        if(port > 65535)
        {
            return -1;
        }
    }
    return port > 0 ? port : -1;
}

static int parse_url(const char *url_text, agent_url_t *out)
{
    memset(out, 0, sizeof(*out));

    const char *scheme_end = strstr(url_text, "://");
    if(!scheme_end)
    {
        return -1;
    }

    size_t scheme_len = (size_t)(scheme_end - url_text);
    if(scheme_len == 0 || scheme_len >= sizeof(out->scheme))
    {
        return -1;
    }

    memcpy(out->scheme, url_text, scheme_len);
    out->scheme[scheme_len] = '\0';
    if(strcmp(out->scheme, "http") == 0)
    {
        out->tls = false;
        out->port = 80;
    }
    else if(strcmp(out->scheme, "https") == 0)
    {
        out->tls = true;
        out->port = 443;
    }
    else
    {
        return -1;
    }

    const char *authority = scheme_end + 3;
    const char *path = strchr(authority, '/');
    const char *query = strchr(authority, '?');
    const char *path_start = NULL;
    if(path && query)
    {
        path_start = path < query ? path : query;
    }
    else
    {
        path_start = path ? path : query;
    }

    const char *authority_end = path_start ? path_start : url_text + strlen(url_text);
    if(authority_end == authority)
    {
        return -1;
    }

    const char *host_start = authority;
    const char *host_end = authority_end;
    const char *port_start = NULL;
    if(*host_start == '[')
    {
        host_start++;
        host_end = strchr(host_start, ']');
        if(!host_end || host_end > authority_end)
        {
            return -1;
        }
        if(host_end + 1 < authority_end && host_end[1] == ':')
        {
            port_start = host_end + 2;
        }
        else if(host_end + 1 != authority_end)
        {
            return -1;
        }
    }
    else
    {
        const char *colon = memchr(authority, ':', (size_t)(authority_end - authority));
        if(colon)
        {
            host_end = colon;
            port_start = colon + 1;
        }
    }

    size_t host_len = (size_t)(host_end - host_start);
    if(host_len == 0 || host_len >= sizeof(out->host))
    {
        return -1;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if(port_start)
    {
        int port = parse_port(port_start, (size_t)(authority_end - port_start));
        if(port < 0)
        {
            return -1;
        }
        out->port = port;
    }

    if(path_start)
    {
        out->path = agent_malloc(strlen(path_start) + 2);
        if(!out->path)
        {
            return -1;
        }
        if(*path_start == '?')
        {
            out->path[0] = '/';
            strcpy(out->path + 1, path_start);
        }
        else
        {
            strcpy(out->path, path_start);
        }
    }
    else
    {
        out->path = agent_malloc(2);
        if(!out->path)
        {
            return -1;
        }
        strcpy(out->path, "/");
    }

    return 0;
}

static int byte_buf_init(byte_buf_t *buf, size_t initial_cap)
{
    buf->data = agent_malloc(initial_cap);
    if(!buf->data)
    {
        return -1;
    }
    buf->len = 0;
    buf->cap = initial_cap;
    return 0;
}

static void byte_buf_free(byte_buf_t *buf)
{
    agent_free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static int byte_buf_append(byte_buf_t *buf, const void *data, size_t len)
{
    if(len == 0)
    {
        return 0;
    }

    if(buf->len > ((size_t) -1) - len - 1)
    {
        return -1;
    }

    size_t need = buf->len + len + 1;
    if(need > buf->cap)
    {
        size_t new_cap = buf->cap;
        while(new_cap < need)
        {
            if(new_cap > ((size_t) -1) / 2)
            {
                return -1;
            }
            new_cap *= 2;
        }

        char *tmp = agent_realloc(buf->data, new_cap);
        if(!tmp)
        {
            return -1;
        }
        buf->data = tmp;
        buf->cap = new_cap;
    }

    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return 0;
}

static int wait_fd(agent_socket_t fd, short events, long timeout_sec)
{
    int timeout_ms;
    if(timeout_sec <= 0)
    {
        timeout_ms = 30000;
    }
    else if(timeout_sec > (long)(INT_MAX / 1000))
    {
        timeout_ms = INT_MAX;
    }
    else
    {
        timeout_ms = (int)timeout_sec * 1000;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;

    for(;;)
    {
        int rc = poll(&pfd, 1, timeout_ms);
        if(rc < 0 && errno == EINTR)
        {
            continue;
        }
        return rc;
    }
}

static int set_socket_timeouts(agent_socket_t fd, long timeout_sec)
{
    struct timeval tv;
    tv.tv_sec = timeout_sec > 0 ? timeout_sec : 30;
    tv.tv_usec = 0;

    if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
    {
        return -1;
    }
    if(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
    {
        return -1;
    }
    return 0;
}

static agent_socket_t connect_tcp(const char *host, int port, long timeout_sec)
{
    char port_buf[16];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    agent_socket_t fd = AGENT_INVALID_SOCKET;

    snprintf(port_buf, sizeof(port_buf), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    int gai = getaddrinfo(host, port_buf, &hints, &res);
    if(gai != 0)
    {
        AGENT_LOG("HTTP: getaddrinfo(%s) failed: %s", host, gai_strerror(gai));
        return -1;
    }

    for(struct addrinfo *ai = res; ai; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if(!agent_socket_is_valid(fd))
        {
            int serr = agent_socket_last_error();
            AGENT_LOG("HTTP: socket failed: errno=%d (%s)",
                      serr, agent_socket_error_text(serr));
            continue;
        }

        if(set_socket_timeouts(fd, timeout_sec) != 0)
        {
            int serr = agent_socket_last_error();
            AGENT_LOG("HTTP: setsockopt timeout failed: errno=%d (%s)",
                      serr, agent_socket_error_text(serr));
            agent_socket_close(fd);
            fd = AGENT_INVALID_SOCKET;
            continue;
        }

        if(connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
        {
            AGENT_LOG("HTTP: TCP connected to %s:%d", host, port);
            break;
        }

        {
            int serr = agent_socket_last_error();
            AGENT_LOG("HTTP: connect attempt to %s:%d failed: errno=%d (%s)",
                      host, port, serr, agent_socket_error_text(serr));
        }
        agent_socket_close(fd);
        fd = AGENT_INVALID_SOCKET;
    }

    freeaddrinfo(res);
    return fd;
}

static void conn_close(agent_conn_t *conn)
{
#if AGENT_HTTP_WITH_OPENSSL
    if(conn->ssl)
    {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
    if(conn->ctx)
    {
        SSL_CTX_free(conn->ctx);
        conn->ctx = NULL;
    }
#endif
    if(agent_socket_is_valid(conn->fd))
    {
        agent_socket_close(conn->fd);
        conn->fd = AGENT_INVALID_SOCKET;
    }
}

#if AGENT_HTTP_WITH_OPENSSL
static int conn_init_tls(agent_conn_t *conn, const char *host)
{
    SSL_library_init();
    SSL_load_error_strings();
    AGENT_LOG("HTTP: starting TLS handshake for host %s", host ? host : "");

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    conn->ctx = SSL_CTX_new(TLS_client_method());
#else
    conn->ctx = SSL_CTX_new(SSLv23_client_method());
#endif
    if(!conn->ctx)
    {
        AGENT_LOG("HTTP: SSL_CTX_new failed");
        return -1;
    }

    /*
     * 按当前产品要求维持原行为：不校验服务端证书。
     *
     * 这里仍然使用 TLS 建立加密通道，并设置 SNI，但不加载 CA、不中断
     * 自签名证书或证书链不完整的连接，避免改变现有设备环境里的联通性。
     */
    AGENT_LOG("HTTP: TLS certificate verification disabled");
    SSL_CTX_set_verify(conn->ctx, SSL_VERIFY_NONE, NULL);

    conn->ssl = SSL_new(conn->ctx);
    if(!conn->ssl)
    {
        AGENT_LOG("HTTP: SSL_new failed");
        return -1;
    }

    SSL_set_fd(conn->ssl, (int)conn->fd);

    SSL_set_tlsext_host_name(conn->ssl, host);

    if(SSL_connect(conn->ssl) != 1)
    {
        unsigned long ssl_err = ERR_get_error();
        AGENT_LOG("HTTP: TLS handshake failed: ssl_err=%lu (%s)",
                  ssl_err, ssl_err ? ERR_error_string(ssl_err, NULL) : "no OpenSSL error");
        return -1;
    }

    conn->tls = true;
    AGENT_LOG("HTTP: TLS handshake ok for host %s", host ? host : "");
    return 0;
}
#endif

static int conn_open(agent_conn_t *conn, const agent_url_t *url, long timeout_sec)
{
    memset(conn, 0, sizeof(*conn));
    conn->fd = AGENT_INVALID_SOCKET;

    conn->fd = connect_tcp(url->host, url->port, timeout_sec);
    if(!agent_socket_is_valid(conn->fd))
    {
        AGENT_LOG("HTTP: connect failed: %s:%d", url->host, url->port);
        return -1;
    }

    if(url->tls)
    {
#if AGENT_HTTP_WITH_OPENSSL
        if(conn_init_tls(conn, url->host) != 0)
        {
            return -1;
        }
#else
        AGENT_LOG("HTTP: HTTPS requires OpenSSL or a platform TLS backend");
        return -1;
#endif
    }

    return 0;
}

static int conn_write_all(agent_conn_t *conn, const void *data, size_t len,
                          long timeout_sec)
{
    const unsigned char *ptr = (const unsigned char *)data;
    size_t sent = 0;

    while(sent < len)
    {
        int rc;
        size_t remain = len - sent;
#if AGENT_HTTP_WITH_OPENSSL
        if(conn->tls)
        {
            rc = SSL_write(conn->ssl, ptr + sent,
                           remain > INT_MAX ? INT_MAX : (int)remain);
            if(rc <= 0)
            {
                int ssl_err = SSL_get_error(conn->ssl, rc);
                if(ssl_err == SSL_ERROR_WANT_READ)
                {
                    if(wait_fd(conn->fd, POLLIN, timeout_sec) > 0)
                    {
                        continue;
                    }
                }
                else if(ssl_err == SSL_ERROR_WANT_WRITE)
                {
                    if(wait_fd(conn->fd, POLLOUT, timeout_sec) > 0)
                    {
                        continue;
                    }
                }
                return -1;
            }
        }
        else
#endif
        {
            ssize_t n = send(conn->fd, ptr + sent, remain, 0);
            if(n < 0 && errno == EINTR)
            {
                continue;
            }
            if(n < 0)
            {
                return -1;
            }
            rc = (int)n;
        }
        sent += (size_t)rc;
    }

    return 0;
}

static int conn_read(agent_conn_t *conn, void *data, size_t len, long timeout_sec)
{
#if AGENT_HTTP_WITH_OPENSSL
    if(conn->tls)
    {
        for(;;)
        {
            errno = 0;
            int rc = SSL_read(conn->ssl, data, len > INT_MAX ? INT_MAX : (int)len);
            if(rc > 0)
            {
                return rc;
            }

            int ssl_err = SSL_get_error(conn->ssl, rc);
            if(ssl_err == SSL_ERROR_ZERO_RETURN)
            {
                return 0;
            }
            if(ssl_err == SSL_ERROR_SYSCALL && errno == 0)
            {
                return 0;
            }
            if(ssl_err == SSL_ERROR_WANT_READ)
            {
                int wait_rc = wait_fd(conn->fd, POLLIN, timeout_sec);
                if(wait_rc > 0)
                {
                    continue;
                }
                if(wait_rc == 0)
                {
                    errno = ETIMEDOUT;
                }
            }
            else if(ssl_err == SSL_ERROR_WANT_WRITE)
            {
                int wait_rc = wait_fd(conn->fd, POLLOUT, timeout_sec);
                if(wait_rc > 0)
                {
                    continue;
                }
                if(wait_rc == 0)
                {
                    errno = ETIMEDOUT;
                }
            }
            else if(errno == 0)
            {
                errno = EIO;
            }
            return -1;
        }
    }
#endif

    for(;;)
    {
        ssize_t n = recv(conn->fd, data, len, 0);
        if(n < 0 && errno == EINTR)
        {
            continue;
        }
        return n < 0 ? -1 : (int)n;
    }
}

static int append_fmt(byte_buf_t *buf, const char *fmt, const char *a,
                      const char *b)
{
    int n = snprintf(NULL, 0, fmt, a ? a : "", b ? b : "");
    if(n < 0)
    {
        return -1;
    }

    char *tmp = agent_malloc((size_t)n + 1);
    if(!tmp)
    {
        return -1;
    }
    snprintf(tmp, (size_t)n + 1, fmt, a ? a : "", b ? b : "");
    int rc = byte_buf_append(buf, tmp, (size_t)n);
    agent_free(tmp);
    return rc;
}

static int build_request(const char *method,
                         const agent_url_t *url,
                         const agent_http_header_t *headers,
                         size_t header_count,
                         const char *body,
                         byte_buf_t *out)
{
    if(byte_buf_init(out, 1024) != 0)
    {
        return -1;
    }

    if(append_fmt(out, "%s %s HTTP/1.1\r\n", method, url->path) != 0)
    {
        return -1;
    }

    char host_value[AGENT_HTTP_HOST_MAX + 16];
    bool default_port = (!url->tls && url->port == 80) ||
                        (url->tls && url->port == 443);
    if(default_port)
    {
        snprintf(host_value, sizeof(host_value), "%s", url->host);
    }
    else
    {
        snprintf(host_value, sizeof(host_value), "%s:%d", url->host, url->port);
    }

    const char *user_agent = "User-Agent: CClaw/1.0\r\n";
    const char *accept = "Accept: */*\r\n";
    const char *connection = "Connection: close\r\n";
    if(append_fmt(out, "Host: %s\r\n", host_value, NULL) != 0 ||
            byte_buf_append(out, user_agent, strlen(user_agent)) != 0 ||
            byte_buf_append(out, accept, strlen(accept)) != 0 ||
            byte_buf_append(out, connection, strlen(connection)) != 0)
    {
        return -1;
    }

    for(size_t i = 0; i < header_count; i++)
    {
        if(!headers[i].name || !headers[i].value)
        {
            continue;
        }
        if(append_fmt(out, "%s: %s\r\n", headers[i].name, headers[i].value) != 0)
        {
            return -1;
        }
    }

    if(body)
    {
        char len_buf[32];
        snprintf(len_buf, sizeof(len_buf), "%lu", (unsigned long)strlen(body));
        if(append_fmt(out, "Content-Length: %s\r\n", len_buf, NULL) != 0)
        {
            return -1;
        }
    }

    if(byte_buf_append(out, "\r\n", 2) != 0)
    {
        return -1;
    }
    if(body && byte_buf_append(out, body, strlen(body)) != 0)
    {
        return -1;
    }

    return 0;
}

static char *find_header_end(const char *data, size_t len)
{
    for(size_t i = 0; i + 3 < len; i++)
    {
        if(data[i] == '\r' && data[i + 1] == '\n' &&
                data[i + 2] == '\r' && data[i + 3] == '\n')
        {
            return (char *)data + i;
        }
    }
    return NULL;
}

static int parse_status_code(const char *headers, size_t header_len)
{
    const char *line_end = memchr(headers, '\n', header_len);
    if(!line_end)
    {
        return 0;
    }

    const char *space = memchr(headers, ' ', (size_t)(line_end - headers));
    if(!space || space + 3 >= line_end)
    {
        return 0;
    }
    return atoi(space + 1);
}

static bool header_value_contains(const char *headers,
                                  size_t header_len,
                                  const char *name,
                                  const char *token)
{
    size_t name_len = strlen(name);
    const char *p = headers;
    const char *end = headers + header_len;

    while(p < end)
    {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if(!line_end)
        {
            line_end = end;
        }

        const char *colon = memchr(p, ':', (size_t)(line_end - p));
        if(colon && (size_t)(colon - p) == name_len &&
                strncasecmp(p, name, name_len) == 0)
        {
            const char *value = colon + 1;
            while(value < line_end && isspace((unsigned char)*value))
            {
                value++;
            }

            size_t token_len = strlen(token);
            for(const char *s = value; s + token_len <= line_end; s++)
            {
                if(strncasecmp(s, token, token_len) == 0)
                {
                    return true;
                }
            }
        }

        p = line_end + 1;
    }

    return false;
}

static int header_get_content_length(const char *headers,
                                     size_t header_len,
                                     size_t *out_len)
{
    size_t name_len = strlen("Content-Length");
    const char *p = headers;
    const char *end = headers + header_len;

    if(out_len)
    {
        *out_len = 0;
    }

    while(p < end)
    {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if(!line_end)
        {
            line_end = end;
        }

        const char *colon = memchr(p, ':', (size_t)(line_end - p));
        if(colon && (size_t)(colon - p) == name_len &&
                strncasecmp(p, "Content-Length", name_len) == 0)
        {
            const char *value = colon + 1;
            while(value < line_end && isspace((unsigned char)*value))
            {
                value++;
            }
            if(value >= line_end || !isdigit((unsigned char)*value))
            {
                return -1;
            }

            errno = 0;
            char *num_end = NULL;
            unsigned long long len = strtoull(value, &num_end, 10);
            if(num_end == value || errno == ERANGE || len > (unsigned long long)SIZE_MAX)
            {
                return -1;
            }

            while(num_end < line_end && *num_end != '\r' &&
                    isspace((unsigned char)*num_end))
            {
                num_end++;
            }
            if(num_end < line_end && *num_end != '\r')
            {
                return -1;
            }

            if(out_len)
            {
                *out_len = (size_t)len;
            }
            return 1;
        }

        p = line_end + 1;
    }

    return 0;
}

static int chunked_body_complete(const char *body, size_t body_len, bool *complete)
{
    const char *p = body;
    const char *end = body + body_len;

    if(complete)
    {
        *complete = false;
    }

    for(;;)
    {
        const char *line = p;
        const char *line_end = memchr(line, '\n', (size_t)(end - line));
        if(!line_end)
        {
            return 0;
        }

        errno = 0;
        char *num_end = NULL;
        unsigned long chunk_len = strtoul(line, &num_end, 16);
        if(num_end == line || errno == ERANGE)
        {
            return -1;
        }
        if(num_end > line_end)
        {
            return -1;
        }

        p = line_end + 1;
        if(chunk_len == 0)
        {
            if(complete)
            {
                *complete = true;
            }
            return 0;
        }

        if((unsigned long)(end - p) < chunk_len)
        {
            return 0;
        }
        p += chunk_len;

        if(p == end)
        {
            return 0;
        }
        if(p[0] == '\r')
        {
            if(p + 1 >= end)
            {
                return 0;
            }
            if(p[1] != '\n')
            {
                return -1;
            }
            p += 2;
        }
        else if(p[0] == '\n')
        {
            p++;
        }
        else
        {
            return -1;
        }
    }
}

static int response_has_complete_body(const byte_buf_t *response)
{
    char *header_end = find_header_end(response->data, response->len);
    if(!header_end)
    {
        return 0;
    }

    size_t header_len = (size_t)(header_end - response->data);
    size_t body_offset = header_len + 4;
    size_t body_len = response->len - body_offset;
    int status = parse_status_code(response->data, header_len);

    if((status >= 100 && status < 200) || status == 204 || status == 304)
    {
        return 1;
    }

    if(header_value_contains(response->data, header_len,
                             "Transfer-Encoding", "chunked"))
    {
        bool complete = false;
        int rc = chunked_body_complete(response->data + body_offset,
                                       body_len, &complete);
        if(rc != 0)
        {
            AGENT_LOG("HTTP: malformed chunked response");
            return -1;
        }
        return complete ? 1 : 0;
    }

    size_t content_length = 0;
    int len_rc = header_get_content_length(response->data, header_len,
                                           &content_length);
    if(len_rc < 0)
    {
        AGENT_LOG("HTTP: invalid Content-Length header");
        return -1;
    }
    if(len_rc > 0)
    {
        return body_len >= content_length ? 1 : 0;
    }

    return 0;
}

static int emit_chunked_body(const char *body,
                             size_t body_len,
                             agent_http_body_cb body_cb,
                             void *userp)
{
    const char *p = body;
    const char *end = body + body_len;

    while(p < end)
    {
        char *line_end = NULL;
        unsigned long chunk_len = strtoul(p, &line_end, 16);
        if(line_end == p)
        {
            return -1;
        }

        while(line_end < end && *line_end != '\n')
        {
            line_end++;
        }
        if(line_end >= end)
        {
            return -1;
        }
        p = line_end + 1;

        if(chunk_len == 0)
        {
            return 0;
        }
        if((unsigned long)(end - p) < chunk_len)
        {
            return -1;
        }

        if(body_cb && body_cb(p, (size_t)chunk_len, userp) != 0)
        {
            return -1;
        }

        p += chunk_len;
        if(p + 1 < end && p[0] == '\r' && p[1] == '\n')
        {
            p += 2;
        }
        else if(p < end && p[0] == '\n')
        {
            p += 1;
        }
    }

    return 0;
}

static int emit_response_body(const byte_buf_t *response,
                              int *out_status,
                              agent_http_body_cb body_cb,
                              void *userp)
{
    char *header_end = find_header_end(response->data, response->len);
    if(!header_end)
    {
        AGENT_LOG("HTTP: response missing headers");
        return -1;
    }

    size_t header_len = (size_t)(header_end - response->data);
    if(out_status)
    {
        *out_status = parse_status_code(response->data, header_len);
    }

    const char *body = header_end + 4;
    size_t body_len = response->len - (size_t)(body - response->data);

    if(header_value_contains(response->data, header_len,
                             "Transfer-Encoding", "chunked"))
    {
        return emit_chunked_body(body, body_len, body_cb, userp);
    }

    if(body_cb && body_len > 0)
    {
        return body_cb(body, body_len, userp);
    }
    return 0;
}

int agent_http_request(const char *method,
                       const char *url_text,
                       const agent_http_header_t *headers,
                       size_t header_count,
                       const char *body,
                       long timeout_sec,
                       agent_http_body_cb body_cb,
                       void *userp,
                       int *out_status)
{
    int rc = -1;
    agent_url_t url;
    agent_conn_t conn;
    byte_buf_t request;
    byte_buf_t response;

    memset(&url, 0, sizeof(url));
    memset(&conn, 0, sizeof(conn));
    conn.fd = AGENT_INVALID_SOCKET;
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));

    if(out_status)
    {
        *out_status = 0;
    }
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }
    if(!method || !url_text)
    {
        return -1;
    }

    if(parse_url(url_text, &url) != 0)
    {
        AGENT_LOG("HTTP: invalid URL: %s", url_text);
        goto cleanup;
    }
    AGENT_LOG("HTTP: request begin method=%s scheme=%s host=%s port=%d path=%s timeout=%ld body_bytes=%d",
              method, url.scheme, url.host, url.port, url.path ? url.path : "",
              timeout_sec, body ? (int)strlen(body) : 0);

    if(build_request(method, &url, headers, header_count, body, &request) != 0)
    {
        AGENT_LOG("HTTP: failed to build request");
        goto cleanup;
    }
    AGENT_LOG("HTTP: built request bytes=%u headers=%u",
              (unsigned)request.len, (unsigned)header_count);

    if(byte_buf_init(&response, AGENT_HTTP_READ_CHUNK) != 0)
    {
        AGENT_LOG("HTTP: failed to allocate response buffer (%d bytes)",
                  AGENT_HTTP_READ_CHUNK);
        goto cleanup;
    }

    if(conn_open(&conn, &url, timeout_sec) != 0)
    {
        goto cleanup;
    }

    if(conn_write_all(&conn, request.data, request.len, timeout_sec) != 0)
    {
        AGENT_LOG("HTTP: write failed");
        goto cleanup;
    }

    for(;;)
    {
        char chunk[AGENT_HTTP_READ_CHUNK];
        int n = conn_read(&conn, chunk, sizeof(chunk), timeout_sec);
        if(n < 0)
        {
            AGENT_LOG("HTTP: read failed errno=%d (%s) raw_bytes=%u",
                      errno, strerror(errno), (unsigned)response.len);
            goto cleanup;
        }
        if(n == 0)
        {
            break;
        }
        if(response.len + (size_t)n > AGENT_HTTP_HEADER_LIMIT &&
                !find_header_end(response.data, response.len))
        {
            AGENT_LOG("HTTP: response headers too large");
            goto cleanup;
        }
        if(byte_buf_append(&response, chunk, (size_t)n) != 0)
        {
            goto cleanup;
        }

        int complete = response_has_complete_body(&response);
        if(complete < 0)
        {
            goto cleanup;
        }
        if(complete > 0)
        {
            break;
        }
    }

    if(emit_response_body(&response, out_status, body_cb, userp) != 0)
    {
        AGENT_LOG("HTTP: failed to parse response body/status (raw_bytes=%u)",
                  (unsigned)response.len);
        goto cleanup;
    }

    AGENT_LOG("HTTP: request done status=%d raw_bytes=%u",
              out_status ? *out_status : 0, (unsigned)response.len);
    rc = 0;

cleanup:
    conn_close(&conn);
    byte_buf_free(&response);
    byte_buf_free(&request);
    agent_url_free(&url);
    return rc;
}

/* ================================================================
 * 流式 HTTP API
 *
 * 与 agent_http_request() 的区别：
 * - agent_http_request() 会把整个响应体缓冲在内存里，请求完成后
 *   一次性交给 body_cb 处理；
 * - 流式 API 在 stream_open 阶段只读取并解析 HTTP 响应头，
 *   然后通过反复调用 stream_read 逐块获取响应体。
 *
 * 适用场景：SSE (Server-Sent Events)、大文件下载等需要边收边处理的场景。
 * ================================================================ */

/* 流句柄的内部结构 */
struct agent_http_stream
{
    agent_conn_t conn;          /* 底层连接（含 socket 和 TLS 状态） */
    long timeout_sec;           /* 超时秒数，传递给 conn_read */

    bool chunked;               /* true = 响应使用 chunked 传输编码 */
    bool headers_parsed;        /* 响应头是否已完整解析 */

    /* ── chunked 编码解码状态 ──────────────────────────────────── *
     * chunked 格式：<hex-length>\r\n<data>\r\n
     * chunk_remaining 记录当前 chunk 还剩多少字节未读取。
     * 读完后会自动解析下一个 chunk 的长度头。
     * 0 表示还没有开始读 chunk 数据（需要先解析 chunk 长度行）。*/
    unsigned long chunk_remaining;

    /* 已读到的 chunked 帧尾标记（\r\n）数量，用于跟踪 chunk 边界 */
    bool chunk_need_crlf;

    /* ── 响应头后多读到的 body 字节（pending body） ────────────── *
     *
     * stream_read_headers() 通过 conn_read() 批量读取数据，
     * TLS/socket 可能一次返回"响应头 + 部分响应体"。
     * 超过 \r\n\r\n 的那部分 body 字节保存在此缓冲区中，
     * agent_http_stream_read() 会优先从这里读取，
     * 耗尽后才从 socket/TLS 层读取新数据。
     *
     * 内存由 TLSF 分配器管理，stream_close 时释放。
     */
    char *pending_body;         /* 多读到的 body 数据 */
    size_t pending_body_len;    /* pending_body 的总字节数 */
    size_t pending_body_pos;    /* 当前已消耗的偏移量 */
};

/**
 * 从连接中读取数据，直到找到完整的 HTTP 响应头（\r\n\r\n 结尾）。
 *
 * 返回的 headers_buf 包含从响应起始到 \r\n\r\n 的所有字节。
 * 调用者需要负责释放 headers_buf->data。
 */
static int stream_read_headers(agent_conn_t *conn,
                               long timeout_sec,
                               byte_buf_t *headers_buf)
{
    if(byte_buf_init(headers_buf, AGENT_HTTP_READ_CHUNK) != 0)
    {
        AGENT_LOG("HTTP stream: failed to allocate headers buffer");
        return -1;
    }

    for(;;)
    {
        /* 检查是否已经找到响应头末尾 */
        if(headers_buf->len >= 4 &&
                find_header_end(headers_buf->data, headers_buf->len))
        {
            return 0;
        }

        /* 响应头过大，可能有问题 */
        if(headers_buf->len >= AGENT_HTTP_HEADER_LIMIT)
        {
            AGENT_LOG("HTTP stream: response headers too large (%u bytes)",
                      (unsigned)headers_buf->len);
            return -1;
        }

        char chunk[AGENT_HTTP_READ_CHUNK];
        int n = conn_read(conn, chunk, sizeof(chunk), timeout_sec);
        if(n < 0)
        {
            AGENT_LOG("HTTP stream: read headers failed");
            return -1;
        }
        if(n == 0)
        {
            /* 连接在响应头未完成时就关闭了 */
            AGENT_LOG("HTTP stream: connection closed before headers complete");
            return -1;
        }

        if(byte_buf_append(headers_buf, chunk, (size_t)n) != 0)
        {
            AGENT_LOG("HTTP stream: failed to append headers data (%u bytes)",
                      (unsigned)headers_buf->len);
            return -1;
        }
    }
}

/**
 * 从流中读取原始 HTTP body 字节（统一读取入口）。
 *
 * 优先从 pending_body 缓冲区消耗字节（stream_open 阶段读响应头时
 * 多读到的 body 数据），耗尽后从底层 socket/TLS 连接读取。
 *
 * 这是 HTTP body 层所有读取操作的唯一入口：
 * - chunked 解码器通过它读取帧头（chunk size 行）
 * - chunked 解码器通过它读取帧数据（chunk payload）
 * - 非 chunked 分支通过它直接读 body
 *
 * 调用者**永远不应该**直接使用 conn_read() 读 body 数据，
 * 否则会跳过 pending_body 中的字节，导致 chunked 帧结构被破坏。
 *
 * @param stream  流句柄
 * @param buf     接收缓冲区
 * @param len     要读取的字节数
 * @return >0 实际读取字节数，0 EOF，<0 错误
 */
static int stream_raw_read(agent_http_stream_t *stream, void *buf, size_t len)
{
    /* 优先从 pending body 中读取 */
    if(stream->pending_body && stream->pending_body_pos < stream->pending_body_len)
    {
        size_t avail = stream->pending_body_len - stream->pending_body_pos;
        size_t to_copy = len < avail ? len : avail;
        memcpy(buf, stream->pending_body + stream->pending_body_pos, to_copy);
        stream->pending_body_pos += to_copy;

        /* pending body 已全部消耗，释放缓冲区 */
        if(stream->pending_body_pos >= stream->pending_body_len)
        {
            agent_free(stream->pending_body);
            stream->pending_body = NULL;
            stream->pending_body_len = 0;
            stream->pending_body_pos = 0;
        }

        return (int)to_copy;
    }

    /* pending 耗尽，从 socket/TLS 读取 */
    return conn_read(&stream->conn, buf, len, stream->timeout_sec);
}

/**
 * 从 chunked 数据流中解析下一个 chunk 的长度。
 *
 * chunked 帧格式：
 *   <hex-length>[;ext]\r\n
 *   <data>\r\n
 *
 * 本函数从当前读取位置找到 \r\n，解析前面的十六进制数字作为 chunk 长度。
 * 长度为 0 表示这是最后一个 chunk（流结束）。
 *
 * @param stream  流句柄
 * @return >=0 剩余 chunk 数据字节数，<0 错误
 */
static int stream_parse_chunk_size(agent_http_stream_t *stream)
{
    /*
     * 逐字节读取直到遇到 \n，收集 chunk 长度行。
     * chunk 长度行格式：<hex-number>[;extensions]\r\n
     * 我们只需要解析 ; 前面的十六进制数字。
     */
    char line_buf[64];
    size_t line_len = 0;

    for(;;)
    {
        char c;
        int n = stream_raw_read(stream, &c, 1);
        if(n <= 0)
        {
            AGENT_LOG("HTTP stream: chunk size read failed (stream_raw_read returned %d)", n);
            return -1;
        }

        if(c == '\n')
        {
            break;
        }

        if(line_len < sizeof(line_buf) - 1)
        {
            line_buf[line_len++] = c;
        }
    }

    /* 去掉末尾的 \r */
    while(line_len > 0 && line_buf[line_len - 1] == '\r')
    {
        line_len--;
    }
    line_buf[line_len] = '\0';

    /* 截断到分号前（忽略 chunk extensions） */
    for(size_t i = 0; i < line_len; i++)
    {
        if(line_buf[i] == ';')
        {
            line_buf[i] = '\0';
            break;
        }
    }

    /* 跳过前一个 chunk 的 \r\n 尾部（如果需要） */
    if(stream->chunk_need_crlf)
    {
        stream->chunk_need_crlf = false;
        /* line_buf 此时可能是空行（就是前一个 chunk 的尾部 \r\n） */
        if(line_len == 0)
        {
            /* 再读一行，这次才是真正的 chunk 大小 */
            line_len = 0;
            for(;;)
            {
                char c;
                int rn = stream_raw_read(stream, &c, 1);
                if(rn <= 0)
                {
                    AGENT_LOG("HTTP stream: chunk CRLF skip read failed (stream_raw_read returned %d)", rn);
                    return -1;
                }
                if(c == '\n')
                {
                    break;
                }
                if(line_len < sizeof(line_buf) - 1)
                {
                    line_buf[line_len++] = c;
                }
            }
            while(line_len > 0 && line_buf[line_len - 1] == '\r')
            {
                line_len--;
            }
            line_buf[line_len] = '\0';
        }
    }

    /* 解析十六进制 chunk 长度 */
    char *end = NULL;
    unsigned long size = strtoul(line_buf, &end, 16);
    if(end == line_buf)
    {
        AGENT_LOG("HTTP stream: invalid chunk size line: '%s'", line_buf);
        return -1;
    }

    return (size > (unsigned long)INT_MAX) ? INT_MAX : (int)size;
}

int agent_http_stream_open(const char *method,
                           const char *url_text,
                           const agent_http_header_t *headers,
                           size_t header_count,
                           const char *body,
                           long timeout_sec,
                           agent_http_stream_t **out_stream,
                           int *out_status)
{
    int rc = -1;
    agent_url_t url;
    byte_buf_t request_buf;
    byte_buf_t headers_buf;

    memset(&url, 0, sizeof(url));
    memset(&request_buf, 0, sizeof(request_buf));
    memset(&headers_buf, 0, sizeof(headers_buf));

    if(out_status)
    {
        *out_status = 0;
    }
    if(!out_stream)
    {
        return -1;
    }
    *out_stream = NULL;

    if(agent_tlsf_init() != 0)
    {
        AGENT_LOG("HTTP stream: tlsf init failed");
        return -1;
    }
    if(!method || !url_text)
    {
        AGENT_LOG("HTTP stream: null param (method=%p url=%p)", method, url_text);
        return -1;
    }

    /* 1. 解析 URL */
    if(parse_url(url_text, &url) != 0)
    {
        AGENT_LOG("HTTP stream: invalid URL: %s", url_text);
        goto cleanup;
    }

    /* 2. 构建 HTTP 请求（复用已有的 build_request 函数） */
    if(build_request(method, &url, headers, header_count, body, &request_buf) != 0)
    {
        AGENT_LOG("HTTP stream: failed to build request");
        goto cleanup;
    }

    /* 3. 分配流句柄 */
    agent_http_stream_t *stream = agent_calloc(1, sizeof(*stream));
    if(!stream)
    {
        AGENT_LOG("HTTP stream: failed to allocate stream handle");
        goto cleanup;
    }
    stream->timeout_sec = timeout_sec;
    stream->chunked = false;
    stream->headers_parsed = false;
    stream->chunk_remaining = 0;
    stream->chunk_need_crlf = false;

    /* 4. 建立 TCP + TLS 连接 */
    if(conn_open(&stream->conn, &url, timeout_sec) != 0)
    {
        AGENT_LOG("HTTP stream: connection failed");
        /* conn_open 可能 TCP 已连接但 TLS 握手失败，
         * 此时 stream->conn.fd 已分配但未被释放。
         * 必须调用 conn_close 确保 fd/SSL 资源释放，
         * 否则每次失败都会泄露一个 socket fd。 */
        conn_close(&stream->conn);
        agent_free(stream);
        goto cleanup;
    }

    /* 5. 发送 HTTP 请求 */
    if(conn_write_all(&stream->conn, request_buf.data, request_buf.len,
                      timeout_sec) != 0)
    {
        AGENT_LOG("HTTP stream: write request failed");
        conn_close(&stream->conn);
        agent_free(stream);
        goto cleanup;
    }

    /* 6. 读取并解析响应头（读到 \r\n\r\n 为止） */
    if(stream_read_headers(&stream->conn, timeout_sec, &headers_buf) != 0)
    {
        AGENT_LOG("HTTP stream: failed to read response headers");
        conn_close(&stream->conn);
        agent_free(stream);
        goto cleanup;
    }

    /* 7. 从响应头中提取状态码 */
    if(out_status)
    {
        *out_status = parse_status_code(headers_buf.data, headers_buf.len);
    }

    /* 8. 检测是否为 chunked 传输编码 */
    stream->chunked = header_value_contains(headers_buf.data, headers_buf.len,
                                            "Transfer-Encoding", "chunked");
    stream->headers_parsed = true;

    /* 9. 保存响应头之后多读到的 body 字节。
     *
     * stream_read_headers() 内部通过 conn_read() 批量读取，TLS/socket 可能一次
     * 返回了完整的响应头 + 部分响应体。如果不保存这些多出来的字节，
     * 它们会在 headers_buf 释放后丢失，导致 SSE 解析漏掉首批 token
     * 或 chunked 解析因缺少起始数据而失败。
     */
    {
        char *hdr_end = find_header_end(headers_buf.data, headers_buf.len);
        if(hdr_end)
        {
            /* header_end 指向 \r\n\r\n 的第一个 \r，+4 跳过整个终止符 */
            size_t header_total = (size_t)(hdr_end - headers_buf.data) + 4;
            size_t body_bytes = headers_buf.len - header_total;
            if(body_bytes > 0)
            {
                stream->pending_body = agent_malloc(body_bytes);
                if(stream->pending_body)
                {
                    memcpy(stream->pending_body,
                           headers_buf.data + header_total, body_bytes);
                    stream->pending_body_len = body_bytes;
                    stream->pending_body_pos = 0;
                    AGENT_LOG("HTTP stream: saved %u pending body bytes past headers",
                              (unsigned)body_bytes);
                }
                else
                {
                    /* pending_body 分配失败：已经读到但无法保存的 body 字节
                     * 会丢失，后续 SSE/chunked 解析会从缺失位置开始，
                     * 产生错误结果（如 SSE 事件被截断、chunk 帧错位）。
                     * 必须关闭连接并返回失败，不能继续打开流。 */
                    AGENT_LOG("HTTP stream: failed to allocate pending body (%u bytes), aborting",
                              (unsigned)body_bytes);
                    conn_close(&stream->conn);
                    agent_free(stream);
                    goto cleanup;
                }
            }
        }
    }

    AGENT_LOG("HTTP stream: open ok, status=%d, chunked=%d, pending=%u",
              out_status ? *out_status : 0, stream->chunked,
              (unsigned)stream->pending_body_len);

    *out_stream = stream;
    rc = 0;

cleanup:
    byte_buf_free(&headers_buf);
    byte_buf_free(&request_buf);
    agent_url_free(&url);
    return rc;
}

int agent_http_stream_read(agent_http_stream_t *stream,
                           void *buf, size_t buf_size)
{
    if(!stream || !buf || buf_size == 0)
    {
        AGENT_LOG("HTTP stream read: invalid params (stream=%p buf=%p buf_size=%u)",
                  stream, buf, (unsigned)buf_size);
        return -1;
    }
    if(!stream->headers_parsed)
    {
        AGENT_LOG("HTTP stream: read before headers parsed");
        return -1;
    }

    /*
     * 非法状态：连接已经关闭或出错。
     * conn.fd == -1 表示底层 socket 已释放。
     */
    if(!agent_socket_is_valid(stream->conn.fd))
    {
        return 0;
    }

    if(stream->chunked)
    {
        /*
         * Chunked 传输编码解码流程。
         *
         * 所有原始字节读取都通过 stream_raw_read()，它会优先消耗
         * stream_open 阶段多读到的 pending body 字节，确保 chunked
         * 解码器能看到完整的帧结构（<hex-size>\r\n<data>\r\n）。
         *
         * 绝对不能直接用 conn_read() 读 body 数据，否则 pending body
         * 中的 chunk 帧头会被跳过，SSE 解析会看到垃圾数据。
         */

        /* 当前 chunk 已读完，需要解析下一个 chunk */
        if(stream->chunk_remaining == 0)
        {
            int size = stream_parse_chunk_size(stream);
            if(size < 0)
            {
                AGENT_LOG("HTTP stream: failed to parse chunk size");
                return -1;
            }
            if(size == 0)
            {
                /* 收到最后一个 chunk（size=0），流结束 */
                return 0;
            }
            stream->chunk_remaining = (unsigned long)size;
        }

        /* 读取当前 chunk 的数据，不超过调用者的缓冲区大小 */
        size_t to_read = buf_size;
        if(to_read > stream->chunk_remaining)
        {
            to_read = (size_t)stream->chunk_remaining;
        }

        int n = stream_raw_read(stream, buf, to_read);
        if(n <= 0)
        {
            AGENT_LOG("HTTP stream read: chunked stream_raw_read failed (n=%d, chunk_remaining=%lu)",
                      n, stream->chunk_remaining);
            return n < 0 ? -1 : 0;
        }

        stream->chunk_remaining -= (unsigned long)n;

        /* chunk 数据读完，下一个 stream_read 需要跳过尾部 \r\n */
        if(stream->chunk_remaining == 0)
        {
            stream->chunk_need_crlf = true;
        }

        return n;
    }
    else
    {
        /*
         * 非 chunked 响应：通过 stream_raw_read() 读取。
         * 优先消耗 pending body，耗尽后从 socket/TLS 读取。
         */
        return stream_raw_read(stream, buf, buf_size);
    }
}

void agent_http_stream_close(agent_http_stream_t *stream)
{
    if(!stream)
    {
        return;
    }

    AGENT_LOG("HTTP stream: closing");

    /* 关闭 TLS 和 TCP 连接 */
    conn_close(&stream->conn);

    /* 释放可能残留的 pending body 缓冲区 */
    agent_free(stream->pending_body);

    /* 释放流句柄 */
    agent_free(stream);
}
