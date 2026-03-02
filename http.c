/*
 * http.c - Minimal HTTP/1.0 client for Solaris 7
 *
 * Uses raw BSD sockets. On Solaris, link with -lsocket -lnsl.
 * No external dependencies.
 */

#include "http.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define HTTP_RECV_BUF  4096
#define HTTP_MAX_RESP  (2 * 1024 * 1024)  /* 2 MB max response */

/* Connect to host:port, returns socket fd or -1 on error */
static int tcp_connect(const char *host, int port)
{
    struct sockaddr_in addr;
    struct hostent *he;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    /* Try numeric IP first */
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == (in_addr_t)-1) {
        /* DNS lookup */
        he = gethostbyname(host);
        if (!he) {
            close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* Send all bytes from buf. Returns 0 on success, -1 on error. */
static int send_all(int fd, const char *buf, int len)
{
    int sent = 0;
    int n;
    while (sent < len) {
        n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* Read entire response into a growing buffer. Returns malloc'd buffer. */
static char *recv_all(int fd, long *out_len)
{
    char *buf = NULL;
    long alloc = 0;
    long total = 0;
    int n;
    char tmp[HTTP_RECV_BUF];

    while ((n = recv(fd, tmp, sizeof(tmp), 0)) > 0) {
        if (total + n > HTTP_MAX_RESP) break;
        if (total + n > alloc) {
            alloc = (total + n) * 2;
            if (alloc > HTTP_MAX_RESP) alloc = HTTP_MAX_RESP;
            buf = (char *)realloc(buf, alloc + 1);
            if (!buf) return NULL;
        }
        memcpy(buf + total, tmp, n);
        total += n;
    }

    if (buf) buf[total] = '\0';
    *out_len = total;
    return buf;
}

/* Parse HTTP response: extract status code, headers, body split */
static int parse_response(const char *raw, long raw_len, http_response_t *resp)
{
    const char *p, *body_start, *line_end;
    long header_len;

    memset(resp, 0, sizeof(*resp));

    /* Find status code in first line: "HTTP/1.x NNN ..." */
    p = strstr(raw, " ");
    if (!p) return -1;
    resp->status_code = atoi(p + 1);

    /* Find end of headers (blank line) */
    body_start = strstr(raw, "\r\n\r\n");
    if (body_start) {
        header_len = body_start - raw;
        body_start += 4;
    } else {
        body_start = strstr(raw, "\n\n");
        if (body_start) {
            header_len = body_start - raw;
            body_start += 2;
        } else {
            /* No body */
            header_len = raw_len;
            body_start = raw + raw_len;
        }
    }

    /* Extract Content-Type header */
    resp->content_type = NULL;
    p = raw;
    while (p < raw + header_len) {
        line_end = strstr(p, "\r\n");
        if (!line_end) line_end = strstr(p, "\n");
        if (!line_end) break;

        if (strncasecmp(p, "Content-Type:", 13) == 0) {
            const char *val = p + 13;
            int vlen;
            while (*val == ' ') val++;
            vlen = line_end - val;
            resp->content_type = (char *)malloc(vlen + 1);
            memcpy(resp->content_type, val, vlen);
            resp->content_type[vlen] = '\0';
        }

        p = line_end;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
    }

    /* Copy body */
    resp->body_len = raw_len - (body_start - raw);
    if (resp->body_len > 0) {
        resp->body = (char *)malloc(resp->body_len + 1);
        memcpy(resp->body, body_start, resp->body_len);
        resp->body[resp->body_len] = '\0';
    } else {
        resp->body = NULL;
        resp->body_len = 0;
    }

    return 0;
}

/* Perform an HTTP request (GET or POST) */
static int http_request(const char *host, int port, const char *method,
                        const char *path, const char *content_type,
                        const char *req_body, int req_body_len,
                        http_response_t *resp)
{
    int fd;
    char header[2048];
    int hlen;
    char *raw;
    long raw_len;
    int rc;

    memset(resp, 0, sizeof(*resp));

    fd = tcp_connect(host, port);
    if (fd < 0) return -1;

    /* Build request */
    if (req_body && req_body_len > 0) {
        hlen = snprintf(header, sizeof(header),
            "%s %s HTTP/1.0\r\n"
            "Host: %s:%d\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path, host, port,
            content_type ? content_type : "application/octet-stream",
            req_body_len);
    } else {
        hlen = snprintf(header, sizeof(header),
            "%s %s HTTP/1.0\r\n"
            "Host: %s:%d\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path, host, port);
    }

    /* Send headers */
    if (send_all(fd, header, hlen) < 0) {
        close(fd);
        return -1;
    }

    /* Send body if present */
    if (req_body && req_body_len > 0) {
        if (send_all(fd, req_body, req_body_len) < 0) {
            close(fd);
            return -1;
        }
    }

    /* Receive response */
    raw = recv_all(fd, &raw_len);
    close(fd);

    if (!raw || raw_len == 0) {
        if (raw) free(raw);
        return -1;
    }

    rc = parse_response(raw, raw_len, resp);
    free(raw);
    return rc;
}

int http_get(const char *host, int port, const char *path,
             http_response_t *resp)
{
    return http_request(host, port, "GET", path, NULL, NULL, 0, resp);
}

int http_post_json(const char *host, int port, const char *path,
                   const char *json_body, http_response_t *resp)
{
    return http_request(host, port, "POST", path,
                        "application/json",
                        json_body, json_body ? strlen(json_body) : 0,
                        resp);
}

void http_response_free(http_response_t *resp)
{
    if (resp->body) {
        free(resp->body);
        resp->body = NULL;
    }
    if (resp->content_type) {
        free(resp->content_type);
        resp->content_type = NULL;
    }
    resp->body_len = 0;
    resp->status_code = 0;
}
