#ifndef NNTP_H
#define NNTP_H
#include <openssl/ssl.h>

#define BUFSZ 8192

typedef struct {
    int sock;
    SSL_CTX *ctx;
    SSL *ssl;
    int use_ssl;
} Conn;

void conn_init(Conn *c);
void conn_cleanup(Conn *c);
int tcp_connect(const char *host, const char *port);
ssize_t conn_readline(Conn *c, char *buf, size_t bufsz);
char *conn_read_multiline(Conn *c);
ssize_t conn_sendf(Conn *c, const char *fmt, ...);
void ssl_init(void);
int conn_starttls(Conn *c);

int nntp_auth(Conn *c, const char *user, const char *pass);
int nntp_starttls(Conn *c);
int nntp_group(Conn *c, const char *group, int *count, int *first, int *last);
char *nntp_xover(Conn *c, int first, int last);
char *nntp_head(Conn *c, int artnum);

#endif