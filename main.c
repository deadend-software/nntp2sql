/*
 * main.c
 *
 * Simple NNTP -> SQL dumper supporting SSL/STARTTLS, AUTH, group selection,
 * headers-only (via XOVER) or full-headers (via HEAD/ARTICLE), and backends:
 *   sqlite3, mariadb/mysql
 *
 * Build (example):
 *   gcc -o nntp2sql main.c -lssl -lcrypto -lsqlite3 -lmysqlclient
 *
 * Note: link only the DB libs you need. Adjust include/library paths as required.
 *
 * Usage:
 *   nntp2sql --host HOST [--port PORT] [--ssl] [--starttls]
 *            [--user USER --pass PASS]
 *            --db-type {sqlite|mariadb|mysql} --db-name DBNAME
 *            [--db-host HOST --db-port PORT --db-user USER --db-pass PASS]
 *            --group GROUPNAME [--headers-only] [--limit N]
 *            [--progress-width N] [--init-db]
 *            [--conf FILE] [--write-conf FILE]
 *
 * This is a compact example for demonstration. Production code should add more
 * robust error handling, retries, better SQL escaping/prepared statements,
 * and concurrency considerations.
 */

#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include <signal.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/ioctl.h>

/* Optional DB client headers */
#include <sqlite3.h>
#if defined(__has_include) && __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>  /* MariaDB/MySQL */
#else
#include <mysql.h>
#endif
#ifdef HAVE_PQ
#include <libpq-fe.h>     /* PostgreSQL */
#endif
#include <pthread.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include "src/export_html.h" /* HTML export declarations */

#define BUFSZ 8192

/* Simple logging */
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

static FILE *g_log = NULL;
static int g_verbose = 0;
static int g_upsert = 1; /* default enabled so skeleton/initialized DBs accept new articles */
static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_stop_signal = 0;
static _Atomic long long g_sql_records_updated = 0;
static _Atomic long long g_articles_updated = 0;
static _Atomic long long g_groups_updated = 0;

static void handle_stop_signal(int sig) {
    g_stop_requested = 1;
    g_stop_signal = sig;
}

static const char *describe_error(AppError e) {
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

static void log_open(const char *path) {
    if (path && *path) {
        g_log = fopen(path, "a");
        if (!g_log) {
            fprintf(stderr, "Could not open log file %s: %s\n", path, strerror(errno));
        }
    }
}

static void log_close(void) {
    if (g_log) { fclose(g_log); g_log = NULL; }
}

static void log_msg(const char *level, const char *fmt, va_list ap) {
    char tbuf[64];
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
    FILE *out = g_log ? g_log : stderr;
    flockfile(out);
    fprintf(out, "%s [%s] ", tbuf, level);
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");
    fflush(out);
    funlockfile(out);
}

static void infof(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt); log_msg("INFO", fmt, ap); va_end(ap);
}

static void warnf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); log_msg("WARN", fmt, ap); va_end(ap);
}

static void fatal(AppError code, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); log_msg("ERROR", fmt, ap); va_end(ap);
    fprintf(stderr, "Error (code %d): %s\n", code, describe_error(code));
    log_close();
    exit(code);
}

/* Network/SSL wrapper */

typedef struct {
    int sock;
    SSL_CTX *ctx;
    SSL *ssl;
    int use_ssl;
    int last_code;
    long long bytes_read;
    char rbuf[65536];
    size_t rpos;
    size_t rlen;
} Conn;

static int tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    int s, fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if ((s = getaddrinfo(host, port, &hints, &res)) != 0) {
        warnf("getaddrinfo failed for %s:%s: %s", host, port, gai_strerror(s));
        return -1;
    }
    for (int retry = 0; retry < 25 && fd == -1 && !g_stop_requested; ++retry) {
        if (retry > 0) {
            struct timespec ts = {0, 5000000};
            nanosleep(&ts, NULL);
            if (g_stop_requested) break;
        }
        for (rp = res; rp != NULL && !g_stop_requested; rp = rp->ai_next) {
            fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd == -1) continue;
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { close(fd); fd = -1; continue; }
            int connected = connect(fd, rp->ai_addr, rp->ai_addrlen) == 0;
            if (!connected && errno == EINPROGRESS) {
                int waited = 0;
                while (waited < 10000 && !g_stop_requested) {
                    struct pollfd event = {fd, POLLOUT, 0};
                    int prc = poll(&event, 1, 50);
                    if (prc > 0) {
                        if (!(event.revents & (POLLERR | POLLHUP))) {
                            int error = 0; socklen_t size = sizeof(error);
                            connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
                            if (!connected && error) errno = error;
                        }
                        break;
                    }
                    if (prc < 0 && errno != EINTR) break;
                    waited += 50;
                }
            }
            if (fcntl(fd, F_SETFL, flags) < 0) connected = 0;
            struct timeval timeout = {15, 0};
            if (connected && (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)) connected = 0;
            if (connected) break;
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd == -1) {
        if (!g_stop_requested) warnf("Unable to connect to %s:%s: %s", host, port, strerror(errno));
        return -1;
    }
    return fd;
}

static void conn_init(Conn *c) {
    memset(c, 0, sizeof(*c));
    c->sock = -1;
}

static void conn_cleanup(Conn *c) {
    if (!c) return;
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); c->ssl = NULL; }
    if (c->ctx) { SSL_CTX_free(c->ctx); c->ctx = NULL; }
    if (c->sock >= 0) close(c->sock);
    c->sock = -1;
    c->use_ssl = 0;
    c->last_code = 0;
    c->rpos = 0;
    c->rlen = 0;
}

static int conn_wait_readable(Conn *c, int total_timeout_ms) {
    if (g_stop_requested || !c || c->sock < 0) return 0;
    if (c->rpos < c->rlen) return 1;
    if (c->use_ssl && c->ssl && SSL_pending(c->ssl) > 0) return 1;
    int waited = 0;
    while (waited < total_timeout_ms) {
        if (g_stop_requested) return 0;
        struct pollfd pfd = { c->sock, POLLIN, 0 };
        int rc = poll(&pfd, 1, 50);
        if (rc > 0) return 1;
        if (rc < 0) {
            if (errno == EINTR) {
                if (g_stop_requested) return 0;
                continue;
            }
            return 0;
        }
        waited += 50;
    }
    return 0;
}

/* read one line (CRLF terminated) into buf (null-terminated) using 64 KB read buffer */
static ssize_t conn_readline(Conn *c, char *buf, size_t bufsz) {
    if (bufsz == 0) return -1;
    size_t pos = 0;
    while (pos + 1 < bufsz) {
        if (g_stop_requested) return -1;
        if (c->rpos >= c->rlen) {
            c->rpos = c->rlen = 0;
            if (!conn_wait_readable(c, 15000)) return -1;
            ssize_t r = c->use_ssl
                ? SSL_read(c->ssl, c->rbuf, sizeof(c->rbuf))
                : recv(c->sock, c->rbuf, sizeof(c->rbuf), 0);
            if (r <= 0) return -1;
            c->rlen = (size_t)r;
        }
        size_t avail = c->rlen - c->rpos;
        size_t room = bufsz - 1 - pos;
        size_t look = avail < room ? avail : room;
        char *nl = memchr(c->rbuf + c->rpos, '\n', look);
        if (nl) {
            size_t chunk = (size_t)(nl - (c->rbuf + c->rpos)) + 1;
            memcpy(buf + pos, c->rbuf + c->rpos, chunk);
            c->rpos += chunk;
            pos += chunk;
            break;
        } else {
            memcpy(buf + pos, c->rbuf + c->rpos, look);
            c->rpos += look;
            pos += look;
        }
    }
    buf[pos] = '\0';
    if (pos < 2 || buf[pos-2] != '\r' || buf[pos-1] != '\n') return -1;
    c->bytes_read += (long long)pos;
    return (ssize_t)pos;
}

/* read multiline dot-terminated response; caller must free returned char*; lines are '\n' separated */
static char *conn_read_multiline_ex(Conn *c, int allow_partial_on_stop) {
    size_t cap = 8192, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    char line[BUFSZ];
    while (1) {
        if (conn_readline(c, line, sizeof(line)) <= 0) {
            if (g_stop_requested && allow_partial_on_stop && len > 0) return out;
            free(out);
            return NULL;
        }
        /* strip CRLF */
        size_t l = strlen(line);
        while (l && (line[l-1] == '\r' || line[l-1] == '\n')) line[--l] = '\0';
        if (strcmp(line, ".") == 0) break;
        /* handle transparent dot-stuffing */
        if (line[0] == '.' ) { memmove(line, line+1, strlen(line)); --l; }
        size_t need = len + l + 2;
        if (need > cap) {
            cap = need * 2;
            char *grown = realloc(out, cap);
            if (!grown) { free(out); return NULL; }
            out = grown;
        }
        memcpy(out + len, line, l);
        len += l;
        out[len++] = '\n';
        out[len] = '\0';
    }
    return out;
}

static char *conn_read_multiline(Conn *c) {
    return conn_read_multiline_ex(c, 0);
}

static ssize_t conn_sendf(Conn *c, const char *fmt, ...) {
    char buf[BUFSZ];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    /* ensure CRLF at end */
    if (len < 2 || buf[len-2] != '\r' || buf[len-1] != '\n') {
        if (len + 2 >= sizeof(buf)) return -1;
        buf[len++] = '\r'; buf[len++] = '\n'; buf[len] = '\0';
    }
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = c->use_ssl ? SSL_write(c->ssl, buf + sent, (int)(len - sent)) :
            send(c->sock, buf + sent, len - sent, 0);
        if (n < 0 && !c->use_ssl && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

/* initialize SSL for client */
static void ssl_init(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

/* start tls on existing conn */
static int conn_starttls(Conn *c) {
    if (c->use_ssl) return 1;
    c->rpos = 0;
    c->rlen = 0;
    ssl_init();
    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) return 0;
    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) return 0;
    if (!SSL_set_fd(c->ssl, c->sock)) return 0;
    if (SSL_connect(c->ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        return 0;
    }
    c->use_ssl = 1;
    return 1;
}

/* DB abstraction: using declarations from src/db.h via export_html */

/* Iteration helpers for article queries by group */
static sqlite3_stmt *sqlite_q_stmt = NULL;
int db_query_articles_begin(DB *db, const char *group_name){
    if (db->type == DB_SQLITE){
        const char *sql = "SELECT artnum, subject, author, date FROM articles WHERE group_name = ? ORDER BY artnum";
        if (sqlite_q_stmt) { sqlite3_finalize(sqlite_q_stmt); sqlite_q_stmt = NULL; }
        if (sqlite3_prepare_v2(db->sqlite, sql, -1, &sqlite_q_stmt, NULL) != SQLITE_OK) return 0;
        if (sqlite3_bind_text(sqlite_q_stmt, 1, group_name, -1, SQLITE_TRANSIENT) != SQLITE_OK) return 0;
        return 1;
    } else if (db->type == DB_MYSQL){
        /* Use simple query; caller will fetch using mysql_use_result */
        char esc[512]; mysql_real_escape_string(db->mysql, esc, group_name, (unsigned long)strlen(group_name));
        char q[1024]; snprintf(q, sizeof(q), "SELECT artnum, subject, author, date FROM articles WHERE group_name='%s' ORDER BY artnum", esc);
        if (mysql_query(db->mysql, q)) { warnf("mysql query failed: %s", mysql_error(db->mysql)); return 0; }
        return 1;
    }
    return 0;
}

int db_query_articles_next(DB *db, DBRow *out){
    if (db->type == DB_SQLITE){
        int rc = sqlite3_step(sqlite_q_stmt);
        if (rc != SQLITE_ROW) return 0;
        out->artnum = sqlite3_column_int64(sqlite_q_stmt, 0);
        out->subject = (const char*)sqlite3_column_text(sqlite_q_stmt, 1);
        out->author  = (const char*)sqlite3_column_text(sqlite_q_stmt, 2);
        out->date    = (const char*)sqlite3_column_text(sqlite_q_stmt, 3);
        return 1;
    } else if (db->type == DB_MYSQL){
        static MYSQL_RES *res = NULL; static MYSQL_ROW row;
        static int initialized = 0;
        if (!initialized){ res = mysql_store_result(db->mysql); initialized = 1; }
        if (!res) return 0;
        row = mysql_fetch_row(res);
        if (!row) return 0;
        out->artnum = row[0] ? atoll(row[0]) : 0;
        out->subject = row[1]; out->author = row[2]; out->date = row[3];
        return 1;
    }
    return 0;
}

void db_query_articles_end(DB *db){
    if (db->type == DB_SQLITE){
        if (sqlite_q_stmt) { sqlite3_finalize(sqlite_q_stmt); sqlite_q_stmt = NULL; }
    } else if (db->type == DB_MYSQL){
        MYSQL_RES *res = mysql_store_result(db->mysql); /* ensure drained */
        if (res) mysql_free_result(res);
    }
}

void db_close(DB *db) {
    if (!db) return;
    if (db->type == DB_SQLITE) {
        if (db->sqlite_insert_article) sqlite3_finalize(db->sqlite_insert_article);
        if (db->sqlite_insert_group) sqlite3_finalize(db->sqlite_insert_group);
        if (db->sqlite_article_ins) sqlite3_finalize(db->sqlite_article_ins);
        if (db->sqlite_group_ins) sqlite3_finalize(db->sqlite_group_ins);
        if (db->sqlite) sqlite3_wal_checkpoint_v2(db->sqlite, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    } else if (db->type == DB_MYSQL) {
        if (db->mysql_insert_article) mysql_stmt_close(db->mysql_insert_article);
        if (db->mysql_article_ins) mysql_stmt_close(db->mysql_article_ins);
    }
    if (db->sqlite) sqlite3_close(db->sqlite);
    if (db->mysql) mysql_close(db->mysql);
    /* no postgres */
    memset(db, 0, sizeof(*db));
}

/* Settings live in a table named after the database: news0 -> news0_settings. For SQLite the name comes from
 * the file (data/news.db -> news_settings). Only letters, digits and '_' survive, so the name is always safe to
 * put in a statement. The old shared table called "settings" is still read, and copied over on first use. */
static char g_settings_table[96] = "settings";
static void set_settings_table(const char *db_name, int is_file) {
    char stem[64] = "";
    const char *base = db_name ? db_name : "";
    if (is_file) {
        const char *slash = strrchr(base, '/');
        if (slash) base = slash + 1;
    }
    size_t n = 0;
    for (const char *p = base; *p && n < sizeof(stem) - 1; ++p) {
        if (is_file && *p == '.' && p > base && strchr(p + 1, '.') == NULL) break; /* drop the file extension */
        stem[n++] = (isalnum((unsigned char)*p) || *p == '_') ? *p : '_';
    }
    stem[n] = '\0';
    if (!*stem) snprintf(stem, sizeof(stem), "nntp2sql");
    snprintf(g_settings_table, sizeof(g_settings_table), "%s_settings", stem);
}

static void db_exec(DB *db, const char *sql) {
    if (db->type == DB_SQLITE) {
        char *err = NULL;
        if (sqlite3_exec(db->sqlite, sql, NULL, NULL, &err) != SQLITE_OK) {
            char msg[512];
            snprintf(msg, sizeof(msg), "%s", err ? err : sqlite3_errmsg(db->sqlite));
            if (err) sqlite3_free(err);
            fatal(ERR_DB_SCHEMA, "sqlite schema error: %s", msg);
        }
    } else if (db->type == DB_MYSQL) {
        if (mysql_query(db->mysql, sql)) {
            fatal(ERR_DB_SCHEMA, "mysql schema error: %s", mysql_error(db->mysql));
        }
    }
}

/* Basic escaping for SQL insertion; for production use prepared statements. */
char *db_escape(DB *db, const char *s) {
    if (!s) return strdup("");
    if (db->type == DB_SQLITE) {
        /* simple safe allocator: replace single quote with two single quotes */
        size_t n = strlen(s);
        size_t cap = n * 2 + 3;
        char *out = malloc(cap);
        char *p = out;
        *p++ = '\'';
        for (; *s; s++) {
            if (*s == '\'') { *p++ = '\''; *p++ = '\''; }
            else *p++ = *s;
        }
        *p++ = '\'';
        *p++ = '\0';
        return out;
    } else if (db->type == DB_MYSQL) {
        unsigned long len = (unsigned long)strlen(s);
        char *buf = malloc(len*2 + 3);
        mysql_real_escape_string(db->mysql, buf, s, len);
        /* wrap in quotes */
        char *out = malloc(strlen(buf) + 3);
        sprintf(out, "'%s'", buf);
        free(buf);
        return out;
    }
    return strdup("''");
}

/* Create tables */
void db_init_schema(DB *db) {
    /* Note: MySQL reserves GROUPS, REFERENCES, LINES. Avoid reserved words: use article_count, refs, line_count. Quote MySQL identifiers. */
    if (db->type == DB_SQLITE) {
        sqlite3_busy_timeout(db->sqlite, 10000);
        sqlite3_exec(db->sqlite, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-65536;", NULL, NULL, NULL);
        db_exec(db,
            "CREATE TABLE IF NOT EXISTS groups ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "name TEXT UNIQUE NOT NULL DEFAULT '', "
            "article_count INTEGER NOT NULL DEFAULT 0, "
            "first INTEGER NOT NULL DEFAULT 0, "
            "last INTEGER NOT NULL DEFAULT 0);"
        );
        db_exec(db,
            "CREATE TABLE IF NOT EXISTS articles ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "artnum INTEGER NOT NULL DEFAULT 0, "
            "subject TEXT NOT NULL DEFAULT '', "
            "author TEXT NOT NULL DEFAULT '', "
            "date TEXT NOT NULL DEFAULT '', "
            "message_id TEXT NOT NULL DEFAULT '', "
            "refs TEXT NOT NULL DEFAULT '', "
            "bytes INTEGER NOT NULL DEFAULT 0, "
            "line_count INTEGER NOT NULL DEFAULT 0, "
            "group_name TEXT NOT NULL DEFAULT '');"
        );
        db_exec(db,
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_articles_group_artnum ON articles(group_name, artnum);"
        );
        db_exec(db,
            "CREATE INDEX IF NOT EXISTS idx_articles_subject ON articles(subject);"
        );
        db_exec(db,
            "CREATE INDEX IF NOT EXISTS idx_articles_author ON articles(author);"
        );
        db_exec(db,
            "CREATE INDEX IF NOT EXISTS idx_articles_message_id ON articles(message_id);"
        );
        {
            char sql[320];
            snprintf(sql, sizeof(sql), "CREATE TABLE IF NOT EXISTS \"%s\" (key TEXT PRIMARY KEY NOT NULL DEFAULT '', value TEXT NOT NULL DEFAULT '');", g_settings_table);
            db_exec(db, sql);
        }
        if (db->sqlite_insert_article) { sqlite3_finalize(db->sqlite_insert_article); db->sqlite_insert_article = NULL; }
        if (db->sqlite_article_ins) { sqlite3_finalize(db->sqlite_article_ins); db->sqlite_article_ins = NULL; }
        if (db->sqlite_insert_group) { sqlite3_finalize(db->sqlite_insert_group); db->sqlite_insert_group = NULL; }
        if (db->sqlite_group_ins) { sqlite3_finalize(db->sqlite_group_ins); db->sqlite_group_ins = NULL; }
        if (sqlite3_prepare_v2(db->sqlite,
            "UPDATE articles SET subject=?, author=?, date=?, message_id=?, refs=?, bytes=?, line_count=? WHERE group_name=? AND artnum=?",
            -1, &db->sqlite_insert_article, NULL) != SQLITE_OK) {
            fatal(ERR_DB_PREPARE, "sqlite prepare article-update failed: %s", sqlite3_errmsg(db->sqlite));
        }
        if (sqlite3_prepare_v2(db->sqlite,
            "INSERT INTO articles (subject,author,date,message_id,refs,bytes,line_count,group_name,artnum) VALUES (?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(group_name,artnum) DO UPDATE SET subject=excluded.subject,author=excluded.author,date=excluded.date,"
            "message_id=excluded.message_id,refs=excluded.refs,bytes=excluded.bytes,line_count=excluded.line_count",
            -1, &db->sqlite_article_ins, NULL) != SQLITE_OK) {
            fatal(ERR_DB_PREPARE, "sqlite prepare article-insert failed: %s", sqlite3_errmsg(db->sqlite));
        }
        if (sqlite3_prepare_v2(db->sqlite,
            "UPDATE groups SET article_count=?, first=?, last=? WHERE name=?",
            -1, &db->sqlite_insert_group, NULL) != SQLITE_OK) {
            fatal(ERR_DB_PREPARE, "sqlite prepare group-update failed: %s", sqlite3_errmsg(db->sqlite));
        }
        if (sqlite3_prepare_v2(db->sqlite,
            "INSERT INTO groups (name, article_count, first, last) VALUES (?,?,?,?)",
            -1, &db->sqlite_group_ins, NULL) != SQLITE_OK) {
            fatal(ERR_DB_PREPARE, "sqlite prepare group-insert failed: %s", sqlite3_errmsg(db->sqlite));
        }
    } else if (db->type == DB_MYSQL) {
        if (mysql_query(db->mysql,
            "CREATE TABLE IF NOT EXISTS `groups` ("
            "id INT AUTO_INCREMENT PRIMARY KEY, "
            "name VARCHAR(255) UNIQUE NOT NULL DEFAULT '', "
            "article_count BIGINT NOT NULL DEFAULT 0, "
            "first BIGINT NOT NULL DEFAULT 0, "
            "last BIGINT NOT NULL DEFAULT 0) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;")) {
            fatal(ERR_DB_SCHEMA, "mysql schema error (groups): %s", mysql_error(db->mysql));
        }
        /* include unique key and search indexes in table create for fresh databases */
        if (mysql_query(db->mysql,
            "CREATE TABLE IF NOT EXISTS `articles` ("
            "id BIGINT AUTO_INCREMENT PRIMARY KEY, "
            "`artnum` BIGINT NOT NULL DEFAULT 0, "
            "`subject` TEXT, "
            "`author` TEXT, "
            "`date` TEXT, "
            "`message_id` TEXT, "
            "`refs` TEXT, "
            "`bytes` BIGINT NOT NULL DEFAULT 0, "
            "`line_count` INT NOT NULL DEFAULT 0, "
            "`group_name` VARCHAR(255) NOT NULL DEFAULT '', "
            "UNIQUE KEY `idx_articles_group_artnum` (`group_name`,`artnum`), "
            "KEY `idx_articles_subject` (`subject`(191)), "
            "KEY `idx_articles_author` (`author`(191)), "
            "KEY `idx_articles_message_id` (`message_id`(191))) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;")) {
            fatal(ERR_DB_SCHEMA, "mysql schema error (articles): %s", mysql_error(db->mysql));
        }
        char settings_sql[320];
        snprintf(settings_sql, sizeof(settings_sql), "CREATE TABLE IF NOT EXISTS `%s` (`key` VARCHAR(191) PRIMARY KEY NOT NULL DEFAULT '', `value` TEXT) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;", g_settings_table);
        if (mysql_query(db->mysql, settings_sql)) {
            fatal(ERR_DB_SCHEMA, "mysql schema error (settings): %s", mysql_error(db->mysql));
        }
        /* for existing tables, ensure 64-bit BIGINT columns and unique key */
        (void)mysql_query(db->mysql, "ALTER TABLE `groups` MODIFY `article_count` BIGINT NOT NULL DEFAULT 0, MODIFY `first` BIGINT NOT NULL DEFAULT 0, MODIFY `last` BIGINT NOT NULL DEFAULT 0;");
        (void)mysql_query(db->mysql, "ALTER TABLE `articles` MODIFY `id` BIGINT AUTO_INCREMENT, MODIFY `artnum` BIGINT NOT NULL DEFAULT 0, MODIFY `bytes` BIGINT NOT NULL DEFAULT 0;");
        if (mysql_query(db->mysql, "ALTER TABLE `articles` ADD UNIQUE KEY `idx_articles_group_artnum` (`group_name`,`artnum`);")) {
            /* duplicate key name or existing index -> non-fatal */
            const char *err = mysql_error(db->mysql);
            if (mysql_errno(db->mysql) == 1062)
                warnf("Duplicate articles exist, so the unique index could not be created; run with --remove-duplicates to clean them up");
            else if (err && *err) {
                infof("mysql index add note: %s", err);
            }
        }
        if (db->mysql_insert_article) { mysql_stmt_close(db->mysql_insert_article); db->mysql_insert_article = NULL; }
        if (db->mysql_article_ins) { mysql_stmt_close(db->mysql_article_ins); db->mysql_article_ins = NULL; }
        db->mysql_insert_article = mysql_stmt_init(db->mysql);
        if (!db->mysql_insert_article) fatal(ERR_DB_PREPARE, "mysql_stmt_init failed for article");
        const char *art_sql = "UPDATE `articles` SET `subject`=?, `author`=?, `date`=?, `message_id`=?, `refs`=?, `bytes`=?, `line_count`=? WHERE `group_name`=? AND `artnum`=?";
        if (mysql_stmt_prepare(db->mysql_insert_article, art_sql, (unsigned long)strlen(art_sql))) {
            fatal(ERR_DB_PREPARE, "mysql prepare article-update failed: %s", mysql_error(db->mysql));
        }
        db->mysql_article_ins = mysql_stmt_init(db->mysql);
        if (!db->mysql_article_ins) fatal(ERR_DB_PREPARE, "mysql_stmt_init failed for article-insert");
        const char *art_ins = "INSERT INTO `articles` (`subject`,`author`,`date`,`message_id`,`refs`,`bytes`,`line_count`,`group_name`,`artnum`) VALUES (?,?,?,?,?,?,?,?,?) "
                              "ON DUPLICATE KEY UPDATE subject=VALUES(subject),author=VALUES(author),date=VALUES(date),message_id=VALUES(message_id),refs=VALUES(refs),bytes=VALUES(bytes),line_count=VALUES(line_count)";
        if (mysql_stmt_prepare(db->mysql_article_ins, art_ins, (unsigned long)strlen(art_ins))) {
            fatal(ERR_DB_PREPARE, "mysql prepare article-insert failed: %s", mysql_error(db->mysql));
        }
        /* MySQL group upsert uses quoted table/columns */
    }
#ifdef HAVE_PQ
    else if (db->type == DB_POSTGRES) {
        PGconn *pgc = (PGconn*)db->impl;
        if (!pgc) fatal(ERR_DB_CONNECT, "postgres connection handle is NULL");
        PGresult *r;
        r = PQexec(pgc, "CREATE TABLE IF NOT EXISTS groups (id SERIAL PRIMARY KEY, name TEXT UNIQUE NOT NULL DEFAULT '', article_count BIGINT NOT NULL DEFAULT 0, first BIGINT NOT NULL DEFAULT 0, last BIGINT NOT NULL DEFAULT 0);");
        if (PQresultStatus(r) != PGRES_COMMAND_OK) { char m[256]; snprintf(m, sizeof(m), "%s", PQerrorMessage(pgc)); PQclear(r); fatal(ERR_DB_SCHEMA, "postgres schema error (groups): %s", m); }
        PQclear(r);
        r = PQexec(pgc, "CREATE TABLE IF NOT EXISTS articles (id BIGSERIAL PRIMARY KEY, artnum BIGINT NOT NULL DEFAULT 0, subject TEXT NOT NULL DEFAULT '', author TEXT NOT NULL DEFAULT '', date TEXT NOT NULL DEFAULT '', message_id TEXT NOT NULL DEFAULT '', refs TEXT NOT NULL DEFAULT '', bytes BIGINT NOT NULL DEFAULT 0, line_count INT NOT NULL DEFAULT 0, group_name TEXT NOT NULL DEFAULT '');");
        if (PQresultStatus(r) != PGRES_COMMAND_OK) { char m[256]; snprintf(m, sizeof(m), "%s", PQerrorMessage(pgc)); PQclear(r); fatal(ERR_DB_SCHEMA, "postgres schema error (articles): %s", m); }
        PQclear(r);
        r = PQexec(pgc, "CREATE UNIQUE INDEX IF NOT EXISTS idx_articles_group_artnum ON articles(group_name, artnum);");
        if (PQresultStatus(r) != PGRES_COMMAND_OK) { char m[256]; snprintf(m, sizeof(m), "%s", PQerrorMessage(pgc)); PQclear(r); fatal(ERR_DB_SCHEMA, "postgres schema error (index): %s", m); }
        PQclear(r);
        r = PQexec(pgc, "CREATE INDEX IF NOT EXISTS idx_articles_subject ON articles(subject);"); PQclear(r);
        r = PQexec(pgc, "CREATE INDEX IF NOT EXISTS idx_articles_author ON articles(author);"); PQclear(r);
        r = PQexec(pgc, "CREATE INDEX IF NOT EXISTS idx_articles_message_id ON articles(message_id);"); PQclear(r);
        char settings_sql[320];
        snprintf(settings_sql, sizeof(settings_sql), "CREATE TABLE IF NOT EXISTS \"%s\" (key TEXT PRIMARY KEY NOT NULL DEFAULT '', value TEXT NOT NULL DEFAULT '');", g_settings_table);
        r = PQexec(pgc, settings_sql);
        if (PQresultStatus(r) != PGRES_COMMAND_OK) { char m[256]; snprintf(m, sizeof(m), "%s", PQerrorMessage(pgc)); PQclear(r); fatal(ERR_DB_SCHEMA, "postgres schema error (settings): %s", m); }
        PQclear(r);
    }
#endif
}

/* Insert group info */
void db_insert_group(DB *db, const char *name, long long count, long long first, long long last) {
    if (!name || !*name) return;
    int updated = 0;
    if (db->type == DB_SQLITE && db->sqlite_insert_group) {
        if (sqlite3_bind_int64(db->sqlite_insert_group, 1, count) != SQLITE_OK ||
            sqlite3_bind_int64(db->sqlite_insert_group, 2, first) != SQLITE_OK ||
            sqlite3_bind_int64(db->sqlite_insert_group, 3, last) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_group, 4, name, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            warnf("sqlite bind group-update failed: %s", sqlite3_errmsg(db->sqlite));
            sqlite3_reset(db->sqlite_insert_group); sqlite3_clear_bindings(db->sqlite_insert_group); return;
        }
        if (sqlite3_step(db->sqlite_insert_group) != SQLITE_DONE) {
            warnf("sqlite group update step failed: %s", sqlite3_errmsg(db->sqlite));
        }
        int ch = sqlite3_changes(db->sqlite);
        sqlite3_reset(db->sqlite_insert_group); sqlite3_clear_bindings(db->sqlite_insert_group);
        if (ch > 0) {
            updated = 1;
        } else {
            if (g_upsert && db->sqlite_group_ins) {
                if (sqlite3_bind_text(db->sqlite_group_ins, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_int64(db->sqlite_group_ins, 2, count) != SQLITE_OK ||
                    sqlite3_bind_int64(db->sqlite_group_ins, 3, first) != SQLITE_OK ||
                    sqlite3_bind_int64(db->sqlite_group_ins, 4, last) != SQLITE_OK) {
                    warnf("sqlite bind group-insert failed: %s", sqlite3_errmsg(db->sqlite));
                    sqlite3_reset(db->sqlite_group_ins); sqlite3_clear_bindings(db->sqlite_group_ins);
                } else {
                    if (sqlite3_step(db->sqlite_group_ins) != SQLITE_DONE) {
                        warnf("sqlite group insert step failed: %s", sqlite3_errmsg(db->sqlite));
                    } else if (sqlite3_changes(db->sqlite) > 0) {
                        updated = 1;
                    }
                    sqlite3_reset(db->sqlite_group_ins); sqlite3_clear_bindings(db->sqlite_group_ins);
                    infof("group inserted: %s", name);
                }
            } else {
                warnf("group not found for update: %s", name);
            }
        }
        if (updated) { g_groups_updated++; g_sql_records_updated++; }
        return;
    }
    if (db->type == DB_MYSQL) {
        char esc_name[512];
        mysql_real_escape_string(db->mysql, esc_name, name, (unsigned long)strlen(name));
        char sql[1024];
        snprintf(sql, sizeof(sql), "UPDATE `groups` SET article_count=%lld, first=%lld, last=%lld WHERE name='%s'", count, first, last, esc_name);
        if (mysql_query(db->mysql, sql)) {
            warnf("mysql group update error: %s", mysql_error(db->mysql));
        }
        my_ulonglong ch = mysql_affected_rows(db->mysql);
        if (ch > 0) {
            updated = 1;
        } else {
            if (g_upsert) {
                snprintf(sql, sizeof(sql), "INSERT INTO `groups` (name,article_count,first,last) VALUES ('%s',%lld,%lld,%lld) ON DUPLICATE KEY UPDATE article_count=VALUES(article_count),first=VALUES(first),last=VALUES(last)", esc_name, count, first, last);
                if (mysql_query(db->mysql, sql)) {
                    warnf("mysql group insert error: %s", mysql_error(db->mysql));
                } else {
                    updated = 1;
                    infof("group inserted: %s", name);
                }
            } else {
                warnf("group not found for update: %s", name);
            }
        }
        if (updated) { g_groups_updated++; g_sql_records_updated++; }
        return;
    }
    #ifdef HAVE_PQ
    if (db->type == DB_POSTGRES) {
        PGconn *pgc = (PGconn*)db->impl; if (!pgc) return;
        const char *pupd[4]; char cbuf[32], fbuf[32], lbuf[32];
        snprintf(cbuf,sizeof(cbuf),"%lld",count); snprintf(fbuf,sizeof(fbuf),"%lld",first); snprintf(lbuf,sizeof(lbuf),"%lld",last);
        pupd[0]=cbuf; pupd[1]=fbuf; pupd[2]=lbuf; pupd[3]=name;
        PGresult *r = PQexecParams(pgc, "UPDATE groups SET article_count=$1, first=$2, last=$3 WHERE name=$4", 4, NULL, pupd, NULL, NULL, 0);
        int ok = (PQresultStatus(r)==PGRES_COMMAND_OK && atoll(PQcmdTuples(r)) > 0); PQclear(r);
        if (ok) updated = 1;
        if (!ok && g_upsert){
            const char *pins[4]; pins[0]=name; pins[1]=cbuf; pins[2]=fbuf; pins[3]=lbuf;
            r = PQexecParams(pgc, "INSERT INTO groups (name, article_count, first, last) VALUES ($1,$2,$3,$4) ON CONFLICT(name) DO UPDATE SET article_count=EXCLUDED.article_count, first=EXCLUDED.first, last=EXCLUDED.last", 4, NULL, pins, NULL, NULL, 0);
            if (PQresultStatus(r)==PGRES_COMMAND_OK && atoll(PQcmdTuples(r)) > 0) updated = 1;
            PQclear(r);
        }
        if (updated) { g_groups_updated++; g_sql_records_updated++; }
        return;
    }
    #endif
}

/* Runs one duplicate-removal statement; returns rows deleted (0 if the table does not exist yet). */
static long long db_delete_rows(DB *db, const char *sqlite_sql, const char *mysql_sql, const char *pg_sql) {
    if (db->type == DB_SQLITE) {
        char *err = NULL;
        if (sqlite3_exec(db->sqlite, sqlite_sql, NULL, NULL, &err) != SQLITE_OK) {
            infof("dedupe skipped: %s", err ? err : "sqlite error");
            sqlite3_free(err);
            return 0;
        }
        return sqlite3_changes(db->sqlite);
    }
    if (db->type == DB_MYSQL) {
        if (mysql_query(db->mysql, mysql_sql)) { infof("dedupe skipped: %s", mysql_error(db->mysql)); return 0; }
        return (long long)mysql_affected_rows(db->mysql);
    }
#ifdef HAVE_PQ
    if (db->type == DB_POSTGRES) {
        PGresult *r = PQexec((PGconn *)db->impl, pg_sql);
        long long n = 0;
        if (PQresultStatus(r) == PGRES_COMMAND_OK) n = atoll(PQcmdTuples(r));
        else infof("dedupe skipped: %s", PQerrorMessage((PGconn *)db->impl));
        PQclear(r);
        return n;
    }
#else
    (void)pg_sql;
#endif
    return 0;
}

/* Removes duplicate article rows, keeping the oldest row (lowest id) of each set:
 *   1. the same article number within a group
 *   2. the same Message-ID within a group (the same post stored under another number)
 * Cross-posts (same Message-ID in different groups) are legitimate and are kept.
 * Must run before db_init_schema() so the unique (group_name, artnum) index can be created. */
static long long db_remove_duplicates(DB *db) {
    long long by_num = db_delete_rows(db,
        "DELETE FROM articles WHERE id NOT IN (SELECT MIN(id) FROM articles GROUP BY group_name, artnum)",
        "DELETE a FROM `articles` a JOIN `articles` b ON a.group_name=b.group_name AND a.artnum=b.artnum AND a.id>b.id",
        "DELETE FROM articles a USING articles b WHERE a.group_name=b.group_name AND a.artnum=b.artnum AND a.id>b.id");
    long long by_msgid = db_delete_rows(db,
        "DELETE FROM articles WHERE message_id<>'' AND id NOT IN "
        "(SELECT MIN(id) FROM articles WHERE message_id<>'' GROUP BY group_name, message_id)",
        "DELETE a FROM `articles` a JOIN `articles` b ON a.group_name=b.group_name AND a.message_id=b.message_id "
        "AND a.id>b.id WHERE a.message_id<>''",
        "DELETE FROM articles a USING articles b WHERE a.group_name=b.group_name AND a.message_id=b.message_id "
        "AND a.id>b.id AND a.message_id<>''");
    fprintf(stdout, "Duplicates removed: %lld with the same article number, %lld with the same Message-ID\n",
            by_num, by_msgid);
    return by_num + by_msgid;
}

static void db_set_setting(DB *db, const char *key, const char *val) {
    if (!key) return;
    if (!val) val = "";
    if (db->type == DB_SQLITE) {
        sqlite3_stmt *st = NULL;
        char sql[320];
        snprintf(sql, sizeof(sql), "INSERT INTO \"%s\" (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", g_settings_table);
        if (sqlite3_prepare_v2(db->sqlite, sql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, val, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    } else if (db->type == DB_MYSQL) {
        char ek[256], ev[1024], sql[1536];
        mysql_real_escape_string(db->mysql, ek, key, (unsigned long)strlen(key));
        mysql_real_escape_string(db->mysql, ev, val, (unsigned long)strlen(val));
        snprintf(sql, sizeof(sql), "INSERT INTO `%s` (`key`, `value`) VALUES ('%s', '%s') ON DUPLICATE KEY UPDATE `value`=VALUES(`value`)", g_settings_table, ek, ev);
        mysql_query(db->mysql, sql);
    }
#ifdef HAVE_PQ
    else if (db->type == DB_POSTGRES) {
        PGconn *pgc = (PGconn*)db->impl;
        if (pgc) {
            const char *params[2] = { key, val };
            char sql[320];
            snprintf(sql, sizeof(sql), "INSERT INTO \"%s\" (key, value) VALUES ($1, $2) ON CONFLICT(key) DO UPDATE SET value=EXCLUDED.value", g_settings_table);
            PGresult *r = PQexecParams(pgc, sql, 2, NULL, params, NULL, NULL, 0);
            PQclear(r);
        }
    }
#endif
}

static void db_ensure_group_skeleton(DB *db, const char *name) {
    if (!name || !*name) return;
    if (db->type == DB_SQLITE) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->sqlite, "INSERT OR IGNORE INTO groups (name, article_count, first, last) VALUES (?, 0, 0, 0)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    } else if (db->type == DB_MYSQL) {
        char esc[512], sql[1024];
        mysql_real_escape_string(db->mysql, esc, name, (unsigned long)strlen(name));
        snprintf(sql, sizeof(sql), "INSERT IGNORE INTO `groups` (name, article_count, first, last) VALUES ('%s', 0, 0, 0)", esc);
        mysql_query(db->mysql, sql);
    }
#ifdef HAVE_PQ
    else if (db->type == DB_POSTGRES) {
        PGconn *pgc = (PGconn*)db->impl;
        if (pgc) {
            const char *params[1] = { name };
            PGresult *r = PQexecParams(pgc, "INSERT INTO groups (name, article_count, first, last) VALUES ($1, 0, 0, 0) ON CONFLICT(name) DO NOTHING", 1, NULL, params, NULL, NULL, 0);
            PQclear(r);
        }
    }
#endif
}

static void db_seed_skeleton(DB *db, const char *host, const char *port, int use_ssl, int do_starttls,
                             const char *group, const char *group_list_path,
                             int headers_only, int threads, int retries, long long limit, int progress_width) {
    char buf[64];
    db_set_setting(db, "schema_version", "1");
    db_set_setting(db, "host", host && *host ? host : "localhost");
    db_set_setting(db, "port", port && *port ? port : (use_ssl ? "563" : "119"));
    snprintf(buf, sizeof(buf), "%d", use_ssl ? 1 : 0);
    db_set_setting(db, "ssl", buf);
    snprintf(buf, sizeof(buf), "%d", do_starttls ? 1 : 0);
    db_set_setting(db, "starttls", buf);
    snprintf(buf, sizeof(buf), "%d", headers_only ? 1 : 0);
    db_set_setting(db, "headers_only", buf);
    snprintf(buf, sizeof(buf), "%d", threads);
    db_set_setting(db, "threads", buf);
    snprintf(buf, sizeof(buf), "%d", retries);
    db_set_setting(db, "retries", buf);
    snprintf(buf, sizeof(buf), "%lld", limit);
    db_set_setting(db, "limit", buf);
    snprintf(buf, sizeof(buf), "%d", progress_width);
    db_set_setting(db, "progress_width", buf);
    db_set_setting(db, "upsert", "1");
    db_set_setting(db, "group", group && *group ? group : "");

    if (group && *group) {
        db_ensure_group_skeleton(db, group);
    }
    if (group_list_path && *group_list_path) {
        FILE *f = fopen(group_list_path, "r");
        if (!f) fatal(ERR_ARGS, "Could not open group list file '%s': %s", group_list_path, strerror(errno));
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p && isspace((unsigned char)*p)) p++;
            size_t len = strlen(p);
            while (len > 0 && isspace((unsigned char)p[len - 1])) p[--len] = '\0';
            if (*p && *p != '#' && *p != ';') {
                db_ensure_group_skeleton(db, p);
            }
        }
        fclose(f);
    }
}

/* Length of the valid UTF-8 sequence at s (1-4), or 0 if the bytes there are not valid UTF-8. */
static int utf8_seq_len(const unsigned char *s) {
    if (s[0] < 0x80) return 1;
    if (s[0] >= 0xC2 && s[0] <= 0xDF) return (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (s[0] == 0xE0 && s[1] < 0xA0) return 0;  /* overlong */
        if (s[0] == 0xED && s[1] >= 0xA0) return 0; /* surrogate */
        return 3;
    }
    if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (s[0] == 0xF0 && s[1] < 0x90) return 0;  /* overlong */
        if (s[0] == 0xF4 && s[1] >= 0x90) return 0; /* beyond U+10FFFF */
        return 4;
    }
    return 0;
}

/* Returns a malloc'd, always-valid UTF-8 copy of s. Usenet headers are often raw Latin-1 or
 * Windows-1252 (e.g. a 0xB7 middle dot), which strict utf8mb4/PostgreSQL columns reject, so
 * each invalid byte is decoded as Windows-1252 and re-encoded as UTF-8. Valid text is unchanged. */
static char *utf8_clean(const char *s) {
    static const unsigned short cp1252[32] = {
        0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
        0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178};
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1); /* each invalid byte grows to at most 3 bytes */
    if (!out) fatal(ERR_RUNTIME, "Out of memory sanitizing text");
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        int len = utf8_seq_len(p);
        if (len) { memcpy(out + o, p, (size_t)len); o += (size_t)len; p += len; continue; }
        unsigned cp = (*p >= 0x80 && *p <= 0x9F) ? cp1252[*p - 0x80] : *p;
        if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        p++;
    }
    out[o] = '\0';
    return out;
}

/* All worker writes are serialized; return failure to the download coordinator. */
static int db_store_article(DB *db, const char *group, long long artnum, const char *subject,
                            const char *author, const char *date, const char *message_id,
                            const char *references, long long bytes, int lines) {
    /* Sanitize every text field so one bad byte cannot make the database reject the whole row. */
    char *clean[6] = {
        subject ? utf8_clean(subject) : NULL, author ? utf8_clean(author) : NULL, date ? utf8_clean(date) : NULL,
        message_id ? utf8_clean(message_id) : NULL, references ? utf8_clean(references) : NULL, group ? utf8_clean(group) : NULL};
    const char *values[9] = {clean[0], clean[1], clean[2], clean[3], clean[4], NULL, NULL, clean[5], NULL};
    int ok = 0;
    if (db->type == DB_SQLITE) {
        sqlite3_stmt *stmt = g_upsert ? db->sqlite_article_ins : db->sqlite_insert_article;
        if (stmt) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            ok = 1;
            for (int i = 0; i < 9; ++i) {
                int rc;
                if (values[i]) {
                    rc = sqlite3_bind_text(stmt, i+1, values[i], -1, SQLITE_TRANSIENT);
                } else if (i == 6) {
                    rc = sqlite3_bind_int(stmt, i+1, lines);
                } else {
                    rc = sqlite3_bind_int64(stmt, i+1, i == 5 ? bytes : artnum);
                }
                if (rc != SQLITE_OK) ok = 0;
            }
            if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db->sqlite) > 0;
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
        if (!ok) warnf("Article write failed for %s #%lld (use --upsert for new articles): %s", group, artnum, sqlite3_errmsg(db->sqlite));
    } else if (db->type == DB_MYSQL) {
        MYSQL_STMT *stmt = g_upsert ? db->mysql_article_ins : db->mysql_insert_article;
        if (stmt) {
            MYSQL_BIND binds[9] = {0};
            for (int i = 0; i < 9; ++i) {
                if (values[i]) {
                    binds[i].buffer_type = MYSQL_TYPE_STRING;
                    binds[i].buffer = (void *)values[i];
                    binds[i].buffer_length = (unsigned long)strlen(values[i]);
                } else if (i == 6) {
                    binds[i].buffer_type = MYSQL_TYPE_LONG;
                    binds[i].buffer = (void *)&lines;
                    binds[i].buffer_length = sizeof(int);
                } else {
                    binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                    binds[i].buffer = i == 5 ? (void *)&bytes : (void *)&artnum;
                    binds[i].buffer_length = sizeof(long long);
                }
            }
            ok = mysql_stmt_bind_param(stmt, binds) == 0 && mysql_stmt_execute(stmt) == 0;
            if (ok && !g_upsert) ok = mysql_stmt_affected_rows(stmt) > 0;
        }
        if (!ok) warnf("Article write failed for %s #%lld: %s", group, artnum, stmt ? mysql_stmt_error(stmt) : mysql_error(db->mysql));
    }
#ifdef HAVE_PQ
    else if (db->type == DB_POSTGRES) {
        const char *update = "UPDATE articles SET subject=$1, author=$2, date=$3, message_id=$4, refs=$5, bytes=$6, line_count=$7 WHERE group_name=$8 AND artnum=$9";
        const char *insert = "INSERT INTO articles (subject,author,date,message_id,refs,bytes,line_count,group_name,artnum) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9) "
                             "ON CONFLICT(group_name,artnum) DO UPDATE SET subject=excluded.subject,author=excluded.author,date=excluded.date,"
                             "message_id=excluded.message_id,refs=excluded.refs,bytes=excluded.bytes,line_count=excluded.line_count";
        char b[32], l[32], a[32];
        snprintf(b, sizeof(b), "%lld", bytes); snprintf(l, sizeof(l), "%d", lines); snprintf(a, sizeof(a), "%lld", artnum);
        values[5] = b; values[6] = l; values[8] = a;
        PGresult *r = PQexecParams((PGconn *)db->impl, g_upsert ? insert : update, 9, NULL, values, NULL, NULL, 0);
        ok = PQresultStatus(r) == PGRES_COMMAND_OK && atoll(PQcmdTuples(r)) > 0;
        if (!ok) warnf("Article write failed: %s", PQresultErrorMessage(r));
        PQclear(r);
    }
#endif
    for (int i = 0; i < 6; ++i) free(clean[i]);
    if (ok) {
        g_articles_updated++;
        g_sql_records_updated++;
    }
    return ok;
}

void db_insert_article(DB *db, const char *group, long long artnum, const char *subject,
                       const char *author, const char *date, const char *message_id,
                       const char *references, long long bytes, int lines) {
    (void)db_store_article(db, group, artnum, subject, author, date, message_id, references, bytes, lines);
}

/* NNTP commands */

/* Authenticate using AUTHINFO USER/PASS */
static int nntp_auth(Conn *c, const char *user, const char *pass) {
    char resp[BUFSZ];
    conn_sendf(c, "AUTHINFO USER %s", user);
    if (conn_readline(c, resp, sizeof(resp)) <= 0) return -1;
    int code = atoi(resp);
    if (code == 381) {
        conn_sendf(c, "AUTHINFO PASS %s", pass);
        if (conn_readline(c, resp, sizeof(resp)) <= 0) return -1;
        return atoi(resp);
    }
    return code;
}

/* request STARTTLS */
static int nntp_starttls(Conn *c) {
    char resp[BUFSZ];
    conn_sendf(c, "STARTTLS");
    if (conn_readline(c, resp, sizeof(resp)) <= 0) return -1;
    return atoi(resp);
}

static int nntp_group(Conn *c, const char *group, long long *count, long long *first, long long *last) {
    char buf[BUFSZ];
    conn_sendf(c, "GROUP %s", group);
    if (conn_readline(c, buf, sizeof(buf)) <= 0) return -1;
    int code = atoi(buf);
    if (code >= 200 && code < 300) {
        /* parse: 211 <count> <first> <last> <groupname> (64-bit article numbers) */
        long long cnt = 0, f = 0, l = 0;
        sscanf(buf, "%*d %lld %lld %lld", &cnt, &f, &l);
        if (count) *count = cnt;
        if (first) *first = f;
        if (last) *last = l;
    }
    return code;
}

/* fetch XOVER for a range; returns malloc'd content with lines separated by \n */
static char *nntp_xover(Conn *c, long long first, long long last) {
    char buf[BUFSZ];
    c->last_code = 0;
    if (conn_sendf(c, "XOVER %lld-%lld", first, last) <= 0) return NULL;
    if (conn_readline(c, buf, sizeof(buf)) <= 0) return NULL;
    size_t blen = strlen(buf);
    while (blen && (buf[blen-1] == '\r' || buf[blen-1] == '\n')) buf[--blen] = '\0';
    int code = c->last_code = atoi(buf);
    if (code != 224) {
        if (code == 423) infof("XOVER empty range %lld-%lld: %s", first, last, buf);
        else warnf("XOVER rejected: %s", buf);
        return NULL;
    }
    return conn_read_multiline_ex(c, 1);
}

/* fetch full header for article number */
static char *nntp_head(Conn *c, long long artnum) {
    char buf[BUFSZ];
    c->last_code = 0;
    if (conn_sendf(c, "HEAD %lld", artnum) <= 0) return NULL;
    if (conn_readline(c, buf, sizeof(buf)) <= 0) return NULL;
    size_t blen = strlen(buf);
    while (blen && (buf[blen-1] == '\r' || buf[blen-1] == '\n')) buf[--blen] = '\0';
    int code = c->last_code = atoi(buf);
    if (code != 221) {
        if (code == 423 || code == 430) infof("HEAD missing for %lld: %s", artnum, buf);
        else warnf("HEAD rejected for %lld: %s", artnum, buf);
        return NULL;
    }
    return conn_read_multiline(c);
}

/* parse XOVER line: fields are tab-separated. Common format:
   article-number TAB subject TAB author TAB date TAB message-id TAB references TAB bytes TAB lines TAB xref
   We'll read first 8 fields.
*/
static int parse_xover_line(const char *line, long long *artnum, char **subject, char **author, char **date,
                            char **message_id, char **references, long long *bytes, int *lines) {
    char *tmp = strdup(line);
    if (!tmp) fatal(ERR_RUNTIME, "Out of memory parsing overview");
    char *fields[8], *p = tmp;
    for (int i = 0; i < 8; ++i) {
        fields[i] = p;
        char *tab = strchr(p, '\t');
        if (tab) { *tab = '\0'; p = tab + 1; }
        else if (i < 7) { free(tmp); return 0; }
    }
    char *end = NULL;
    errno = 0;
    long long id = strtoll(fields[0], &end, 10);
    if (errno == ERANGE || !end || *end || id < 1) { free(tmp); return 0; }
    *artnum = id;
    *subject = strdup(fields[1]); *author = strdup(fields[2]); *date = strdup(fields[3]);
    *message_id = strdup(fields[4]); *references = strdup(fields[5]);
    *bytes = atoll(fields[6]); *lines = atoi(fields[7]);
    free(tmp);
    if (!*subject || !*author || !*date || !*message_id || !*references)
        fatal(ERR_RUNTIME, "Out of memory copying overview");
    return 1;
}

static void strip_field_ws(char *s) {
    if (!s) return;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = '\0';
}

/* Strip header values: find "Subject: ..." lines etc., supporting RFC 5322 folded headers */
static void extract_from_headers(const char *hdrs, char **subject, char **from, char **date,
                                 char **message_id, char **references, long long *bytes, int *lines) {
    *subject = strdup("");
    *from = strdup("");
    *date = strdup("");
    *message_id = strdup("");
    *references = strdup("");
    *bytes = 0; *lines = 0;
    char *copy = strdup(hdrs);
    if (!copy) fatal(ERR_RUNTIME, "Out of memory parsing headers");
    char *save = NULL;
    char **current = NULL;
    for (char *raw = strtok_r(copy, "\n", &save); raw; raw = strtok_r(NULL, "\n", &save)) {
        if ((*raw == ' ' || *raw == '\t') && current && *current) {
            char *cont = raw;
            while (*cont == ' ' || *cont == '\t') cont++;
            size_t clen = strlen(cont);
            while (clen && (cont[clen-1] == ' ' || cont[clen-1] == '\t' || cont[clen-1] == '\r')) cont[--clen] = '\0';
            if (clen > 0) {
                size_t old_len = strlen(*current);
                char *grown = realloc(*current, old_len + clen + 2);
                if (grown) {
                    *current = grown;
                    if (old_len > 0) (*current)[old_len++] = ' ';
                    memcpy(*current + old_len, cont, clen + 1);
                }
            }
            continue;
        }
        current = NULL;
        char *line = raw;
        while (*line && isspace((unsigned char)*line)) line++;
        if (strncasecmp(line, "Subject:", 8) == 0) {
            free(*subject); *subject = strdup(line + 8); strip_field_ws(*subject); current = subject;
        } else if (strncasecmp(line, "From:", 5) == 0) {
            free(*from); *from = strdup(line + 5); strip_field_ws(*from); current = from;
        } else if (strncasecmp(line, "Date:", 5) == 0) {
            free(*date); *date = strdup(line + 5); strip_field_ws(*date); current = date;
        } else if (strncasecmp(line, "Message-ID:", 11) == 0) {
            free(*message_id); *message_id = strdup(line + 11); strip_field_ws(*message_id); current = message_id;
        } else if (strncasecmp(line, "References:", 11) == 0) {
            free(*references); *references = strdup(line + 11); strip_field_ws(*references); current = references;
        } else if (strncasecmp(line, "Lines:", 6) == 0) {
            *lines = atoi(line + 6);
        } else if (strncasecmp(line, "Bytes:", 6) == 0) {
            *bytes = atoll(line + 6);
        }
    }
    free(copy);
}

/* A balanced binary range tree records batch outcomes. Its leaves also form a
 * linked FIFO, so workers can claim batches without allocating per article. */
typedef struct WorkNode {
    long long first, last;
    int failed;
    struct WorkNode *left, *right, *next;
} WorkNode;

typedef struct RootList {
    WorkNode *root;
    struct RootList *next;
} RootList;

typedef struct {
    WorkNode *root, *head, *tail;
    RootList *extra_roots;
    long long batch_size;
    long long group_first;
    long long next_backfill_last;
    /* Remaining forward range not yet expanded into nodes. Groups can span billions of
     * article numbers, so nodes are built one bounded window at a time, never up front. */
    long long fwd_next, fwd_last;
    pthread_mutex_t mutex;
} WorkQueue;

#define QUEUE_WINDOW_BATCHES 1024

/* Expands the next window of the forward range into work nodes. Caller holds q->mutex (or is
 * single-threaded). Returns 1 if a window was added. */
static int queue_expand_forward(WorkQueue *q);

static void tree_free(WorkNode *node) {
    if (!node) return;
    tree_free(node->left); tree_free(node->right); free(node);
}

static void queue_free_all(WorkQueue *q) {
    if (!q) return;
    tree_free(q->root);
    q->root = NULL;
    RootList *cur = q->extra_roots;
    while (cur) {
        RootList *next = cur->next;
        tree_free(cur->root);
        free(cur);
        cur = next;
    }
    q->extra_roots = NULL;
}

static WorkNode *tree_build(WorkQueue *q, long long first, long long last) {
    WorkNode *node = calloc(1, sizeof(*node));
    if (!node) fatal(ERR_RUNTIME, "Out of memory building work queue");
    node->first = first; node->last = last;
    if (last - first >= q->batch_size) {
        long long mid = first + (last - first) / 2;
        node->left = tree_build(q, first, mid);
        node->right = tree_build(q, mid + 1, last);
    } else {
        if (q->tail) q->tail->next = node; else q->head = node;
        q->tail = node;
    }
    return node;
}

static int queue_expand_forward(WorkQueue *q) {
    if (q->fwd_next > q->fwd_last) return 0;
    long long span = q->batch_size * QUEUE_WINDOW_BATCHES;
    if (span < q->batch_size) span = q->batch_size; /* overflow guard */
    long long win_last = (q->fwd_last - q->fwd_next + 1 > span) ? q->fwd_next + span - 1 : q->fwd_last;
    WorkNode *root = tree_build(q, q->fwd_next, win_last);
    q->fwd_next = win_last + 1;
    if (!q->root) { q->root = root; return 1; }
    RootList *rl = calloc(1, sizeof(*rl));
    if (!rl) fatal(ERR_RUNTIME, "Out of memory building work queue");
    rl->root = root; rl->next = q->extra_roots; q->extra_roots = rl;
    return 1;
}

static int tree_failed(const WorkNode *node) {
    if (!node) return 0;
    return node->failed || tree_failed(node->left) || tree_failed(node->right);
}

static int queue_any_tree_failed(const WorkQueue *q) {
    if (!q) return 0;
    if (tree_failed(q->root)) return 1;
    for (const RootList *r = q->extra_roots; r; r = r->next) {
        if (tree_failed(r->root)) return 1;
    }
    return 0;
}

typedef struct {
    DB *db;
    const char *group, *host, *port, *user, *pass;
    int retries, use_ssl, do_starttls, headers_only, progress_width;
    int total_threads, active_threads;
    int any_failed;
    unsigned int tick;
    double start_time;
    double last_render_time;
    long long target_articles;
    long long total, completed, stored, missing, bytes_downloaded;
    char last_item[160];
    pthread_mutex_t mutex;
    WorkQueue *queue;
} WorkerArgs;

static WorkNode *queue_pop(WorkerArgs *wa) {
    if (g_stop_requested) return NULL;
    WorkQueue *q = wa->queue;
    pthread_mutex_lock(&q->mutex);
    if (wa->target_articles > 0) {
        pthread_mutex_lock(&wa->mutex);
        long long stored_now = wa->stored;
        int failed_now = wa->any_failed;
        pthread_mutex_unlock(&wa->mutex);
        if (stored_now >= wa->target_articles) {
            q->head = q->tail = NULL;
            pthread_mutex_unlock(&q->mutex);
            return NULL;
        }
        if (!q->head) queue_expand_forward(q);
        if (!q->head && !failed_now && q->next_backfill_last >= q->group_first) {
            long long needed = wa->target_articles - stored_now;
            long long min_chunk = q->batch_size * (wa->total_threads > 0 ? wa->total_threads : 1);
            if (needed < min_chunk) needed = min_chunk;
            long long bf_last = q->next_backfill_last;
            long long avail = bf_last - q->group_first + 1;
            long long bf_first = (avail > needed) ? (bf_last - needed + 1) : q->group_first;
            q->next_backfill_last = bf_first - 1;
            /* Enqueue highest (newest) ranges first when backfilling downward */
            long long cur_last = bf_last;
            while (cur_last >= bf_first) {
                long long span = q->batch_size * 16;
                if (span < q->batch_size) span = q->batch_size;
                long long cur_first = (cur_last - bf_first + 1 > span) ? (cur_last - span + 1) : bf_first;
                WorkNode *extra = tree_build(q, cur_first, cur_last);
                RootList *rl = calloc(1, sizeof(*rl));
                if (rl) {
                    rl->root = extra;
                    rl->next = q->extra_roots;
                    q->extra_roots = rl;
                }
                cur_last = cur_first - 1;
            }
        }
    }
    if (!q->head && wa->target_articles == 0) queue_expand_forward(q);
    WorkNode *node = g_stop_requested ? NULL : q->head;
    if (node) q->head = node->next;
    if (!q->head) q->tail = NULL;
    pthread_mutex_unlock(&q->mutex);
    return node;
}

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static void format_duration(double seconds, char *buf, size_t sz) {
    if (seconds < 0.0 || seconds > 359999.0) {
        snprintf(buf, sz, "--:--");
        return;
    }
    long total_sec = (long)(seconds + 0.5);
    long hrs = total_sec / 3600;
    long mins = (total_sec % 3600) / 60;
    long secs = total_sec % 60;
    if (hrs > 0) snprintf(buf, sz, "%02ld:%02ld:%02ld", hrs, mins, secs);
    else snprintf(buf, sz, "%02ld:%02ld", mins, secs);
}

static void format_byte_rate(double bytes_per_sec, char *buf, size_t sz) {
    if (bytes_per_sec >= 1048576.0)
        snprintf(buf, sz, "%.2f MB/s", bytes_per_sec / 1048576.0);
    else if (bytes_per_sec >= 1024.0)
        snprintf(buf, sz, "%.1f KB/s", bytes_per_sec / 1024.0);
    else
        snprintf(buf, sz, "%.0f B/s", bytes_per_sec);
}

static int contains_nocase(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return 0;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        if (strncasecmp(p, needle, nlen) == 0) return 1;
    }
    return 0;
}

/* Format current file or header/article label cleanly without newlines/tabs */
static void update_last_item(WorkerArgs *wa, long long number, const char *subject) {
    char clean[128];
    size_t j = 0;
    if (subject && *subject) {
        const char *q1 = strchr(subject, '"');
        const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        int is_yenc = contains_nocase(subject, "yenc");
        if (is_yenc && q1 && q2 && q2 > q1 + 1) {
            size_t flen = (size_t)(q2 - q1 - 1);
            if (flen >= sizeof(clean)) flen = sizeof(clean) - 1;
            for (size_t i = 0; i < flen; ++i) {
                unsigned char ch = (unsigned char)q1[1 + i];
                clean[j++] = (ch < 32 || ch == 127) ? ' ' : (char)ch;
            }
            clean[j] = '\0';
            snprintf(wa->last_item, sizeof(wa->last_item), "File: \"%s\" (#%lld)", clean, number);
            return;
        }
        for (size_t i = 0; subject[i] && j + 1 < sizeof(clean); ++i) {
            unsigned char ch = (unsigned char)subject[i];
            clean[j++] = (ch < 32 || ch == 127) ? ' ' : (char)ch;
        }
        clean[j] = '\0';
        snprintf(wa->last_item, sizeof(wa->last_item), "%s #%lld: %s",
                 is_yenc ? "File" : (wa->headers_only ? "Hdr" : "Art"), number, clean);
    } else {
        snprintf(wa->last_item, sizeof(wa->last_item), "%s #%lld",
                 wa->headers_only ? "Hdr" : "Art", number);
    }
}

static int terminal_columns(void) {
    struct winsize ws;
    if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20)
        return (int)ws.ws_col;
    const char *env = getenv("COLUMNS");
    if (env && atoi(env) > 20) return atoi(env);
    return 0;
}

/* Single-line in-place terminal progress bar (caller holds wa->mutex).
 * Truncates to terminal width and clears to end-of-line so it never wraps or scrolls,
 * while guaranteeing that Time Remaining (ETA) is never cut off on narrower terminals. */
static void render_progress(WorkerArgs *wa, int final) {
    double now = now_sec();
    if (!final && wa->last_render_time > 0.0 && (now - wa->last_render_time) < 0.025) {
        return;
    }
    wa->last_render_time = now;

    static const char spin[] = "|/-\\";
    char indicator = final ? '*' : spin[wa->tick++ & 3];
    double elapsed = now - wa->start_time;
    if (elapsed < 0.001) elapsed = 0.001;
    double item_rate = (double)wa->completed / elapsed;
    double byte_rate = (double)wa->bytes_downloaded / elapsed;
    long long remaining = wa->total > wa->completed ? (wa->total - wa->completed) : 0;
    double eta_sec = (item_rate > 0.0 && remaining > 0) ? ((double)remaining / item_rate) : 0.0;
    if (!final && remaining > 0 && item_rate > 0.0 && eta_sec < 1.0) eta_sec = 1.0;
    double pct = wa->total > 0 ? (100.0 * (double)wa->completed / (double)wa->total) : 100.0;
    if (pct > 100.0) pct = 100.0;

    char elapsed_s[32], eta_s[32], bw_s[32];
    format_duration(elapsed, elapsed_s, sizeof(elapsed_s));
    if (final || remaining == 0) snprintf(eta_s, sizeof(eta_s), "00:00");
    else if (wa->completed == 0) snprintf(eta_s, sizeof(eta_s), "--:--");
    else format_duration(eta_sec, eta_s, sizeof(eta_s));
    format_byte_rate(byte_rate, bw_s, sizeof(bw_s));

    int cols = terminal_columns();
    int bar_w = wa->progress_width > 0 ? wa->progress_width : 24;
    if (bar_w > 60) bar_w = 60;
    if (cols > 0 && bar_w > cols / 6) bar_w = cols / 6;
    if (bar_w < 8) bar_w = 8;

    char bar[68];
    int filled = (int)((pct / 100.0) * bar_w + 0.5);
    if (filled > bar_w) filled = bar_w;
    for (int i = 0; i < bar_w; ++i) {
        if (i < filled) bar[i] = '#';
        else if (i == filled && wa->completed > 0 && !final) bar[i] = '>';
        else bar[i] = '-';
    }
    bar[bar_w] = '\0';

    const char *mode_label = wa->headers_only ? "Headers/XOVER" : "Group/HEAD";
    const char *unit_label = wa->headers_only ? "hdr/s" : "art/s";
    const char *item_label = wa->last_item[0] ? wa->last_item : "starting...";

    char prefix[768];
    int plen = snprintf(prefix, sizeof(prefix),
        "[%c] %s (%s) [%s] %5.1f%% %lld/%lld (ok:%lld miss:%lld sql:%lld) | T:%d/%d | %.1f %s (%s) | Elapsed %s ETA %s",
        indicator, wa->group, mode_label, bar, pct,
        wa->completed, wa->total, wa->stored, wa->missing, (long long)g_sql_records_updated,
        wa->active_threads, wa->total_threads,
        item_rate, unit_label, bw_s, elapsed_s, eta_s);
    if (plen < 0) return;

    char line[1024];
    int n = snprintf(line, sizeof(line), "%s | %s", prefix, item_label);
    if (n < 0) return;

    if (cols > 1 && n >= cols) {
        /* Keep the entire stats prefix including Elapsed and ETA (Time Remaining) visible */
        if (plen < cols) {
            line[cols - 1] = '\0';
            n = cols - 1;
        } else {
            /* Compact layout for narrow terminal windows so ETA (Time Remaining) is still shown */
            n = snprintf(line, sizeof(line),
                "[%c] %.16s (%s) [%.8s] %5.1f%% %lld/%lld ok:%lld miss:%lld T:%d/%d %.0f %s Elapsed %s ETA %s",
                indicator, wa->group, mode_label, bar, pct,
                wa->completed, wa->total, wa->stored, wa->missing,
                wa->active_threads, wa->total_threads,
                item_rate, unit_label, elapsed_s, eta_s);
            if (n >= cols) {
                line[cols - 1] = '\0';
                n = cols - 1;
            }
        }
    }
    if (isatty(STDOUT_FILENO))
        fprintf(stdout, "\r%s\033[K", line);
    else
        fprintf(stdout, "\r%s", line);
    fflush(stdout);
}

static int thread_connect(Conn *tc, WorkerArgs *wa) {
    char line[BUFSZ];
    for (int retry = 0; retry < 25 && !g_stop_requested; ++retry) {
        if (retry > 0) {
            struct timespec ts = {0, 5000000};
            nanosleep(&ts, NULL);
            if (g_stop_requested) break;
        }
        conn_init(tc);
        tc->sock = tcp_connect(wa->host, wa->port);
        if (tc->sock < 0) continue;
        if (wa->use_ssl && !conn_starttls(tc)) { conn_cleanup(tc); continue; }
        if (conn_readline(tc, line, sizeof(line)) > 0) break;
        conn_cleanup(tc);
    }
    if (tc->sock < 0 || g_stop_requested) goto fail;
    if (atoi(line) != 200 && atoi(line) != 201) goto fail;
    if (wa->do_starttls && !wa->use_ssl) {
        if (nntp_starttls(tc) != 382 || !conn_starttls(tc)) goto fail;
    }
    if (wa->user && wa->pass && nntp_auth(tc, wa->user, wa->pass) != 281) goto fail;
    if (nntp_group(tc, wa->group, NULL, NULL, NULL) != 211) goto fail;
    return 1;
fail:
    conn_cleanup(tc);
    return 0;
}

static int store_header(WorkerArgs *wa, const char *data, long long number, int overview, int *reached_target) {
    if (reached_target) *reached_target = 0;
    char *subject = NULL, *author = NULL, *date = NULL, *msgid = NULL, *refs = NULL;
    long long bytes = 0;
    int lines = 0;
    if (overview) {
        if (!parse_xover_line(data, &number, &subject, &author, &date, &msgid, &refs, &bytes, &lines)) return 0;
    } else {
        extract_from_headers(data, &subject, &author, &date, &msgid, &refs, &bytes, &lines);
    }
    pthread_mutex_lock(&wa->mutex);
    if (wa->target_articles > 0 && wa->stored >= wa->target_articles) {
        if (reached_target) *reached_target = 1;
        pthread_mutex_unlock(&wa->mutex);
        free(subject); free(author); free(date); free(msgid); free(refs);
        return 1;
    }
    int ok = db_store_article(wa->db, wa->group, number, subject, author, date, msgid, refs, bytes, lines);
    if (ok) {
        wa->stored++;
        update_last_item(wa, number, subject);
        if (wa->target_articles > 0) {
            wa->completed = wa->stored;
            if (wa->stored >= wa->target_articles && reached_target) *reached_target = 1;
        }
    } else {
        wa->any_failed = 1;
    }
    if (overview) {
        if (wa->target_articles <= 0) wa->completed += 1;
        render_progress(wa, 0);
    }
    pthread_mutex_unlock(&wa->mutex);
    free(subject); free(author); free(date); free(msgid); free(refs);
    return ok;
}

static void *header_worker(void *arg) {
    WorkerArgs *wa = arg;
    if (wa->db->type == DB_MYSQL && mysql_thread_init() != 0) {
        warnf("MySQL worker initialization failed");
        return NULL;
    }
    pthread_mutex_lock(&wa->mutex);
    wa->active_threads++;
    render_progress(wa, 0);
    pthread_mutex_unlock(&wa->mutex);

    Conn tc; conn_init(&tc);
    long long prev_bytes = 0;
    WorkNode *job;
    while (!g_stop_requested && (job = queue_pop(wa))) {
        long long end = wa->headers_only ? job->first : job->last;
        for (long long number = job->first; !g_stop_requested; ++number) {
            if (wa->target_articles > 0) {
                pthread_mutex_lock(&wa->mutex);
                int done = (wa->stored >= wa->target_articles);
                pthread_mutex_unlock(&wa->mutex);
                if (done) break;
            }
            char *data = NULL;
            int missing = 0;
            for (int attempt = 0; attempt <= wa->retries && !g_stop_requested; ++attempt) {
                if (tc.sock < 0) {
                    prev_bytes = 0;
                    if (!thread_connect(&tc, wa)) continue;
                }
                data = wa->headers_only ? nntp_xover(&tc, job->first, job->last) : nntp_head(&tc, number);
                missing = tc.last_code == 423 || (!wa->headers_only && tc.last_code == 430);
                if (data || missing || g_stop_requested) break;
                /* Retry on a fresh, authenticated connection, never a partial response. */
                conn_cleanup(&tc);
                prev_bytes = 0;
            }
            long long delta_bytes = tc.bytes_read >= prev_bytes ? (tc.bytes_read - prev_bytes) : tc.bytes_read;
            prev_bytes = tc.bytes_read;

            if (missing) {
                pthread_mutex_lock(&wa->mutex);
                long long span = wa->headers_only ? (job->last - job->first + 1) : 1;
                if (wa->target_articles <= 0) {
                    wa->missing += span;
                    wa->completed += span;
                } else if (!wa->headers_only) {
                    wa->missing += 1;
                }
                wa->bytes_downloaded += delta_bytes;
                snprintf(wa->last_item, sizeof(wa->last_item), "%s #%lld (missing)",
                         wa->headers_only ? "Hdr" : "Art", number);
                render_progress(wa, 0);
                pthread_mutex_unlock(&wa->mutex);
            } else if (!data && g_stop_requested) {
                job->failed = 1;
                pthread_mutex_lock(&wa->mutex);
                wa->bytes_downloaded += delta_bytes;
                render_progress(wa, 0);
                pthread_mutex_unlock(&wa->mutex);
                break;
            } else if (!data) {
                job->failed = 1;
                warnf("Download failed for %s #%lld-%lld", wa->group, number, wa->headers_only ? job->last : number);
                pthread_mutex_lock(&wa->mutex);
                wa->any_failed = 1;
                if (wa->target_articles <= 0) {
                    wa->completed += wa->headers_only ? (job->last - job->first + 1) : 1;
                }
                wa->bytes_downloaded += delta_bytes;
                render_progress(wa, 0);
                pthread_mutex_unlock(&wa->mutex);
            } else if (wa->headers_only) {
                /* Validate the entire batch before writing any rows. Empty columns
                 * are significant; retries must not silently shift XOVER fields. */
                long long span = job->last - job->first + 1;
                unsigned char *seen = calloc((size_t)(span > 0 ? span : 1), 1);
                if (!seen) fatal(ERR_RUNTIME, "Out of memory allocating overview bitmap");
                long long rows = 0;
                int valid = 1;
                char *copy = strdup(data), *save = NULL;
                if (!copy) fatal(ERR_RUNTIME, "Out of memory parsing overview");
                for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                    char *tail = NULL;
                    errno = 0;
                    long long id = strtoll(line, &tail, 10);
                    int tabs = 0;
                    for (const char *p = line; *p; ++p) if (*p == '\t') tabs++;
                    if (errno == ERANGE || !tail || *tail != '\t' || tabs < 7 || id < job->first || id > job->last || seen[id - job->first]) {
                        valid = 0;
                        break;
                    }
                    seen[id - job->first] = 1;
                    rows++;
                }
                free(copy);
                free(seen);
                if (!valid) {
                    job->failed = 1;
                    pthread_mutex_lock(&wa->mutex);
                    wa->any_failed = 1;
                    if (!g_stop_requested && wa->target_articles <= 0) wa->completed += span;
                    wa->bytes_downloaded += delta_bytes;
                    render_progress(wa, 0);
                    pthread_mutex_unlock(&wa->mutex);
                } else {
                    save = NULL;
                    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                        int reached = 0;
                        if (!store_header(wa, line, 0, 1, &reached)) job->failed = 1;
                        if (reached) break;
                    }
                    pthread_mutex_lock(&wa->mutex);
                    if (!g_stop_requested) {
                        if (wa->target_articles <= 0) {
                            wa->completed += (span - rows);
                        }
                    } else {
                        job->failed = 1;
                    }
                    wa->bytes_downloaded += delta_bytes;
                    render_progress(wa, 0);
                    pthread_mutex_unlock(&wa->mutex);
                }
            } else {
                int reached = 0;
                if (!store_header(wa, data, number, 0, &reached)) job->failed = 1;
                pthread_mutex_lock(&wa->mutex);
                if (wa->target_articles <= 0) wa->completed += 1;
                wa->bytes_downloaded += delta_bytes;
                render_progress(wa, 0);
                pthread_mutex_unlock(&wa->mutex);
                if (reached) {
                    free(data);
                    break;
                }
            }
            free(data);
            if (number == end || g_stop_requested) break;
        }
    }
    conn_cleanup(&tc);
    pthread_mutex_lock(&wa->mutex);
    if (wa->active_threads > 0) wa->active_threads--;
    pthread_mutex_unlock(&wa->mutex);
    if (wa->db->type == DB_MYSQL) mysql_thread_end();
    return NULL;
}

/* Calls cb(key, value) for every row of the settings table (SQLite, MySQL/MariaDB or PostgreSQL). */
static void db_each_setting(DB *db, void (*cb)(const char *, const char *, void *), void *ud) {
    if (db->type == DB_SQLITE && db->sqlite) {
        sqlite3_stmt *st = NULL;
        char sql[320];
        snprintf(sql, sizeof(sql), "SELECT key, value FROM \"%s\"", g_settings_table);
        if (sqlite3_prepare_v2(db->sqlite, sql, -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *k = (const char *)sqlite3_column_text(st, 0), *v = (const char *)sqlite3_column_text(st, 1);
                if (k && v) cb(k, v, ud);
            }
            sqlite3_finalize(st);
        }
    } else if (db->type == DB_MYSQL && db->mysql) {
        MYSQL_RES *res = NULL;
        char sql[320];
        snprintf(sql, sizeof(sql), "SELECT `key`, `value` FROM `%s`", g_settings_table);
        if (!mysql_query(db->mysql, sql) && (res = mysql_store_result(db->mysql))) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res))) if (row[0] && row[1]) cb(row[0], row[1], ud);
            mysql_free_result(res);
        }
    }
#ifdef HAVE_PQ
    else if (db->type == DB_POSTGRES && db->impl) {
        char sql[320];
        snprintf(sql, sizeof(sql), "SELECT key, value FROM \"%s\"", g_settings_table);
        PGresult *r = PQexec((PGconn *)db->impl, sql);
        if (PQresultStatus(r) == PGRES_TUPLES_OK)
            for (int i = 0; i < PQntuples(r); ++i) cb(PQgetvalue(r, i, 0), PQgetvalue(r, i, 1), ud);
        PQclear(r);
    }
#endif
}

static void count_any_setting(const char *k, const char *v, void *ud) { (void)k; (void)v; ++*(int *)ud; }

/* Databases made by earlier versions keep their options in a table simply called "settings". The first time a
 * database is opened with the per-database table still empty, those rows are copied across (the old table is left
 * untouched). */
static void db_migrate_legacy_settings(DB *db) {
    if (!strcmp(g_settings_table, "settings")) return;
    int rows = 0;
    db_each_setting(db, count_any_setting, &rows);
    if (rows) return;
    char sql[512];
    int copied = 0;
    if (db->type == DB_SQLITE && db->sqlite) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->sqlite, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='settings'", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0) {
                snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO \"%s\" (key, value) SELECT key, value FROM settings", g_settings_table);
                copied = sqlite3_exec(db->sqlite, sql, NULL, NULL, NULL) == SQLITE_OK ? sqlite3_changes(db->sqlite) : 0;
            }
            sqlite3_finalize(st);
        }
    } else if (db->type == DB_MYSQL && db->mysql) {
        snprintf(sql, sizeof(sql), "INSERT IGNORE INTO `%s` (`key`, `value`) SELECT `key`, `value` FROM `settings`", g_settings_table);
        if (!mysql_query(db->mysql, sql)) copied = (int)mysql_affected_rows(db->mysql); /* fails quietly when there is no old table */
    }
#ifdef HAVE_PQ
    else if (db->type == DB_POSTGRES && db->impl) {
        snprintf(sql, sizeof(sql), "INSERT INTO \"%s\" (key, value) SELECT key, value FROM settings ON CONFLICT DO NOTHING", g_settings_table);
        PGresult *r = PQexec((PGconn *)db->impl, sql);
        if (PQresultStatus(r) == PGRES_COMMAND_OK) copied = atoi(PQcmdTuples(r));
        PQclear(r);
    }
#endif
    if (copied > 0) infof("Copied %d settings from the old 'settings' table into '%s'", copied, g_settings_table);
}

typedef struct {
    const char **host, **port, **user, **pass, **group;
    int *use_ssl, *do_starttls, *headers_only, *threads, *retries, *progress_width;
    long long *limit;
    int has_cli_host, has_cli_port, has_cli_ssl, has_cli_starttls, has_cli_group, has_cli_headers_only,
        has_cli_threads, has_cli_retries, has_cli_limit, has_cli_progress_width;
} StoredDefaults;

static void apply_stored_default(const char *k, const char *v, void *ud) {
    StoredDefaults *d = ud;
    if (!d->has_cli_host && strcmp(k, "host") == 0 && *v) *d->host = strdup(v);
    else if (!d->has_cli_port && strcmp(k, "port") == 0 && *v) *d->port = strdup(v);
    else if (!d->has_cli_ssl && strcmp(k, "ssl") == 0) *d->use_ssl = atoi(v) ? 1 : 0;
    else if (!d->has_cli_starttls && strcmp(k, "starttls") == 0) *d->do_starttls = atoi(v) ? 1 : 0;
    /* Credentials only fill gaps: anything given on the command line or in a config file wins. */
    else if (strcmp(k, "user") == 0 && *v && !*d->user) *d->user = strdup(v);
    else if (strcmp(k, "pass") == 0 && *v && !*d->pass) *d->pass = strdup(v);
    else if (!d->has_cli_group && strcmp(k, "group") == 0 && *v) *d->group = strdup(v);
    else if (!d->has_cli_headers_only && strcmp(k, "headers_only") == 0) *d->headers_only = atoi(v) ? 1 : 0;
    else if (!d->has_cli_threads && strcmp(k, "threads") == 0 && atoi(v) >= 1 && atoi(v) <= 64) *d->threads = atoi(v);
    else if (!d->has_cli_retries && strcmp(k, "retries") == 0 && atoi(v) >= 0 && atoi(v) <= 10) *d->retries = atoi(v);
    else if (!d->has_cli_limit && strcmp(k, "limit") == 0 && atoll(v) >= 0) *d->limit = atoll(v);
    else if (!d->has_cli_progress_width && strcmp(k, "progress_width") == 0 && atoi(v) >= 5 && atoi(v) <= 200) *d->progress_width = atoi(v);
}

static void db_load_skeleton_defaults(DB *db, const char **host, const char **port, int *use_ssl, int *do_starttls,
                                      const char **user, const char **pass,
                                      const char **group, int *headers_only, int *threads, int *retries,
                                      long long *limit, int *progress_width,
                                      int has_cli_host, int has_cli_port, int has_cli_ssl, int has_cli_starttls,
                                      int has_cli_group, int has_cli_headers_only, int has_cli_threads,
                                      int has_cli_retries, int has_cli_limit, int has_cli_progress_width) {
    StoredDefaults d = {host, port, user, pass, group, use_ssl, do_starttls, headers_only, threads, retries,
                        progress_width, limit, has_cli_host, has_cli_port, has_cli_ssl, has_cli_starttls,
                        has_cli_group, has_cli_headers_only, has_cli_threads, has_cli_retries, has_cli_limit,
                        has_cli_progress_width};
    db_each_setting(db, apply_stored_default, &d);
}

static int str_differs(const char *a, const char *b) { return strcmp(a ? a : "", b ? b : "") != 0; }

/* --confirm-db-defaults: show what the database's stored settings would change, ask, and apply them only
 * on a yes. Returns 1 if they were applied. Never blocks a non-interactive run: with no terminal on stdin
 * the stored defaults are skipped. */
static void count_host_setting(const char *k, const char *v, void *ud) { (void)v; if (strcmp(k, "host") == 0) ++*(int *)ud; }

static void print_stored_setting(const char *k, const char *v, void *ud) {
    int secret = strcmp(k, "pass") == 0 || strcmp(k, "db_pass") == 0;
    printf("  %-16s %s\n", k, secret && *v ? "********" : v);
    ++*(int *)ud;
}

static int db_load_defaults_confirmed(DB *db, const char *db_name, int ask, int explicit_read,
                                      const char **host, const char **port, int *use_ssl,
                                      int *do_starttls, const char **user, const char **pass, const char **group,
                                      int *headers_only, int *threads, int *retries, long long *limit, int *progress_width,
                                      int has_cli_host, int has_cli_port, int has_cli_ssl, int has_cli_starttls,
                                      int has_cli_group, int has_cli_headers_only, int has_cli_threads,
                                      int has_cli_retries, int has_cli_limit, int has_cli_progress_width) {
    if (explicit_read) {
        int stored = 0;
        db_each_setting(db, count_host_setting, &stored);
        if (!stored) fatal(ERR_CONFIG, "--load-config-db: database '%s' has no stored settings in '%s' (create them with --save-config-db)", db_name, g_settings_table);
        printf("Settings stored in table '%s' of database '%s':\n", g_settings_table, db_name);
        int shown = 0;
        db_each_setting(db, print_stored_setting, &shown);
        printf("%d settings stored.\n", shown);
        if (explicit_read == 2) return 0; /* read-only request: nothing to apply */
        printf("\n");
    }
    /* Dry run on copies: command-line options (has_cli_*) and anything already set are respected, exactly as a real load would. */
    const char *h = *host, *po = *port, *u = *user, *pw = *pass, *g = *group;
    int ssl = *use_ssl, st = *do_starttls, ho = *headers_only, th = *threads, re = *retries, pwid = *progress_width;
    long long lim = *limit;
    db_load_skeleton_defaults(db, &h, &po, &ssl, &st, &u, &pw, &g, &ho, &th, &re, &lim, &pwid,
                              has_cli_host, has_cli_port, has_cli_ssl, has_cli_starttls, has_cli_group,
                              has_cli_headers_only, has_cli_threads, has_cli_retries, has_cli_limit, has_cli_progress_width);
    int changes = 0;
    char cur[160], now[160];
#define SHOW(label, differs, curfmt, curarg, nowfmt, nowarg) \
    if (differs) { \
        if (!changes++) printf("The database '%s' has stored defaults that would change this run:\n", db_name); \
        snprintf(cur, sizeof(cur), curfmt, curarg); snprintf(now, sizeof(now), nowfmt, nowarg); \
        printf("  %-16s %-28s (currently: %s)\n", label, now, cur); }
    SHOW("host", str_differs(h, *host), "%s", *host ? *host : "not set", "%s", h ? h : "not set")
    SHOW("port", str_differs(po, *port), "%s", *port ? *port : "default", "%s", po ? po : "default")
    SHOW("ssl", ssl != *use_ssl, "%s", *use_ssl ? "on" : "off", "%s", ssl ? "on" : "off")
    SHOW("starttls", st != *do_starttls, "%s", *do_starttls ? "on" : "off", "%s", st ? "on" : "off")
    SHOW("user", str_differs(u, *user), "%s", *user ? *user : "not set", "%s", u ? u : "not set")
    SHOW("pass", str_differs(pw, *pass), "%s", *pass ? "********" : "not set", "%s", pw ? "********" : "not set")
    SHOW("group", str_differs(g, *group), "%s", *group ? *group : "not set", "%s", g ? g : "not set")
    SHOW("headers_only", ho != *headers_only, "%d", *headers_only, "%d", ho)
    SHOW("threads", th != *threads, "%d", *threads, "%d", th)
    SHOW("retries", re != *retries, "%d", *retries, "%d", re)
    SHOW("limit", lim != *limit, "%lld", *limit, "%lld", lim)
    SHOW("progress_width", pwid != *progress_width, "%d", *progress_width, "%d", pwid)
#undef SHOW
    if (!changes) {
        printf("The database '%s' has no stored defaults that differ from this run's settings.\n", db_name);
        return 0;
    }
    if (ask && !isatty(STDIN_FILENO)) {
        printf("Not applying them: stdin is not a terminal, so they cannot be confirmed.\n");
        return 0;
    }
    if (ask) {
        printf("Use these stored defaults? [y/N] ");
        fflush(stdout);
        char answer[16] = "";
        if (!fgets(answer, sizeof(answer), stdin) || g_stop_requested) { printf("\nNot applying them.\n"); return 0; }
        if (answer[0] != 'y' && answer[0] != 'Y') { printf("Not applying them; continuing with this run's settings.\n"); return 0; }
    }
    db_load_skeleton_defaults(db, host, port, use_ssl, do_starttls, user, pass, group, headers_only, threads, retries, limit,
                              progress_width, has_cli_host, has_cli_port, has_cli_ssl, has_cli_starttls, has_cli_group,
                              has_cli_headers_only, has_cli_threads, has_cli_retries, has_cli_limit, has_cli_progress_width);
    printf("Applied the stored defaults.\n");
    return 1;
}

/* Highest article number already stored for a group (0 if none), for --resume. */
static long long db_max_artnum(DB *db, const char *group) {
    long long max_num = 0;
    if (db->type == DB_SQLITE && db->sqlite) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->sqlite, "SELECT MAX(artnum) FROM articles WHERE group_name = ?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, group, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) max_num = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    } else if (db->type == DB_MYSQL && db->mysql) {
        char esc[1024], sql[1280];
        mysql_real_escape_string(db->mysql, esc, group, (unsigned long)strlen(group));
        snprintf(sql, sizeof(sql), "SELECT MAX(`artnum`) FROM `articles` WHERE `group_name`='%s'", esc);
        MYSQL_RES *res = NULL;
        if (!mysql_query(db->mysql, sql) && (res = mysql_store_result(db->mysql))) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row && row[0]) max_num = atoll(row[0]);
            mysql_free_result(res);
        }
    }
#ifdef HAVE_PQ
    else if (db->type == DB_POSTGRES && db->impl) {
        const char *params[1] = {group};
        PGresult *r = PQexecParams((PGconn *)db->impl, "SELECT MAX(artnum) FROM articles WHERE group_name = $1",
                                   1, NULL, params, NULL, NULL, 0);
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0 && !PQgetisnull(r, 0, 0)) max_num = atoll(PQgetvalue(r, 0, 0));
        PQclear(r);
    }
#endif
    return max_num;
}

/* --save-config: store the connection/download configuration in the database's settings table so it
 * travels with the data. isotope-viewer reads these keys. The NNTP password is stored in plain text. */
static void db_save_config(DB *db, const char *host, const char *port, int use_ssl, int do_starttls,
                           const char *user, const char *pass, const char *group, int headers_only,
                           int threads, int retries, long long limit, int progress_width) {
    char buf[64];
    db_set_setting(db, "schema_version", "1");
    db_set_setting(db, "host", host && *host ? host : "localhost");
    db_set_setting(db, "port", port && *port ? port : (use_ssl ? "563" : "119"));
    snprintf(buf, sizeof(buf), "%d", use_ssl ? 1 : 0); db_set_setting(db, "ssl", buf);
    snprintf(buf, sizeof(buf), "%d", do_starttls ? 1 : 0); db_set_setting(db, "starttls", buf);
    db_set_setting(db, "user", user ? user : "");
    db_set_setting(db, "pass", pass ? pass : "");
    db_set_setting(db, "group", group ? group : "");
    snprintf(buf, sizeof(buf), "%d", headers_only ? 1 : 0); db_set_setting(db, "headers_only", buf);
    snprintf(buf, sizeof(buf), "%d", threads); db_set_setting(db, "threads", buf);
    snprintf(buf, sizeof(buf), "%d", retries); db_set_setting(db, "retries", buf);
    snprintf(buf, sizeof(buf), "%lld", limit); db_set_setting(db, "limit", buf);
    snprintf(buf, sizeof(buf), "%d", progress_width); db_set_setting(db, "progress_width", buf);
}

/* ---- --since MMDDYYYY: locate the first article posted on/after a date ---- */

/* UTC epoch for a calendar date/time (portable replacement for timegm, which strict POSIX hides). */
static long long utc_epoch(long year, int mon /* 1-12 */, int day, long hh, long mm, long ss) {
    long y = year - (mon <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

/* Parses MMDDYYYY into a UTC epoch (midnight). Returns -1 if malformed or not a real date. */
static long long parse_since_date(const char *v) {
    if (!v || strlen(v) != 8) return -1;
    for (int i = 0; i < 8; ++i) if (v[i] < '0' || v[i] > '9') return -1;
    int mon = (v[0]-'0')*10 + (v[1]-'0'), day = (v[2]-'0')*10 + (v[3]-'0');
    int year = atoi(v + 4);
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mon < 1 || mon > 12 || year < 1970 || day < 1) return -1;
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (day > mdays[mon-1] + (mon == 2 && leap)) return -1;
    return utc_epoch(year, mon, day, 0, 0, 0);
}

/* Parses an RFC 5322 Date header ("Tue, 23 Feb 1999 10:11:12 +0100") into a UTC epoch; -1 on failure. */
static long long parse_article_date(const char *d) {
    static const char *months[] = {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
    if (!d) return -1;
    const char *p = strchr(d, ',');
    p = p ? p + 1 : d;
    while (*p == ' ') p++;
    char *end = NULL;
    long day = strtol(p, &end, 10);
    if (end == p || day < 1 || day > 31) return -1;
    p = end; while (*p == ' ') p++;
    int mon = -1;
    for (int i = 0; i < 12; ++i) if (strncasecmp(p, months[i], 3) == 0) { mon = i; break; }
    if (mon < 0) return -1;
    p += 3; while (isalpha((unsigned char)*p)) p++;
    long year = strtol(p, &end, 10);
    if (end == p) return -1;
    if (end - p <= 2) year += year < 70 ? 2000 : 1900;
    p = end;
    long hh = 0, mm = 0, ss = 0;
    sscanf(p, " %ld:%ld:%ld", &hh, &mm, &ss);
    long off = 0; /* numeric zone offset, if present */
    const char *z = p;
    while (*z && *z != '+' && *z != '-') z++;
    if (*z && strlen(z) >= 5 && isdigit((unsigned char)z[1])) {
        long hhmm = atol(z + 1);
        off = (hhmm / 100) * 3600 + (hhmm % 100) * 60;
        if (*z == '-') off = -off;
    }
    return utc_epoch(year, mon + 1, (int)day, hh, mm, ss) - off;
}

/* Returns the posting time of the first existing article at or above 'n' (within a short probe),
 * storing its number in *found. Returns -1 if nothing parseable exists there (gap / expired). */
static long long probe_article_date(Conn *c, long long n, long long last, long long *found) {
    for (long long width = 8; width <= 4096; width *= 8) {
        long long hi = n + width - 1 > last ? last : n + width - 1;
        char *ov = nntp_xover(c, n, hi);
        if (g_stop_requested) { free(ov); return -1; }
        if (ov) {
            char *save = NULL;
            for (char *line = strtok_r(ov, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                long long id; char *subj, *auth, *date, *mid, *refs; long long bytes; int lines;
                if (!parse_xover_line(line, &id, &subj, &auth, &date, &mid, &refs, &bytes, &lines)) continue;
                long long t = parse_article_date(date);
                free(subj); free(auth); free(date); free(mid); free(refs);
                if (t >= 0) { *found = id; free(ov); return t; }
            }
            free(ov);
        }
        if (hi >= last) break;
    }
    return -1;
}

/* Binary-searches [first,last] for the lowest article number posted on/after 'since'.
 * Returns last+1 if every article is older. An unreadable probe (expired/missing articles)
 * is treated as older than the date, since expiry removes the oldest articles first. */
static long long find_first_article_since(Conn *c, long long first, long long last, long long since) {
    long long lo = first, hi = last + 1;
    while (lo < hi && !g_stop_requested) {
        long long mid = lo + (hi - lo) / 2, found = 0;
        long long t = probe_article_date(c, mid, last, &found);
        if (t >= 0 && t >= since) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

/* CLI parsing and validation */
/* Full reference shown for a bare `nntp2sql`, --help and -h. Generated from structured text so every option and
 * its aliases sit on one line; @PROG@ is replaced with the program name. */
static const char *const HELP_LINES[] = {
    "nntp2sql - download Usenet (NNTP) article headers into a SQL database\n",
    "\n",
    "SYNOPSIS\n",
    "  @PROG@ --host HOST --db-type TYPE --db-name NAME --group GROUP [options]   download headers\n",
    "  @PROG@ --db-type TYPE --db-name NAME --group GROUP --resume                fetch only what is new\n",
    "  @PROG@ --host HOST --db-type TYPE --db-name NAME --list-groups             store the server's group list\n",
    "  @PROG@ --db-type TYPE --db-name NAME --load-config-db                      show a database's saved settings\n",
    "  @PROG@ --help                                                              show this reference\n",
    "\n",
    "Articles are fetched over NNTP and stored in SQLite, MySQL/MariaDB or PostgreSQL. Re-running is safe:\n",
    "each article is stored once per (group, article number), so fetching a range again updates rows instead\n",
    "of duplicating them. Only headers are downloaded; bodies and files are fetched by isotope-viewer.\n",
    "\n",
    "NEWS SERVER (NNTP)\n",
    "  --host HOST                 News server to connect to (default: localhost).\n",
    "  --port PORT                 Server port (default: 563 with --ssl, otherwise 119).\n",
    "  --ssl                       Encrypt the whole connection with TLS from the start (usually port 563).\n",
    "  --starttls                  Connect in plain text, then upgrade to TLS with the STARTTLS command.\n",
    "  --user USER                 Account name. Must be given together with --pass.\n",
    "  --pass PASS                 Account password. Must be given together with --user.\n",
    "\n",
    "DATABASE\n",
    "  --db-type TYPE              Required. sqlite, mysql, mariadb or postgres.\n",
    "  --db-name NAME              Required. Database name; for sqlite, the path of the database file.\n",
    "  --db-host HOST              MySQL/PostgreSQL server host (default: localhost).\n",
    "  --db-port PORT              MySQL/PostgreSQL server port (default: 3306 for MySQL, 5432 for\n",
    "                              PostgreSQL).\n",
    "  --db-user USER              Database user (default: root for MySQL, postgres for PostgreSQL).\n",
    "  --db-pass PASS              Database password (default: empty).\n",
    "\n",
    "WHAT TO DOWNLOAD\n",
    "  --group GROUP               Newsgroup to fetch, e.g. alt.binaries.sounds.lossless.jazz.\n",
    "  --headers-only, --xover     Fetch headers in bulk with XOVER (much faster). Without it, each article's\n",
    "                              full header is fetched one at a time with HEAD (slower, more detail).\n",
    "  --limit N                   Fetch only the newest N articles of the group. Default: the whole group.\n",
    "  --articles N, --count N, --max-articles N, -n N\n",
    "                              Like --limit, but keep going until exactly N articles are stored, even\n",
    "                              when some article numbers are missing from the server.\n",
    "  --since MMDDYYYY, --from-date MMDDYYYY\n",
    "                              Start at the first article posted on or after this date, e.g. 10152026 for\n",
    "                              15 Oct 2026 (month, day, 4-digit year; must be a real date). Downloads\n",
    "                              upward from there; with --limit, N articles going forward. Useful on huge\n",
    "                              groups whose oldest articles have expired.\n",
    "  --resume, --continue, --from-last\n",
    "                              Continue where the last run stopped: start at the highest article number\n",
    "                              already stored for the group (that article is fetched again, which is\n",
    "                              harmless). Nothing stored yet: starts normally. If the stored numbers are\n",
    "                              beyond the server's range (a renumbered group) it warns and starts from\n",
    "                              the beginning. With --limit, takes N articles going forward from that\n",
    "                              point; with --since, starts at the later of the two.\n",
    "  --threads N                 Parallel server connections, 1-64 (default: 16). Respect your provider's\n",
    "                              limit.\n",
    "  --retries N                 Reconnect attempts per failed request, 0-10 (default: 3).\n",
    "  --upsert                    Insert new articles and update existing ones (the default).\n",
    "  --update-only, --no-upsert  Only update articles that are already stored; never insert new ones.\n",
    "\n",
    "GROUP LIST AND MAINTENANCE (each of these does its job and exits)\n",
    "  --list-groups, --all-groups\n",
    "                              Download the server's complete newsgroup list (LIST ACTIVE), store every\n",
    "                              group in the database's groups table, print them, and exit. The article\n",
    "                              count stored is an estimate (high - low + 1) until headers are fetched.\n",
    "  --remove-duplicates, --dedupe\n",
    "                              Delete duplicate articles (same article number, or same Message-ID, within\n",
    "                              a group), keeping the oldest row, then rebuild the unique index.\n",
    "                              Cross-posts in different groups are kept. Run it on databases made by\n",
    "                              older versions.\n",
    "  --export-html               Write the stored articles of a group to an HTML page instead of\n",
    "                              downloading. Needs --export-html-out and --group (or --group-list).\n",
    "  --export-html-out PATH      Output file for --export-html (a directory when exporting a group list).\n",
    "  --group-list FILE           Text file with one group name per line ('#' or ';' start a comment). Used\n",
    "                              by --export-html and --skeleton-db.\n",
    "\n",
    "CREATING A DATABASE\n",
    "  --init-db                   Create the tables and record the server settings. A missing MySQL/MariaDB\n",
    "                              database is created; a SQLite file is created on first use; a PostgreSQL\n",
    "                              database must already exist. Continues with the download if --group is\n",
    "                              given, otherwise exits after initializing.\n",
    "  --create-db                 Create the database and tables, then exit without downloading.\n",
    "  --skeleton-db, --create-skeleton-db, --skeleton\n",
    "                              Create the database, tables, settings and group rows, then exit.\n",
    "\n",
    "SAVED SETTINGS\n",
    "Your options can be saved to a FILE (~/.config/nntp2sql/nntp2sql.conf) and/or into the DATABASE itself,\n",
    "in a table named after it (database news0 -> table news0_settings). The two are independent. Both hold\n",
    "passwords in plain text; the file is created readable only by you.\n",
    "\n",
    "  --save-config               Save this run's server, login, database, threads, retries and other\n",
    "                              options to the file, then exit. Does not touch any database.\n",
    "                              isotope-viewer uses the file as its defaults.\n",
    "  --save-config-db            Save the same options into the database's settings table, then exit. Needs\n",
    "                              --db-type and --db-name; does not write the file. isotope-viewer applies\n",
    "                              them when it opens that database. Give both save options to do both.\n",
    "  --load-config, --read-config\n",
    "                              Read the saved file (or the one named by --conf) and print what it\n",
    "                              contains, passwords hidden. Fails if the file is missing. Applies even\n",
    "                              with --no-config.\n",
    "  --load-config-db, --read-config-db\n",
    "                              Read the settings stored in a database and print what would change. The\n",
    "                              database must be named on the command line with --db-name (e.g. --db-type\n",
    "                              mysql --db-name news0), not just taken from a config file. Applied without\n",
    "                              a prompt unless --confirm-db-defaults is also given. Fails if it has no\n",
    "                              stored settings.\n",
    "                              By themselves the two load options just show the settings and exit. Add\n",
    "                              any option that asks for work (--group, --limit, --since, --resume,\n",
    "                              --list-groups, ...) and they read the settings and then do that work. For\n",
    "                              example: --db-type mysql --db-name news0 --load-config-db --group\n",
    "                              alt.test. Combined with a save option they copy between the two places and\n",
    "                              exit: --load-config-db --save-config (database -> file), --load-config\n",
    "                              --save-config-db (file -> database).\n",
    "  --confirm-db-defaults, --ask-db-defaults\n",
    "                              Before fetching, show the defaults stored in the database (server, login,\n",
    "                              threads, group, ...) that would change this run, with the current value\n",
    "                              beside each (passwords as ********), and ask [y/N] before applying them.\n",
    "                              Without this option stored defaults are applied automatically. Needs a\n",
    "                              terminal; when stdin is not interactive they are skipped instead of\n",
    "                              waiting.\n",
    "  --no-config                 Ignore the saved file for this run.\n",
    "  --conf FILE                 Read options from FILE (key=value lines) instead of the default saved\n",
    "                              file.\n",
    "  --write-conf FILE           Write the current options to FILE and exit.\n",
    "\n",
    "  Precedence: command-line options always win. Below that, server, port, group, threads and similar\n",
    "  settings stored in the database override the config file, while the NNTP login (--user/--pass) from\n",
    "  the config file wins over the one stored in the database. Built-in defaults apply to anything unset.\n",
    "  The saved file is read automatically when it exists (--no-config skips it). Config keys: host port ssl\n",
    "  starttls user pass db_type db_name db_host db_port db_user db_pass group headers_only limit\n",
    "  progress_width threads retries.\n",
    "\n",
    "OUTPUT\n",
    "  --progress-width N          Width of the progress bar in characters, 5-200 (default: 24).\n",
    "  --verbose                   Print informational messages as well as warnings and errors.\n",
    "  --log FILE                  Also append log messages to FILE.\n",
    "  --help, -h                  Show this reference. Running with no arguments does the same.\n",
    "\n",
    "Ctrl+C stops cleanly: workers finish their current request and everything stored so far is kept (exit\n",
    "code 130).\n",
    "\n",
    "EXIT CODES\n",
    "  0 success      2 bad arguments      3 bad config file or missing saved settings      130 stopped with Ctrl+C\n",
    "  10-15 network, TLS, NNTP greeting/command or login error      20-22 database connect, schema or statement error\n",
    "  30 runtime error, or a download that finished with failed requests or writes\n",
    "\n",
    "EXAMPLES\n",
    "  Newest 5000 articles of a group into a local SQLite file:\n",
    "    @PROG@ --host news.example.com --ssl --user me --pass secret \\\n",
    "        --db-type sqlite --db-name news.db --init-db --group alt.test --limit 5000 --headers-only\n",
    "\n",
    "  Everything since 15 Oct 2026 into MySQL, then later only what is new:\n",
    "    @PROG@ --host news.example.com --ssl --user me --pass secret \\\n",
    "        --db-type mysql --db-name news0 --db-user root --init-db --group alt.test --since 10152026\n",
    "    @PROG@ --host news.example.com --ssl --user me --pass secret \\\n",
    "        --db-type mysql --db-name news0 --db-user root --group alt.test --resume\n",
    "\n",
    "  Save the settings into the database once, then use them (only the group is named):\n",
    "    @PROG@ --host news.example.com --ssl --user me --pass secret --threads 8 \\\n",
    "        --db-type mysql --db-name news0 --db-user root --save-config-db\n",
    "    @PROG@ --db-type mysql --db-name news0 --db-user root --load-config-db --group alt.test --resume\n",
    "\n",
    "  List every group on the server, remove duplicates, show what a database saved:\n",
    "    @PROG@ --host news.example.com --ssl --user me --pass secret \\\n",
    "        --db-type sqlite --db-name news.db --init-db --list-groups\n",
    "    @PROG@ --db-type mysql --db-name news0 --db-user root --remove-duplicates\n",
    "    @PROG@ --db-type mysql --db-name news0 --db-user root --load-config-db\n",
    "\n",
    "FILES\n",
    "  ~/.config/nntp2sql/nntp2sql.conf\n",
    "                              Saved options (--save-config). $XDG_CONFIG_HOME is honoured.\n",
    "  ~/.config/nntp2sql/viewer.ini\n",
    "                              isotope-viewer's own remembered settings.\n",
    "  <database>_settings         Table inside each database holding its saved options (--save-config-db),\n",
    "                              e.g. news0_settings. A shared table called \"settings\" from older versions\n",
    "                              is copied across on first use.\n",
    "\n",
    "COMPANION PROGRAMS (each has its own icon and man page)\n",
    "  isotope-viewer              GTK4 reader for what nntp2sql stored. Opening a database lists its groups;\n",
    "                              a group's articles load when you pick it. Text posts are shown as threads;\n",
    "                              Read article opens a window with the text and any image it carries, from\n",
    "                              which you can reply (NNTP POST, always confirmed first). yEnc files are\n",
    "                              queued and downloaded; a file is saved only when every part is present and\n",
    "                              its sizes and CRC32 check out, so files with missing parts are refused,\n",
    "                              never saved partly.\n",
    "  hydrogen-group-viewer       GTK4 list of the groups table (see --list-groups), with filtering and\n",
    "                              sorting.\n",
    "  Both detect nntp2sql databases (news0, news1, ...) on SQLite, MySQL and PostgreSQL automatically and\n",
    "  start from the saved settings.\n",
    "\n",
    "KNOWN ISSUES AND LIMITS\n",
    "  - Without --limit, --articles, --since or --resume, a group is read from its OLDEST article number\n",
    "    upward. On large binary groups the oldest articles have usually expired on the server, so the early\n",
    "    run is mostly 'missing'. Use --since, --limit or --resume to start where articles still exist.\n",
    "  - --since finds its start with a binary search on article dates, so it assumes dates rise roughly with\n",
    "    article numbers; a few badly dated posts can move the start slightly. It needs the server's XOVER.\n",
    "  - Article numbers on some servers are sparse; the group's article count is then only an estimate.\n",
    "  - Headers that are not valid UTF-8 are converted assuming Windows-1252, which is right for most old\n",
    "    newsreaders but can mis-render other legacy encodings.\n",
    "  - --save-config and --save-config-db keep the NNTP and database passwords in plain text.\n",
    "  - --init-db cannot create a PostgreSQL database; create it first. PostgreSQL is the least exercised\n",
    "    backend. --write-conf and --save-config exit before connecting to anything.\n",
    "  - Only headers are downloaded, never article bodies; use isotope-viewer to read bodies and fetch\n",
    "    files.\n",
    "\n",
    "See also: nntp2sql(1), isotope-viewer(1), hydrogen-group-viewer(1).\n",
    NULL
};
static void print_full_help(const char *prog) {
    for (const char *const *line = HELP_LINES; *line; ++line) {
        const char *p = *line, *at;
        while ((at = strstr(p, "@PROG@"))) { fwrite(p, 1, (size_t)(at - p), stdout); fputs(prog, stdout); p = at + 6; }
        fputs(p, stdout);
    }
}

static void usage_and_exit(const char *prog, AppError code, const char *detail) {
    if (code == ERR_OK) {
        print_full_help(prog);
    } else {
        fprintf(stderr, "Usage: %s --host HOST --db-type {sqlite|mariadb|mysql|postgres} --db-name NAME --group GROUP [options]\n"
                        "Run '%s --help' for a description of every option.\n", prog, prog);
        fprintf(stderr, "Error (code %d): %s\n", code, describe_error(code));
        if (detail && *detail) fprintf(stderr, "Details: %s\n", detail);
    }
    log_close();
    exit(code);
}

static const char *require_arg(const char *prog, int argc, char **argv, int *idx, const char *opt, int allow_empty) {
    if (*idx + 1 >= argc) {
        char msg[256];
        snprintf(msg, sizeof(msg), "option '%s' requires a value", opt);
        usage_and_exit(prog, ERR_ARGS, msg);
    }
    const char *val = argv[++(*idx)];
    if (!allow_empty && (!val || val[0] == '\0')) {
        char msg[256];
        snprintf(msg, sizeof(msg), "option '%s' cannot be empty", opt);
        usage_and_exit(prog, ERR_ARGS, msg);
    }
    if (!allow_empty && val[0] == '-' && val[1] == '-' && val[2] != '\0') {
        char msg[256];
        snprintf(msg, sizeof(msg), "option '%s' requires a value (got flag '%s')", opt, val);
        usage_and_exit(prog, ERR_ARGS, msg);
    }
    return val;
}

static int parse_checked_int(const char *prog, const char *opt_name, const char *val, int min_v, int max_v, AppError err_code) {
    if (!val || !*val) {
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid empty integer for %s (expected %d..%d)", opt_name, min_v, max_v);
        if (err_code == ERR_CONFIG) fatal(ERR_CONFIG, "%s", msg);
        usage_and_exit(prog, err_code, msg);
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || v < min_v || v > max_v) {
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid value '%s' for %s (expected integer in range %d..%d)", val, opt_name, min_v, max_v);
        if (err_code == ERR_CONFIG) fatal(ERR_CONFIG, "%s", msg);
        usage_and_exit(prog, err_code, msg);
    }
    return (int)v;
}

static long long parse_checked_ll(const char *prog, const char *opt_name, const char *val, long long min_v, long long max_v, AppError err_code) {
    if (!val || !*val) {
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid empty integer for %s (expected %lld..%lld)", opt_name, min_v, max_v);
        if (err_code == ERR_CONFIG) fatal(ERR_CONFIG, "%s", msg);
        usage_and_exit(prog, err_code, msg);
    }
    errno = 0;
    char *end = NULL;
    long long v = strtoll(val, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || v < min_v || v > max_v) {
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid value '%s' for %s (expected integer in range %lld..%lld)", val, opt_name, min_v, max_v);
        if (err_code == ERR_CONFIG) fatal(ERR_CONFIG, "%s", msg);
        usage_and_exit(prog, err_code, msg);
    }
    return v;
}

static int has_whitespace_or_ctrl(const char *s) {
    if (!s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7f) return 1;
    }
    return 0;
}

/* Simple key=value config file loader */
static void trim(char *s) {
    char *p = s; while (*p && isspace((unsigned char)*p)) p++; if (p!=s) memmove(s,p,strlen(p)+1);
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n-1])) s[--n] = '\0';
}

static int load_conf(const char *prog, const char *path,
                     const char **host, const char **port, int *use_ssl, int *do_starttls,
                     const char **user, const char **pass,
                     const char **db_type_s, const char **db_name, const char **db_host,
                     const char **db_port, const char **db_user, const char **db_pass,
                     const char **group, int *headers_only, long long *limit, int *progress_width,
                     int *threads, int *retries,
                     int has_cli_ssl, int has_cli_starttls, int has_cli_headers_only,
                     int has_cli_limit, int has_cli_progress_width, int has_cli_threads, int has_cli_retries) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fatal(ERR_CONFIG, "Could not open configuration file '%s': %s", path, strerror(errno));
    }
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        trim(line);
        if (!*line || *line=='#' || *line==';') continue;
        char *eq = strchr(line,'=');
        if (!eq) {
            fclose(f);
            fatal(ERR_CONFIG, "Malformed configuration line %d in '%s' (missing '=')", lineno, path);
        }
        *eq='\0'; char *key=line; char *val=eq+1; trim(key); trim(val);
        if (!*key) {
            fclose(f);
            fatal(ERR_CONFIG, "Empty key on line %d in '%s'", lineno, path);
        }
        if (strcasecmp(key,"host")==0) { if (!*host) *host = strdup(val); }
        else if (strcasecmp(key,"port")==0) {
            parse_checked_int(prog, "config port", val, 1, 65535, ERR_CONFIG);
            if (!*port) *port = strdup(val);
        }
        else if (strcasecmp(key,"ssl")==0) {
            int v = parse_checked_int(prog, "config ssl", val, 0, 1, ERR_CONFIG);
            if (!has_cli_ssl) *use_ssl = v;
        }
        else if (strcasecmp(key,"starttls")==0) {
            int v = parse_checked_int(prog, "config starttls", val, 0, 1, ERR_CONFIG);
            if (!has_cli_starttls) *do_starttls = v;
        }
        else if (strcasecmp(key,"user")==0) { if (!*user && *val) *user = strdup(val); }
        else if (strcasecmp(key,"pass")==0) { if (!*pass) *pass = strdup(val); }
        else if (strcasecmp(key,"db_type")==0 || strcasecmp(key,"db-type")==0) { if (!*db_type_s) *db_type_s = strdup(val); }
        else if (strcasecmp(key,"db_name")==0 || strcasecmp(key,"db-name")==0) { if (!*db_name) *db_name = strdup(val); }
        else if (strcasecmp(key,"db_host")==0 || strcasecmp(key,"db-host")==0) { if (!*db_host) *db_host = strdup(val); }
        else if (strcasecmp(key,"db_port")==0 || strcasecmp(key,"db-port")==0) {
            parse_checked_int(prog, "config db_port", val, 1, 65535, ERR_CONFIG);
            if (!*db_port) *db_port = strdup(val);
        }
        else if (strcasecmp(key,"db_user")==0 || strcasecmp(key,"db-user")==0) { if (!*db_user) *db_user = strdup(val); }
        else if (strcasecmp(key,"db_pass")==0 || strcasecmp(key,"db-pass")==0) { if (!*db_pass) *db_pass = strdup(val); }
        else if (strcasecmp(key,"group")==0) { if (!*group && *val) *group = strdup(val); }
        else if (strcasecmp(key,"headers_only")==0 || strcasecmp(key,"headers-only")==0) {
            int v = parse_checked_int(prog, "config headers_only", val, 0, 1, ERR_CONFIG);
            if (!has_cli_headers_only) *headers_only = v;
        }
        else if (strcasecmp(key,"limit")==0 || strcasecmp(key,"articles")==0 || strcasecmp(key,"count")==0) {
            long long v = parse_checked_ll(prog, "config limit", val, 0, LLONG_MAX, ERR_CONFIG);
            if (!has_cli_limit) *limit = v;
        }
        else if (strcasecmp(key,"progress_width")==0 || strcasecmp(key,"progress-width")==0) {
            int v = parse_checked_int(prog, "config progress_width", val, 5, 200, ERR_CONFIG);
            if (!has_cli_progress_width) *progress_width = v;
        }
        else if (strcasecmp(key,"threads")==0) {
            int v = parse_checked_int(prog, "config threads", val, 1, 64, ERR_CONFIG);
            if (!has_cli_threads) *threads = v;
        }
        else if (strcasecmp(key,"retries")==0) {
            int v = parse_checked_int(prog, "config retries", val, 0, 10, ERR_CONFIG);
            if (!has_cli_retries) *retries = v;
        }
        else {
            fclose(f);
            fatal(ERR_CONFIG, "Unknown configuration key '%s' on line %d in '%s'", key, lineno, path);
        }
    }
    fclose(f);
    return 1;
}

/* Default per-user config file shared with isotope-viewer: $XDG_CONFIG_HOME or ~/.config. */
static char *default_conf_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char buf[PATH_MAX];
    if (xdg && *xdg) snprintf(buf, sizeof(buf), "%s/nntp2sql/nntp2sql.conf", xdg);
    else if (home && *home) snprintf(buf, sizeof(buf), "%s/.config/nntp2sql/nntp2sql.conf", home);
    else return NULL;
    return strdup(buf);
}

static void make_parent_dirs(const char *path) {
    char *copy = strdup(path);
    if (!copy) return;
    for (char *p = copy + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        (void)mkdir(copy, 0700);
        *p = '/';
    }
    free(copy);
}

/* True when argv asks for real work (a group, limits, --since, --list-groups, ...) rather than only naming
 * the connection, the config, or output options. A bare --load-config/--load-config-db shows settings and
 * stops; combined with work it reads the settings and then does that work. */
/* True if the exact option appears on the command line itself (not merely supplied by a config file). */
static int args_has_option(int argc, char **argv, const char *option) {
    for (int i = 1; i < argc; ++i) if (strcmp(argv[i], option) == 0) return 1;
    return 0;
}

static int args_request_work(int argc, char **argv) {
    static const char *neutral[] = {
        "--host", "--port", "--ssl", "--starttls", "--user", "--pass", "--db-type", "--db-name", "--db-host",
        "--db-port", "--db-user", "--db-pass", "--conf", "--no-config", "--load-config", "--read-config",
        "--load-config-db", "--read-config-db", "--confirm-db-defaults", "--ask-db-defaults", "--progress-width",
        "--verbose", "--log", "--help", "-h", NULL};
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' || !argv[i][1]) continue; /* a value, not an option */
        int known = 0;
        for (int k = 0; neutral[k]; ++k) if (strcmp(argv[i], neutral[k]) == 0) { known = 1; break; }
        if (!known) return 1;
    }
    return 0;
}

/* Prints the settings read from a config file, hiding passwords. */
static void print_conf_summary(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    printf("Read settings from %s:\n", path);
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        line[strcspn(line, "\r\n")] = '\0';
        if (!*line || *line == '#' || !eq) continue;
        *eq = '\0';
        int secret = strcmp(line, "pass") == 0 || strcmp(line, "db_pass") == 0;
        printf("  %-16s %s\n", line, secret && eq[1] ? "********" : eq + 1);
        n++;
    }
    fclose(f);
    printf("%d settings read.\n", n);
}

static int write_conf(const char *path,
                      const char *host, const char *port, int use_ssl, int do_starttls,
                      const char *user, const char *pass,
                      const char *db_type_s, const char *db_name, const char *db_host,
                      const char *db_port, const char *db_user, const char *db_pass,
                      const char *group, int headers_only, long long limit, int progress_width,
                      int threads, int retries) {
    /* The file holds passwords: create it private (0600) and tighten an existing one too. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) { if (fd >= 0) close(fd); warnf("Could not write conf: %s", path); return 0; }
    (void)fchmod(fd, 0600);
    fprintf(f,"# nntp2sql configuration\n");
    if (host) fprintf(f,"host=%s\n", host);
    if (port) fprintf(f,"port=%s\n", port);
    fprintf(f,"ssl=%d\n", use_ssl);
    fprintf(f,"starttls=%d\n", do_starttls);
    if (user) fprintf(f,"user=%s\n", user);
    if (pass) fprintf(f,"pass=%s\n", pass);
    if (db_type_s) fprintf(f,"db_type=%s\n", db_type_s);
    if (db_name) fprintf(f,"db_name=%s\n", db_name);
    if (db_host) fprintf(f,"db_host=%s\n", db_host);
    if (db_port) fprintf(f,"db_port=%s\n", db_port);
    if (db_user) fprintf(f,"db_user=%s\n", db_user);
    if (db_pass) fprintf(f,"db_pass=%s\n", db_pass);
    if (group) fprintf(f,"group=%s\n", group);
    fprintf(f,"headers_only=%d\n", headers_only);
    fprintf(f,"limit=%lld\n", limit);
    fprintf(f,"progress_width=%d\n", progress_width);
    fprintf(f,"threads=%d\n", threads);
    fprintf(f,"retries=%d\n", retries);
    return fclose(f) == 0;
}

static void report_interrupt_and_exit(DB *db, Conn *c, long long missing) {
    if (!g_stop_requested) return;
    fprintf(stdout, "\nDownload interrupted (Ctrl+C): %lld SQL records updated (%lld articles/headers stored, %lld group records updated, %lld missing)\n",
            (long long)g_sql_records_updated, (long long)g_articles_updated, (long long)g_groups_updated, missing);
    fflush(stdout);
    if (c) conn_cleanup(c);
    if (db) db_close(db);
    log_close();
    exit(128 + (g_stop_signal ? (int)g_stop_signal : SIGINT));
}

/* Wraps bulk group inserts in one transaction; per-row commits on 100k+ groups are very slow. */
static void db_begin_bulk(DB *db) {
    if (db->type == DB_SQLITE) sqlite3_exec(db->sqlite, "BEGIN", NULL, NULL, NULL);
    else if (db->type == DB_MYSQL) (void)mysql_query(db->mysql, "START TRANSACTION");
#ifdef HAVE_PQ
    else if (db->type == DB_POSTGRES) PQclear(PQexec((PGconn *)db->impl, "BEGIN"));
#endif
}
static void db_commit_bulk(DB *db) {
    if (db->type == DB_SQLITE) sqlite3_exec(db->sqlite, "COMMIT", NULL, NULL, NULL);
    else if (db->type == DB_MYSQL) (void)mysql_query(db->mysql, "COMMIT");
#ifdef HAVE_PQ
    else if (db->type == DB_POSTGRES) PQclear(PQexec((PGconn *)db->impl, "COMMIT"));
#endif
}

/* --list-groups: downloads the server's full newsgroup list (LIST ACTIVE, falling back to LIST),
 * stores every group in the groups table, and prints them. The server gives only the low/high
 * article numbers, so the stored article_count is the estimate high-low+1 until headers are fetched. */
static void list_all_groups(DB *db, const char *host, const char *port, int use_ssl, int do_starttls,
                            const char *user, const char *pass) {
    Conn c;
    conn_init(&c);
    c.sock = tcp_connect(host, port);
    if (c.sock < 0) fatal(ERR_NET_CONNECT, "Unable to connect to %s:%s", host, port);
    if (use_ssl) {
        ssl_init();
        c.ctx = SSL_CTX_new(TLS_client_method());
        if (!c.ctx) fatal(ERR_TLS, "SSL_CTX_new failed");
        c.ssl = SSL_new(c.ctx);
        if (!c.ssl) fatal(ERR_TLS, "SSL_new failed");
        if (!SSL_set_fd(c.ssl, c.sock)) fatal(ERR_TLS, "SSL_set_fd failed");
        if (SSL_connect(c.ssl) <= 0) { ERR_print_errors_fp(stderr); fatal(ERR_TLS, "SSL_connect failed"); }
        c.use_ssl = 1;
    }
    char line[BUFSZ];
    if (conn_readline(&c, line, sizeof(line)) <= 0) fatal(ERR_NNTP_GREETING, "No greeting from server");
    if (atoi(line) >= 400) fatal(ERR_NNTP_GREETING, "Server error: %s", line);
    if (do_starttls && !use_ssl) {
        int rc = nntp_starttls(&c);
        if (rc < 200 || rc >= 300) fatal(ERR_TLS, "STARTTLS failed: %d", rc);
        if (!conn_starttls(&c)) fatal(ERR_TLS, "TLS handshake failed");
    }
    if (user && pass) {
        int rc = nntp_auth(&c, user, pass);
        if (rc >= 400) fatal(ERR_AUTH, "AUTH failed: %d", rc);
    }

    char *list = NULL;
    const char *cmds[] = {"LIST ACTIVE", "LIST"};
    for (int i = 0; i < 2 && !list && !g_stop_requested; ++i) {
        infof("Requesting newsgroup list (%s)...", cmds[i]);
        fprintf(stderr, "Downloading group list...\n");
        if (conn_sendf(&c, "%s", cmds[i]) <= 0 || conn_readline(&c, line, sizeof(line)) <= 0)
            fatal(ERR_NNTP_CMD, "%s failed: no response", cmds[i]);
        if (atoi(line) == 215) list = conn_read_multiline(&c);
        else infof("%s rejected: %s", cmds[i], line);
    }
    if (g_stop_requested) { conn_cleanup(&c); report_interrupt_and_exit(db, NULL, 0); }
    if (!list) fatal(ERR_NNTP_CMD, "Server did not provide a newsgroup list");

    long long stored = 0;
    db_begin_bulk(db);
    char *save = NULL;
    for (char *l = strtok_r(list, "\n", &save); l && !g_stop_requested; l = strtok_r(NULL, "\n", &save)) {
        char name[512], status[32] = "";
        long long high = 0, low = 0;
        size_t n = strlen(l);
        while (n && (l[n-1] == '\r' || l[n-1] == ' ')) l[--n] = '\0';
        if (sscanf(l, "%511s %lld %lld %31s", name, &high, &low, status) < 3) continue;
        long long count = (high >= low && high > 0) ? high - low + 1 : 0;
        db_insert_group(db, name, count, low, high);
        printf("%-60s %14lld %14lld  %s\n", name, low, high, status);
        stored++;
    }
    db_commit_bulk(db);
    free(list);
    conn_cleanup(&c);
    printf("\n%lld newsgroups downloaded and stored in the database\n", stored);
    fflush(stdout);
    if (g_stop_requested) report_interrupt_and_exit(db, NULL, 0);
}

int main(int argc, char **argv) {
    if (argc == 1) usage_and_exit(argv[0], ERR_OK, NULL); /* no options: show the full reference */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    const char *host = NULL, *port = NULL, *user = NULL, *pass = NULL;
    int use_ssl = 0, do_starttls = 0;
    const char *db_type_s = NULL, *db_name = NULL, *db_host = NULL, *db_user = NULL, *db_pass = NULL, *db_port = NULL;
    const char *group = NULL;
    int headers_only = 0;
    long long limit = 0;
    int resume = 0; /* --resume: start at the last article already stored for the group */
    long long since_epoch = 0; /* --since: start at first article posted on/after this UTC time */
    int exact_article_target = 0; /* set when --articles / --count / -n is used */
    int progress_width = 24; /* default bar width */
    int init_db = 0; /* create database if needed */
    int list_groups = 0; /* --list-groups: download, store and show every newsgroup, then exit */
    int remove_duplicates = 0; /* --remove-duplicates: dedupe articles, rebuild indexes, exit */
    int create_db_exit = 0; /* if --create-db used: create DB + schema then exit */
    int skeleton_db = 0; /* if --skeleton-db used: create skeleton DB with all defaults then exit */
    const char *conf_path = NULL; /* load settings */
    const char *write_conf_path = NULL; /* save settings */
    int confirm_db_defaults = 0; /* --confirm-db-defaults: ask before applying defaults stored in the database */
    int load_config = 0, load_config_db = 0; /* --load-config (file), --load-config-db (database): explicit reads */
    int save_config = 0, save_config_db = 0, no_config = 0; /* --save-config (file), --save-config-db (database), --no-config */
    int threads = 16; /* high-throughput multithreaded worker pool for both HEAD and XOVER */
    int retries = 3; /* reconnect attempts for HEAD and XOVER */
    const char *log_path = NULL; /* optional log file */
    /* Track which flags were explicitly set on the CLI */
    int has_cli_host = 0, has_cli_port = 0, has_cli_ssl = 0, has_cli_starttls = 0;
    int has_cli_group = 0, has_cli_headers_only = 0, has_cli_limit = 0;
    int has_cli_progress_width = 0, has_cli_threads = 0, has_cli_retries = 0;
    /* HTML export options */
    int opt_export_html = 0; const char *opt_export_group = NULL; const char *opt_export_group_list = NULL; const char *opt_export_out = NULL;
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage_and_exit(argv[0], ERR_OK, NULL);
        }
        else if (strcmp(argv[i], "--host") == 0) {
            host = require_arg(argv[0], argc, argv, &i, "--host", 0);
            has_cli_host = 1;
        }
        else if (strcmp(argv[i], "--port") == 0) {
            port = require_arg(argv[0], argc, argv, &i, "--port", 0);
            parse_checked_int(argv[0], "--port", port, 1, 65535, ERR_ARGS);
            has_cli_port = 1;
        }
        else if (strcmp(argv[i], "--ssl") == 0) { use_ssl = 1; has_cli_ssl = 1; }
        else if (strcmp(argv[i], "--starttls") == 0) { do_starttls = 1; has_cli_starttls = 1; }
        else if (strcmp(argv[i], "--user") == 0) {
            user = require_arg(argv[0], argc, argv, &i, "--user", 0);
        }
        else if (strcmp(argv[i], "--pass") == 0) {
            pass = require_arg(argv[0], argc, argv, &i, "--pass", 1);
        }
        else if (strcmp(argv[i], "--db-type") == 0) {
            db_type_s = require_arg(argv[0], argc, argv, &i, "--db-type", 0);
        }
        else if (strcmp(argv[i], "--db-name") == 0) {
            db_name = require_arg(argv[0], argc, argv, &i, "--db-name", 0);
        }
        else if (strcmp(argv[i], "--db-host") == 0) {
            db_host = require_arg(argv[0], argc, argv, &i, "--db-host", 0);
        }
        else if (strcmp(argv[i], "--db-port") == 0) {
            db_port = require_arg(argv[0], argc, argv, &i, "--db-port", 0);
            parse_checked_int(argv[0], "--db-port", db_port, 1, 65535, ERR_ARGS);
        }
        else if (strcmp(argv[i], "--db-user") == 0) {
            db_user = require_arg(argv[0], argc, argv, &i, "--db-user", 0);
        }
        else if (strcmp(argv[i], "--db-pass") == 0) {
            db_pass = require_arg(argv[0], argc, argv, &i, "--db-pass", 1);
        }
        else if (strcmp(argv[i], "--group") == 0) {
            group = require_arg(argv[0], argc, argv, &i, "--group", 0);
            opt_export_group = group;
            has_cli_group = 1;
        }
        else if (strcmp(argv[i], "--headers-only") == 0 || strcmp(argv[i], "--xover") == 0) {
            headers_only = 1;
            has_cli_headers_only = 1;
        }
        else if (strcmp(argv[i], "--limit") == 0) {
            const char *v = require_arg(argv[0], argc, argv, &i, "--limit", 0);
            limit = parse_checked_ll(argv[0], "--limit", v, 1, LLONG_MAX, ERR_ARGS);
            has_cli_limit = 1;
        }
        else if (strcmp(argv[i], "--articles") == 0 || strcmp(argv[i], "--count") == 0 ||
                 strcmp(argv[i], "--max-articles") == 0 || strcmp(argv[i], "-n") == 0) {
            const char *opt = argv[i];
            const char *v = require_arg(argv[0], argc, argv, &i, opt, 0);
            limit = parse_checked_ll(argv[0], opt, v, 1, LLONG_MAX, ERR_ARGS);
            exact_article_target = 1;
            has_cli_limit = 1;
        }
        else if (strcmp(argv[i], "--resume") == 0 || strcmp(argv[i], "--continue") == 0 || strcmp(argv[i], "--from-last") == 0) { resume = 1; }
        else if (strcmp(argv[i], "--since") == 0 || strcmp(argv[i], "--from-date") == 0) {
            const char *v = require_arg(argv[0], argc, argv, &i, "--since", 0);
            since_epoch = parse_since_date(v);
            if (since_epoch < 0) usage_and_exit(argv[0], ERR_ARGS, "--since expects a real date as MMDDYYYY (e.g. 02221999)");
        }
        else if (strcmp(argv[i], "--progress-width") == 0) {
            const char *v = require_arg(argv[0], argc, argv, &i, "--progress-width", 0);
            progress_width = parse_checked_int(argv[0], "--progress-width", v, 5, 200, ERR_ARGS);
            has_cli_progress_width = 1;
        }
        else if (strcmp(argv[i], "--remove-duplicates") == 0 || strcmp(argv[i], "--dedupe") == 0) { remove_duplicates = 1; }
        else if (strcmp(argv[i], "--list-groups") == 0 || strcmp(argv[i], "--all-groups") == 0) { list_groups = 1; }
        else if (strcmp(argv[i], "--init-db") == 0) { init_db = 1; }
        else if (strcmp(argv[i], "--create-db") == 0) { init_db = 1; create_db_exit = 1; }
        else if (strcmp(argv[i], "--skeleton-db") == 0 || strcmp(argv[i], "--create-skeleton-db") == 0 || strcmp(argv[i], "--skeleton") == 0) {
            init_db = 1;
            create_db_exit = 1;
            skeleton_db = 1;
        }
        else if (strcmp(argv[i], "--conf") == 0) {
            conf_path = require_arg(argv[0], argc, argv, &i, "--conf", 0);
        }
        else if (strcmp(argv[i], "--save-config") == 0) { save_config = 1; }
        else if (strcmp(argv[i], "--save-config-db") == 0) { save_config_db = 1; }
        else if (strcmp(argv[i], "--confirm-db-defaults") == 0 || strcmp(argv[i], "--ask-db-defaults") == 0) { confirm_db_defaults = 1; }
        else if (strcmp(argv[i], "--load-config") == 0 || strcmp(argv[i], "--read-config") == 0) { load_config = 1; }
        else if (strcmp(argv[i], "--load-config-db") == 0 || strcmp(argv[i], "--read-config-db") == 0) { load_config_db = 1; }
        else if (strcmp(argv[i], "--no-config") == 0) { no_config = 1; }
        else if (strcmp(argv[i], "--write-conf") == 0) {
            write_conf_path = require_arg(argv[0], argc, argv, &i, "--write-conf", 0);
        }
        else if (strcmp(argv[i], "--log") == 0) {
            log_path = require_arg(argv[0], argc, argv, &i, "--log", 0);
        }
        else if (strcmp(argv[i], "--verbose") == 0) { g_verbose = 1; }
        else if (strcmp(argv[i], "--threads") == 0) {
            const char *v = require_arg(argv[0], argc, argv, &i, "--threads", 0);
            threads = parse_checked_int(argv[0], "--threads", v, 1, 64, ERR_ARGS);
            has_cli_threads = 1;
        }
        else if (strcmp(argv[i], "--retries") == 0) {
            const char *v = require_arg(argv[0], argc, argv, &i, "--retries", 0);
            retries = parse_checked_int(argv[0], "--retries", v, 0, 10, ERR_ARGS);
            has_cli_retries = 1;
        }
        else if (strcmp(argv[i], "--upsert") == 0) { g_upsert = 1; }
        else if (strcmp(argv[i], "--update-only") == 0 || strcmp(argv[i], "--no-upsert") == 0) { g_upsert = 0; }
        else if (strcmp(argv[i], "--export-html") == 0) { opt_export_html = 1; }
        else if (strcmp(argv[i], "--export-html-out") == 0) {
            opt_export_out = require_arg(argv[0], argc, argv, &i, "--export-html-out", 0);
        }
        else if (strcmp(argv[i], "--group-list") == 0) {
            opt_export_group_list = require_arg(argv[0], argc, argv, &i, "--group-list", 0);
        }
        else {
            char msg[256];
            snprintf(msg, sizeof(msg), "unknown option '%s'", argv[i]);
            usage_and_exit(argv[0], ERR_ARGS, msg);
        }
        i++;
    }

    if (log_path) log_open(log_path);
    infof("Starting nntp2sql");

    /* Reading stored settings is always about one named database, never one picked up from a config file. */
    if (load_config_db && !args_has_option(argc, argv, "--db-name"))
        usage_and_exit(argv[0], ERR_ARGS, "--load-config-db needs a database: name it with --db-name "
                                          "(for example --db-type mysql --db-name news0)");

    /* Default per-user config: loaded automatically if present (CLI options still win), and the
     * target of --save-config. isotope-viewer reads the same file for its defaults. */
    /* Read-only: only --load-config/--load-config-db were requested (no save, no work). */
    int load_read_only = (load_config || load_config_db) && !args_request_work(argc, argv) &&
                         !save_config && !save_config_db && !write_conf_path;
    char *default_conf = default_conf_path();
    if (load_config && !conf_path) {
        if (!default_conf) fatal(ERR_CONFIG, "--load-config needs HOME or XDG_CONFIG_HOME to be set");
        if (access(default_conf, R_OK) != 0)
            fatal(ERR_CONFIG, "--load-config: no saved config at '%s' (create it with --save-config)", default_conf);
        conf_path = default_conf; /* an explicit read applies even with --no-config */
    } else if (!conf_path && !no_config && default_conf && access(default_conf, R_OK) == 0) conf_path = default_conf;
    if (save_config) {
        if (!default_conf) fatal(ERR_CONFIG, "--save-config needs HOME or XDG_CONFIG_HOME to be set");
        make_parent_dirs(default_conf);
        if (!write_conf_path) write_conf_path = default_conf;
    }

    /* load configuration file early (values from CLI override) */
    if (conf_path) {
        load_conf(argv[0], conf_path, &host, &port, &use_ssl, &do_starttls,
                  &user, &pass, &db_type_s, &db_name, &db_host,
                  &db_port, &db_user, &db_pass, &group,
                  &headers_only, &limit, &progress_width,
                  &threads, &retries,
                  has_cli_ssl, has_cli_starttls, has_cli_headers_only,
                  has_cli_limit, has_cli_progress_width, has_cli_threads, has_cli_retries);
        if (load_config) print_conf_summary(conf_path);
        /* A read-only request does nothing else: no database, no server, no download. */
        if (load_config && !load_config_db && load_read_only) {
            log_close();
            return ERR_OK;
        }
        if (!opt_export_group && group) opt_export_group = group;
    }

    /* Validate combined flags and entered strings */
    if (use_ssl && do_starttls) {
        usage_and_exit(argv[0], ERR_ARGS, "cannot specify both --ssl and --starttls");
    }
    if ((user && !pass) || (!user && pass)) {
        usage_and_exit(argv[0], ERR_ARGS, "both --user and --pass must be provided for authentication");
    }
    if (host && (!*host || has_whitespace_or_ctrl(host))) {
        usage_and_exit(argv[0], ERR_ARGS, "invalid --host value");
    }
    if (db_host && (!*db_host || has_whitespace_or_ctrl(db_host))) {
        usage_and_exit(argv[0], ERR_ARGS, "invalid --db-host value");
    }
    if (group && (!*group || has_whitespace_or_ctrl(group))) {
        usage_and_exit(argv[0], ERR_ARGS, "invalid --group value");
    }
    if (opt_export_html) {
        if (!opt_export_out || !*opt_export_out) {
            usage_and_exit(argv[0], ERR_ARGS, "--export-html requires --export-html-out PATH");
        }
        if (!opt_export_group && !opt_export_group_list) {
            usage_and_exit(argv[0], ERR_ARGS, "--export-html requires --group or --group-list");
        }
    }

    /* Apply defaults for skeleton-db if db_type or db_name were not provided */
    if (skeleton_db) {
        if (!db_type_s) db_type_s = "sqlite";
        if (!db_name) {
            db_name = (strcmp(db_type_s, "sqlite") == 0) ? "nntp2sql.db" : "nntp2sql";
        }
    }

    if (!host) host = "localhost"; /* default NNTP host */
    if (!port) port = use_ssl ? "563" : "119";

    /* If only writing config file and no DB init requested, save and exit */
    if (write_conf_path && !init_db && !create_db_exit && !skeleton_db && !save_config_db && !load_config_db) {
        if (!write_conf(write_conf_path, host, port, use_ssl, do_starttls,
                        user, pass, db_type_s, db_name, db_host, db_port, db_user, db_pass,
                        group, headers_only, limit, progress_width, threads, retries)) {
            fatal(ERR_CONFIG, "Failed to write configuration file '%s': %s", write_conf_path, strerror(errno));
        }
        fprintf(stdout, "Configuration written to %s\n", write_conf_path);
        log_close();
        return ERR_OK;
    }

    if (!db_type_s || !*db_type_s) {
        usage_and_exit(argv[0], ERR_ARGS, "missing required option --db-type {sqlite|mariadb|mysql|postgres}");
    }
    if (!db_name || !*db_name) {
        usage_and_exit(argv[0], ERR_ARGS, "missing required option --db-name DBNAME");
    }

    DB db = {0};
    if (strcmp(db_type_s, "sqlite") == 0) db.type = DB_SQLITE;
    else if (strcmp(db_type_s, "mariadb") == 0 || strcmp(db_type_s, "mysql") == 0) db.type = DB_MYSQL;
    else if (strcmp(db_type_s, "postgres") == 0 || strcmp(db_type_s, "pgsql") == 0) db.type = DB_POSTGRES;
    else {
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid --db-type '%s' (expected sqlite, mariadb, mysql, or postgres)", db_type_s);
        usage_and_exit(argv[0], ERR_ARGS, msg);
    }
    set_settings_table(db_name, db.type == DB_SQLITE); /* e.g. news0 -> news0_settings */

    if (db.type == DB_MYSQL) {
        for (const char *p = db_name; *p; ++p) {
            if (*p == '`' || (unsigned char)*p < 0x20) {
                usage_and_exit(argv[0], ERR_ARGS, "invalid character in --db-name for MySQL/MariaDB");
            }
        }
    }

    infof("Config: host=%s port=%s ssl=%d starttls=%d db=%s type=%s group=%s",
          host, port, use_ssl, do_starttls, db_name, db_type_s, group ? group : "(none)");

    /* Open DB (optionally create database) */
    if (db.type == DB_SQLITE) {
        if (sqlite3_open(db_name, &db.sqlite) != SQLITE_OK) {
            fatal(ERR_DB_CONNECT, "sqlite open failed for '%s': %s", db_name, db.sqlite ? sqlite3_errmsg(db.sqlite) : "out of memory");
        }
    } else if (db.type == DB_MYSQL) {
        const char *mh = db_host ? db_host : "localhost";
        unsigned int mp = db_port ? (unsigned int)parse_checked_int(argv[0], "--db-port", db_port, 1, 65535, ERR_ARGS) : 3306;
        const char *mu = db_user ? db_user : "root";
        const char *mpass = db_pass ? db_pass : "";
        if (init_db) {
            MYSQL *tmp = mysql_init(NULL);
            if (!tmp) fatal(ERR_DB_CONNECT, "mysql_init failed (init phase)");
            if (!mysql_real_connect(tmp, mh, mu, mpass, NULL, mp, NULL, 0)) {
                char err_buf[512];
                snprintf(err_buf, sizeof(err_buf), "%s", mysql_error(tmp));
                mysql_close(tmp);
                fatal(ERR_DB_CONNECT, "mysql server connect failed: %s", err_buf);
            }
            char q[512];
            snprintf(q, sizeof(q), "CREATE DATABASE IF NOT EXISTS `%s` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;", db_name);
            if (mysql_query(tmp, q)) {
                char err_buf[512];
                snprintf(err_buf, sizeof(err_buf), "%s", mysql_error(tmp));
                mysql_close(tmp);
                fatal(ERR_DB_SCHEMA, "mysql create database failed for '%s': %s", db_name, err_buf);
            }
            mysql_close(tmp);
        }
        db.mysql = mysql_init(NULL);
        if (!db.mysql) fatal(ERR_DB_CONNECT, "mysql_init failed");
        if (!mysql_real_connect(db.mysql, mh, mu, mpass, db_name, mp, NULL, CLIENT_FOUND_ROWS)) {
            fatal(ERR_DB_CONNECT, "mysql connect failed: %s", mysql_error(db.mysql));
        }
    }
#ifdef HAVE_PQ
    else if (db.type == DB_POSTGRES) {
        /* Connect via libpq */
        char conninfo[512];
        snprintf(conninfo, sizeof(conninfo), "host=%s port=%s dbname=%s user=%s password=%s",
                 db_host?db_host:"localhost",
                 db_port?db_port:"5432",
                 db_name?db_name:"postgres",
                 db_user?db_user:"postgres",
                 db_pass?db_pass:"");
        PGconn *pgc = PQconnectdb(conninfo);
        if (PQstatus(pgc) != CONNECTION_OK) fatal(ERR_DB_CONNECT, "postgres connect failed: %s", PQerrorMessage(pgc));
        db.impl = pgc;
    }
#else
    else if (db.type == DB_POSTGRES) {
        fatal(ERR_DB_CONNECT, "PostgreSQL support was not compiled into this build");
    }
#endif

    if (remove_duplicates) {
        db_remove_duplicates(&db);
        db_init_schema(&db); /* creates the unique (group_name, artnum) index now that duplicates are gone */
        db_close(&db);
        log_close();
        return ERR_OK;
    }

    db_init_schema(&db);
    db_migrate_legacy_settings(&db);

    if (init_db || create_db_exit || skeleton_db) {
        db_seed_skeleton(&db, host, port, use_ssl, do_starttls, group, opt_export_group_list,
                         headers_only, threads, retries, limit, progress_width);
    } else if (!save_config_db || load_config_db) {
        /* Load any stored defaults from the database if not overridden on CLI/conf; with
         * --confirm-db-defaults, show them and ask first. */
        if (confirm_db_defaults || load_config_db)
            db_load_defaults_confirmed(&db, db_name, confirm_db_defaults,
                                       !load_config_db ? 0 : load_read_only ? 2 : 1, &host, &port, &use_ssl, &do_starttls, &user, &pass, &group,
                                       &headers_only, &threads, &retries, &limit, &progress_width,
                                       has_cli_host, has_cli_port, has_cli_ssl, has_cli_starttls,
                                       has_cli_group, has_cli_headers_only, has_cli_threads,
                                       has_cli_retries, has_cli_limit, has_cli_progress_width);
        else
            db_load_skeleton_defaults(&db, &host, &port, &use_ssl, &do_starttls, &user, &pass, &group,
                                      &headers_only, &threads, &retries, &limit, &progress_width,
                                      has_cli_host, has_cli_port, has_cli_ssl, has_cli_starttls,
                                      has_cli_group, has_cli_headers_only, has_cli_threads,
                                      has_cli_retries, has_cli_limit, has_cli_progress_width);
        if (!opt_export_group && group) opt_export_group = group;
    }

    if (load_config_db && load_read_only) {
        db_close(&db); /* read-only request: report what the database holds, then stop */
        log_close();
        return ERR_OK;
    }
    if (save_config_db) {
        db_save_config(&db, host, port, use_ssl, do_starttls, user, pass, group, headers_only,
                       threads, retries, limit, progress_width);
        fprintf(stdout, "Configuration stored in table '%s' of database '%s' (%s)\n", g_settings_table, db_name, db_type_s);
        if (!write_conf_path) { /* database only: no config file was requested */
            db_close(&db);
            log_close();
            return ERR_OK;
        }
    }
    if (write_conf_path) {
        if (!write_conf(write_conf_path, host, port, use_ssl, do_starttls,
                        user, pass, db_type_s, db_name, db_host, db_port, db_user, db_pass,
                        group, headers_only, limit, progress_width, threads, retries)) {
            db_close(&db);
            fatal(ERR_CONFIG, "Failed to write configuration file '%s': %s", write_conf_path, strerror(errno));
        }
        fprintf(stdout, "Configuration written to %s\n", write_conf_path);
        db_close(&db);
        log_close();
        return ERR_OK; /* exit after saving */
    }

    if (skeleton_db) {
        fprintf(stdout, "Skeleton database created for '%s' (%s)\n", db_name, db_type_s);
        db_close(&db);
        log_close();
        return ERR_OK;
    }

    if (list_groups) {
        list_all_groups(&db, host, port, use_ssl, do_starttls, user, pass);
        db_close(&db);
        log_close();
        return ERR_OK;
    }

    if (create_db_exit) {
        fprintf(stdout, "Database and schema created for '%s' (%s)\n", db_name, db_type_s);
        db_close(&db);
        log_close();
        return ERR_OK; /* exit without NNTP ingest */
    }

    /* If --init-db was passed without --group and without --export-html, finish after initializing DB */
    if (init_db && !group && !opt_export_group_list && !opt_export_html) {
        fprintf(stdout, "Database initialized for '%s' (%s)\n", db_name, db_type_s);
        db_close(&db);
        log_close();
        return ERR_OK;
    }

    /* If export-only requested, skip NNTP connection */
    if (opt_export_html) {
        if (opt_export_group) {
            int rcx = export_group_to_html(&db, opt_export_group, opt_export_out);
            if (rcx != 0) fatal(ERR_RUNTIME, "HTML export failed (%d)", rcx);
            db_close(&db); log_close(); return ERR_OK;
        } else if (opt_export_group_list) {
            int rcx = export_groups_from_file(&db, opt_export_group_list, opt_export_out);
            if (rcx != 0) fatal(ERR_RUNTIME, "HTML export list failed (%d)", rcx);
            db_close(&db); log_close(); return ERR_OK;
        }
    }

    if ((!group || !*group) && (!opt_export_group_list || !*opt_export_group_list)) {
        db_close(&db);
        usage_and_exit(argv[0], ERR_ARGS, "missing required option --group GROUPNAME");
    }

    /* Build list of groups to download (supports --group and/or --group-list) */
    char **groups_to_fetch = NULL;
    size_t group_count = 0, group_cap = 0;
    if (group && *group) {
        group_cap = 4;
        groups_to_fetch = calloc(group_cap, sizeof(char *));
        if (!groups_to_fetch) fatal(ERR_RUNTIME, "Out of memory allocating group list");
        groups_to_fetch[group_count++] = strdup(group);
    }
    if (opt_export_group_list && *opt_export_group_list) {
        FILE *gf = fopen(opt_export_group_list, "r");
        if (!gf) {
            db_close(&db);
            fatal(ERR_ARGS, "Could not open group list file '%s': %s", opt_export_group_list, strerror(errno));
        }
        char gline[512];
        while (fgets(gline, sizeof(gline), gf)) {
            char *p = gline;
            while (*p && isspace((unsigned char)*p)) p++;
            size_t glen = strlen(p);
            while (glen > 0 && isspace((unsigned char)p[glen - 1])) p[--glen] = '\0';
            if (!*p || *p == '#' || *p == ';') continue;
            if (group_count == group_cap) {
                group_cap = group_cap ? group_cap * 2 : 8;
                char **grown = realloc(groups_to_fetch, group_cap * sizeof(char *));
                if (!grown) fatal(ERR_RUNTIME, "Out of memory growing group list");
                groups_to_fetch = grown;
            }
            groups_to_fetch[group_count++] = strdup(p);
        }
        fclose(gf);
    }
    if (group_count == 0) {
        free(groups_to_fetch);
        db_close(&db);
        usage_and_exit(argv[0], ERR_ARGS, "no valid groups specified in --group or --group-list");
    }

    /* sanitize progress width */
    if (progress_width < 5) progress_width = 5;
    if (progress_width > 200) progress_width = 200;

    int any_download_failed = 0;
    long long total_missing_all = 0;

    for (size_t gi = 0; gi < group_count; ++gi) {
        const char *cur_group = groups_to_fetch[gi];
        if (g_stop_requested) {
            for (size_t k = 0; k < group_count; ++k) free(groups_to_fetch[k]);
            free(groups_to_fetch);
            report_interrupt_and_exit(&db, NULL, total_missing_all);
        }

        /* NNTP connection */
        Conn c;
        conn_init(&c);
        c.sock = tcp_connect(host, port);
        if (g_stop_requested) {
            for (size_t k = 0; k < group_count; ++k) free(groups_to_fetch[k]);
            free(groups_to_fetch);
            report_interrupt_and_exit(&db, &c, total_missing_all);
        }
        if (c.sock < 0) fatal(ERR_NET_CONNECT, "Unable to connect to %s:%s", host, port);

        /* SSL on connect? */
        if (use_ssl) {
            ssl_init();
            c.ctx = SSL_CTX_new(TLS_client_method());
            if (!c.ctx) fatal(ERR_TLS, "SSL_CTX_new failed");
            c.ssl = SSL_new(c.ctx);
            if (!c.ssl) fatal(ERR_TLS, "SSL_new failed");
            if (!SSL_set_fd(c.ssl, c.sock)) fatal(ERR_TLS, "SSL_set_fd failed");
            if (SSL_connect(c.ssl) <= 0) { ERR_print_errors_fp(stderr); fatal(ERR_TLS, "SSL_connect failed"); }
            c.use_ssl = 1;
        }

        char line[BUFSZ];
        if (conn_readline(&c, line, sizeof(line)) <= 0) {
            if (g_stop_requested) {
                for (size_t k = 0; k < group_count; ++k) free(groups_to_fetch[k]);
                free(groups_to_fetch);
                report_interrupt_and_exit(&db, &c, total_missing_all);
            }
            fatal(ERR_NNTP_GREETING, "No greeting from server");
        }
        int greet = atoi(line);
        if (greet >= 400) fatal(ERR_NNTP_GREETING, "Server error: %s", line);

        /* optionally STARTTLS */
        if (do_starttls && !use_ssl) {
            int rc = nntp_starttls(&c);
            if (g_stop_requested) {
                for (size_t k = 0; k < group_count; ++k) free(groups_to_fetch[k]);
                free(groups_to_fetch);
                report_interrupt_and_exit(&db, &c, total_missing_all);
            }
            if (rc < 200 || rc >= 300) fatal(ERR_TLS, "STARTTLS failed: %d", rc);
            if (!conn_starttls(&c)) fatal(ERR_TLS, "TLS handshake failed");
        }

        /* AUTH if provided */
        if (user && pass) {
            int rc = nntp_auth(&c, user, pass);
            if (g_stop_requested) {
                for (size_t k = 0; k < group_count; ++k) free(groups_to_fetch[k]);
                free(groups_to_fetch);
                report_interrupt_and_exit(&db, &c, total_missing_all);
            }
            if (rc >= 400) fatal(ERR_AUTH, "AUTH failed: %d", rc);
        }

        /* select group and update SQL immediately */
        long long count = 0, first = 0, last = 0;
        int rc = nntp_group(&c, cur_group, &count, &first, &last);
        if (g_stop_requested) {
            for (size_t k = 0; k < group_count; ++k) free(groups_to_fetch[k]);
            free(groups_to_fetch);
            report_interrupt_and_exit(&db, &c, total_missing_all);
        }
        if (rc < 200 || rc >= 300) fatal(ERR_NNTP_CMD, "GROUP failed: %d", rc);
        db_insert_group(&db, cur_group, count, first, last);
        if (g_stop_requested) {
            for (size_t k = 0; k < group_count; ++k) free(groups_to_fetch[k]);
            free(groups_to_fetch);
            report_interrupt_and_exit(&db, &c, total_missing_all);
        }

        if (count == 0 || last < first || last < 1) {
            warnf("Group has no articles.");
            conn_cleanup(&c);
            continue;
        }
        if (first < 1) first = 1;

        long long fetch_first = first, fetch_last = last;
        /* --resume: continue from the highest article number already stored for this group. */
        long long resume_from = 0;
        if (resume) {
            resume_from = db_max_artnum(&db, cur_group);
            if (resume_from > last) {
                warnf("--resume: stored articles for %s go up to #%lld but the server's range ends at #%lld "
                      "(renumbered?); starting from the beginning of the range.", cur_group, resume_from, last);
                resume_from = 0;
            } else if (resume_from <= 0) {
                infof("--resume: no articles stored for %s yet; starting normally.", cur_group);
                resume_from = 0;
            } else {
                fprintf(stdout, "Resuming %s at article #%lld (last stored), server range #%lld..#%lld%s\n", cur_group,
                        resume_from, first, last, resume_from == last ? " - already up to date" : "");
                if (resume_from < first) resume_from = first; /* expired on the server: start at the oldest left */
            }
        }
        int forward_mode = since_epoch > 0 || resume_from > 0; /* going upward from a start point, not newest-first */
        if (since_epoch > 0) {
            fetch_first = find_first_article_since(&c, first, last, since_epoch);
            if (g_stop_requested) {
                for (size_t k = 0; k < group_count; ++k) free(groups_to_fetch[k]);
                free(groups_to_fetch);
                report_interrupt_and_exit(&db, &c, total_missing_all);
            }
            if (fetch_first > last) {
                warnf("No articles in %s were posted on or after the --since date.", cur_group);
                conn_cleanup(&c);
                continue;
            }
            if (resume_from > fetch_first) fetch_first = resume_from; /* the later of --since and --resume */
            infof("--since: starting at article %lld of %lld..%lld", fetch_first, first, last);
            /* With --since, --limit caps how many articles are taken going forward from the date. */
            if (limit > 0 && limit < fetch_last - fetch_first + 1) fetch_last = fetch_first + limit - 1;
        } else if (resume_from > 0) {
            fetch_first = resume_from;
            /* With --resume, --limit caps how many articles are taken going forward from that point. */
            if (limit > 0 && limit < fetch_last - fetch_first + 1) fetch_last = fetch_first + limit - 1;
        } else if (limit > 0) {
            if (limit < (last - first + 1)) {
                fetch_first = last - limit + 1;
                if (fetch_first < first) fetch_first = first;
            }
        }

        /* Release the discovery connection before starting the worker pool. */
        conn_cleanup(&c);
        if (fetch_first < 1 || fetch_last < fetch_first)
            fatal(ERR_NNTP_CMD, "Invalid article range");
        long long total = fetch_last - fetch_first + 1;
        int group_threads = threads;
        if ((long long)group_threads > total) group_threads = (int)total;
        if (group_threads < 1) group_threads = 1;
        WorkQueue queue = {0};
        queue.batch_size = (total + group_threads - 1) / group_threads;
        long long max_batch = headers_only ? (total > 2000 ? 500 : 16) : (total > 2000 ? 32 : 4);
        if (queue.batch_size > max_batch) queue.batch_size = max_batch;
        if (queue.batch_size < 1) queue.batch_size = 1;
        queue.group_first = forward_mode ? fetch_first : first;
        queue.next_backfill_last = fetch_first - 1;
        if (pthread_mutex_init(&queue.mutex, NULL) != 0) fatal(ERR_RUNTIME, "Queue mutex initialization failed");
        queue.fwd_next = fetch_first; queue.fwd_last = fetch_last;
        queue_expand_forward(&queue);
        WorkerArgs wa = {0};
        wa.db = &db; wa.group = cur_group; wa.host = host; wa.port = port;
        wa.user = user; wa.pass = pass; wa.use_ssl = use_ssl; wa.do_starttls = do_starttls;
        wa.headers_only = headers_only; wa.retries = retries; wa.progress_width = progress_width;
        wa.total_threads = group_threads; wa.start_time = now_sec();
        if (!forward_mode && limit > 0 && (exact_article_target || limit >= 100)) {
            long long group_span = last - first + 1;
            wa.target_articles = (limit < group_span) ? limit : total;
            wa.total = wa.target_articles;
        } else {
            wa.target_articles = 0;
            wa.total = total;
        }
        wa.queue = &queue;
        snprintf(wa.last_item, sizeof(wa.last_item), "Connecting (%lld..%lld)...", fetch_first, fetch_last);
        if (pthread_mutex_init(&wa.mutex, NULL) != 0) fatal(ERR_RUNTIME, "Worker mutex initialization failed");
        pthread_mutex_lock(&wa.mutex);
        render_progress(&wa, 0);
        pthread_mutex_unlock(&wa.mutex);
        pthread_t tids[64];
        int started = 0;
        for (int t = 0; t < group_threads; ++t) {
            int error = pthread_create(&tids[started], NULL, header_worker, &wa);
            if (error) warnf("Worker creation failed: %s; continuing with available workers", strerror(error));
            else started++;
        }
        if (started > 0) {
            pthread_mutex_lock(&wa.mutex);
            wa.total_threads = started;
            pthread_mutex_unlock(&wa.mutex);
        }
        /* A resource-limited host can still complete the queue synchronously. */
        if (!started) {
            wa.total_threads = 1;
            header_worker(&wa);
        }
        for (int t = 0; t < started; ++t) {
            int error = pthread_join(tids[t], NULL);
            if (error) fatal(ERR_RUNTIME, "Worker join failed: %s", strerror(error));
        }
        total_missing_all += wa.missing;
        int download_failed = wa.any_failed || queue.head != NULL || queue_any_tree_failed(&queue);
        pthread_mutex_lock(&wa.mutex);
        if (!download_failed && !g_stop_requested && wa.target_articles > 0) {
            if (wa.stored < wa.total) {
                wa.total = wa.stored > 0 ? wa.stored : 1;
            }
            wa.completed = wa.total;
        }
        if (g_stop_requested) {
            snprintf(wa.last_item, sizeof(wa.last_item), "Interrupted (Ctrl+C)");
        } else {
            snprintf(wa.last_item, sizeof(wa.last_item), "%s", download_failed ? "Incomplete" : "Complete");
        }
        render_progress(&wa, 1);
        pthread_mutex_unlock(&wa.mutex);

        queue_free_all(&queue);
        pthread_mutex_destroy(&queue.mutex);
        pthread_mutex_destroy(&wa.mutex);

        if (g_stop_requested) {
            for (size_t k = 0; k < group_count; ++k) free(groups_to_fetch[k]);
            free(groups_to_fetch);
            report_interrupt_and_exit(&db, NULL, total_missing_all);
        }

        fprintf(stdout, "\nDownload %s: %lld stored, %lld missing (%lld SQL records updated)\n",
                download_failed ? "incomplete" : "complete", wa.stored, wa.missing, (long long)g_sql_records_updated);
        fflush(stdout);
        if (download_failed) any_download_failed = 1;
    }

    for (size_t k = 0; k < group_count; ++k) free(groups_to_fetch[k]);
    free(groups_to_fetch);

    db_close(&db);
    log_close();
    return any_download_failed ? ERR_RUNTIME : ERR_OK;
}
