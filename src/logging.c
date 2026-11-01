/*
 * logging.c - Structured logging and error handling (beta 0.42)
 */
#include "logging.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

FILE *g_log = NULL;
int g_verbose = 0;
int g_upsert = 0;

const char *describe_error(AppError e) {
    switch (e) {
        case ERR_OK: return "ok";
        case ERR_ARGS: return "invalid or missing arguments";
        case ERR_CONFIG: return "configuration error";
        case ERR_NET_DNS: return "DNS resolution failed";
        case ERR_NET_CONNECT: return "network connect failed";
        case ERR_TLS: return "TLS/SSL error";
        case ERR_NNTP_GREETING: return "NNTP greeting failed";
        case ERR_NNTP_CMD: return "NNTP command failed";
        case ERR_AUTH: return "authentication failed";
        case ERR_DB_CONNECT: return "database connection failed";
        case ERR_DB_SCHEMA: return "database schema creation failed";
        case ERR_DB_PREPARE: return "database prepared statement failed";
        default: return "runtime error";
    }
}

static void log_msg_internal(const char *level, const char *fmt, va_list ap) {
    char tbuf[64]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
    FILE *out = g_log ? g_log : stderr;
    fprintf(out, "%s [%s] ", tbuf, level);
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");
    fflush(out);
}

void log_open(const char *path) {
    if (path && *path) {
        g_log = fopen(path, "a");
        if (!g_log) {
            fprintf(stderr, "Could not open log file %s\n", path);
        }
    }
}

void log_close(void) {
    if (g_log) { fclose(g_log); g_log = NULL; }
}

void infof(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt); log_msg_internal("INFO", fmt, ap); va_end(ap);
}

void warnf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); log_msg_internal("WARN", fmt, ap); va_end(ap);
}

void fatal(AppError code, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); log_msg_internal("ERROR", fmt, ap); va_end(ap);
    fprintf(stderr, "Error (code %d): %s\n", code, describe_error(code));
    log_close();
    exit(code);
}
