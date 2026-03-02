/*
 * http.h - Minimal HTTP/1.0 client for Solaris 7
 *
 * Uses raw sockets (-lsocket -lnsl on Solaris).
 * No TLS, no chunked encoding, no keep-alive.
 * Perfect for talking to a local bridge server.
 */

#ifndef SPARCCORD_HTTP_H
#define SPARCCORD_HTTP_H

#include <stddef.h>

/* Response from an HTTP request */
typedef struct {
    int   status_code;   /* HTTP status code (200, 404, etc.) */
    char *body;          /* Response body (malloc'd, caller frees) */
    long  body_len;      /* Length of body in bytes */
    char *content_type;  /* Content-Type header value (malloc'd, caller frees) */
} http_response_t;

/*
 * Perform an HTTP GET request.
 * Returns 0 on success, -1 on error.
 * Caller must free resp->body and resp->content_type.
 */
int http_get(const char *host, int port, const char *path,
             http_response_t *resp);

/*
 * Perform an HTTP POST request with a JSON body.
 * Returns 0 on success, -1 on error.
 * Caller must free resp->body and resp->content_type.
 */
int http_post_json(const char *host, int port, const char *path,
                   const char *json_body, http_response_t *resp);

/*
 * Free the contents of an http_response_t (but not the struct itself).
 */
void http_response_free(http_response_t *resp);

#endif /* SPARCCORD_HTTP_H */
