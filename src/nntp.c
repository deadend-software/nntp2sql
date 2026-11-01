/*
 * nntp.c - NNTP networking helpers (beta .3)
 */
#include "nntp.h"
#include "logging.h"
#include <openssl/err.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

void conn_init(Conn *c) { memset(c, 0, sizeof(*c)); c->sock = -1; }

void conn_cleanup(Conn *c) {
    if (!c) return;
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); c->ssl = NULL; }
    if (c->ctx) { SSL_CTX_free(c->ctx); c->ctx = NULL; }
    if (c->sock >= 0) close(c->sock);
}

int tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp; int s, fd = -1;
    memset(&hints, 0, sizeof(hints)); hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    if ((s = getaddrinfo(host, port, &hints, &res)) != 0) { warnf("getaddrinfo failed for %s:%s: %s", host, port, gai_strerror(s)); return -1; }
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd == -1) { warnf("Unable to connect to %s:%s: %s", host, port, strerror(errno)); }
    return fd;
}

ssize_t conn_readline(Conn *c, char *buf, size_t bufsz) {
    size_t pos = 0;
    while (pos + 1 < bufsz) {
        char ch; ssize_t r = c->use_ssl ? SSL_read(c->ssl, &ch, 1) : recv(c->sock, &ch, 1, 0);
        if (r <= 0) return -1; buf[pos++] = ch;
        if (pos >= 2 && buf[pos-2] == '\r' && buf[pos-1] == '\n') break;
    }
    buf[pos] = '\0'; return (ssize_t)pos;
}

char *conn_read_multiline(Conn *c) {
    size_t cap = 8192, len = 0; char *out = malloc(cap); if (!out) return NULL; out[0] = '\0';
    char line[BUFSZ];
    while (1) {
        if (conn_readline(c, line, sizeof(line)) <= 0) { free(out); return NULL; }
        size_t l = strlen(line); while (l && (line[l-1] == '\r' || line[l-1] == '\n')) line[--l] = '\0';
        if (strcmp(line, ".") == 0) break; if (line[0] == '.') memmove(line, line+1, strlen(line));
        size_t need = len + l + 2; if (need > cap) { cap = need * 2; out = realloc(out, cap); if (!out) return NULL; }
        memcpy(out + len, line, l); len += l; out[len++] = '\n'; out[len] = '\0';
    }
    return out;
}

ssize_t conn_sendf(Conn *c, const char *fmt, ...) {
    char buf[BUFSZ]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    size_t len = strlen(buf);
    if (len < 2 || buf[len-2] != '\r' || buf[len-1] != '\n') { if (len + 2 >= sizeof(buf)) return -1; buf[len++]='\r'; buf[len++]='\n'; buf[len]='\0'; }
    if (c->use_ssl) { int r = SSL_write(c->ssl, buf, (int)len); return r; } else { return send(c->sock, buf, len, 0); }
}

void ssl_init(void) { SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms(); }

int conn_starttls(Conn *c) {
    if (c->use_ssl) return 1; ssl_init(); c->ctx = SSL_CTX_new(TLS_client_method()); if (!c->ctx) return 0;
    c->ssl = SSL_new(c->ctx); if (!c->ssl) return 0; if (!SSL_set_fd(c->ssl, c->sock)) return 0;
    if (SSL_connect(c->ssl) <= 0) { ERR_print_errors_fp(stderr); return 0; }
    c->use_ssl = 1; return 1;
}

int nntp_auth(Conn *c, const char *user, const char *pass) {
    char resp[BUFSZ]; conn_sendf(c, "AUTHINFO USER %s", user);
    if (conn_readline(c, resp, sizeof(resp)) <= 0) return -1; int code = atoi(resp);
    if (code == 381) { conn_sendf(c, "AUTHINFO PASS %s", pass); if (conn_readline(c, resp, sizeof(resp)) <= 0) return -1; return atoi(resp); }
    return code;
}

int nntp_starttls(Conn *c) { char resp[BUFSZ]; conn_sendf(c, "STARTTLS"); if (conn_readline(c, resp, sizeof(resp)) <= 0) return -1; return atoi(resp); }

int nntp_group(Conn *c, const char *group, int *count, int *first, int *last) {
    char buf[BUFSZ]; conn_sendf(c, "GROUP %s", group); if (conn_readline(c, buf, sizeof(buf)) <= 0) return -1; int code = atoi(buf);
    if (code >= 200 && code < 300) { int cnt=0, f=0, l=0; sscanf(buf, "%*d %d %d %d", &cnt, &f, &l); if (count) *count=cnt; if (first) *first=f; if (last) *last=l; }
    return code;
}

char *nntp_xover(Conn *c, int first, int last) {
    char buf[BUFSZ]; conn_sendf(c, "XOVER %d-%d", first, last); if (conn_readline(c, buf, sizeof(buf)) <= 0) return NULL; int code = atoi(buf);
    if (code < 200 || code >= 300) { warnf("XOVER rejected: %s", buf); return NULL; } return conn_read_multiline(c);
}

char *nntp_head(Conn *c, int artnum) {
    char buf[BUFSZ]; conn_sendf(c, "HEAD %d", artnum); if (conn_readline(c, buf, sizeof(buf)) <= 0) return NULL; int code = atoi(buf);
    if (code < 200 || code >= 300) { warnf("HEAD rejected for %d: %s", artnum, buf); return NULL; } return conn_read_multiline(c);
}
