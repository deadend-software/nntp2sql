#ifndef LOGGING_H
#define LOGGING_H
#include <stdio.h>

typedef enum {
    ERR_OK = 0,
    ERR_ARGS = 2,
    ERR_CONFIG = 3,
    ERR_NET_DNS = 10,
    ERR_NET_CONNECT = 11,
    ERR_TLS = 12,
    ERR_NNTP_GREETING = 13,
    ERR_NNTP_CMD = 14,
    ERR_AUTH = 15,
    ERR_DB_CONNECT = 20,
    ERR_DB_SCHEMA = 21,
    ERR_DB_PREPARE = 22,
    ERR_RUNTIME = 30
} AppError;

extern FILE *g_log;
extern int g_verbose;
extern int g_upsert;

const char *describe_error(AppError e);
void log_open(const char *path);
void log_close(void);
void infof(const char *fmt, ...);
void warnf(const char *fmt, ...);
void fatal(AppError code, const char *fmt, ...);

#endif