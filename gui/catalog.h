/* Internal viewer model. GLib GTree is a balanced binary search tree.
 * Subject-based grouping is provisional; yEnc bodies are authoritative. */
#ifndef VIEWER_CATALOG_H
#define VIEWER_CATALOG_H
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <gio/gio.h>
#include <sqlite3.h>
#if defined(__has_include) && __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mysql.h>
#endif
#ifdef HAVE_PQ
#include <libpq-fe.h>
#endif
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct NewsFile NewsFile;

typedef struct {
    gint64 number;
    char *subject, *author, *date, *message;
    char *refs; /* References header, kept for text articles only (threading and replies) */
    const char *group; /* interned in NewsCatalog.groups */
    guint part, total;
    gboolean yenc, queued;
    NewsFile *file;
} NewsArticle;

struct NewsFile {
    char *key, *title, *author, *status, *saved_path;
    const char *group; /* interned in NewsCatalog.groups */
    guint total, parts_count;
    gboolean ambiguous, queued;
    GTree *parts; /* part number -> borrowed NewsArticle */
    double elapsed;
};

typedef struct {
    GPtrArray *articles, *files, *groups;
    GTree *index; /* normalized subject + group + author -> borrowed NewsFile */
    GTree *group_tree; /* group name -> borrowed group string */
    const char *last_group;
    char *resolved_database;
    char *resolved_connection;
    char *default_host;
    char *default_group;
    char *default_user, *default_pass; /* stored by `nntp2sql --save-config` */
    int stored_ssl, stored_starttls;   /* -1 until the settings row is seen */
    guint default_port;
    guint default_security;
    guint default_workers;
    gboolean has_default_security;
} NewsCatalog;

static gint number_compare(gconstpointer a, gconstpointer b, gpointer unused) {
    (void)unused;
    return (GPOINTER_TO_UINT(a) > GPOINTER_TO_UINT(b)) - (GPOINTER_TO_UINT(a) < GPOINTER_TO_UINT(b));
}
static gint text_compare(gconstpointer a, gconstpointer b, gpointer unused) {
    (void)unused; return g_strcmp0(a, b);
}
static gint group_ptr_compare(gconstpointer a, gconstpointer b) {
    const char * const *ga = a;
    const char * const *gb = b;
    return g_strcmp0(*ga, *gb);
}
static void article_free(gpointer value) {
    NewsArticle *a = value;
    g_free(a->subject); g_free(a->author); g_free(a->date); g_free(a->message); g_free(a->refs); g_free(a);
}
static void file_free(gpointer value) {
    NewsFile *f = value;
    g_tree_destroy(f->parts); g_free(f->key); g_free(f->title);
    g_free(f->author); g_free(f->status); g_free(f->saved_path); g_free(f);
}
static NewsCatalog *catalog_new(void) {
    NewsCatalog *c = g_new0(NewsCatalog, 1);
    c->articles = g_ptr_array_new_full(4096, article_free);
    c->files = g_ptr_array_new_full(1024, file_free);
    c->groups = g_ptr_array_new_with_free_func(g_free);
    c->index = g_tree_new_full(text_compare, NULL, NULL, NULL);
    c->group_tree = g_tree_new_full(text_compare, NULL, NULL, NULL);
    c->stored_ssl = c->stored_starttls = -1;
    return c;
}
static void catalog_free(gpointer value) {
    NewsCatalog *c = value;
    if (!c) return;
    g_tree_destroy(c->group_tree); g_tree_destroy(c->index);
    g_ptr_array_unref(c->files); g_ptr_array_unref(c->articles); g_ptr_array_unref(c->groups);
    g_free(c->resolved_database);
    g_free(c->resolved_connection);
    g_free(c->default_host);
    g_free(c->default_group);
    g_free(c->default_user);
    if (c->default_pass) { memset(c->default_pass, 0, strlen(c->default_pass)); g_free(c->default_pass); }
    g_free(c);
}
static const char *catalog_intern_group(NewsCatalog *c, const char *group) {
    if (!group || !*group) return "";
    if (c->last_group && strcmp(c->last_group, group) == 0) return c->last_group;
    const char *existing = g_tree_lookup(c->group_tree, group);
    if (existing) {
        c->last_group = existing;
        return existing;
    }
    char *valid = g_utf8_make_valid(group, -1);
    g_strstrip(valid);
    if (!*valid) { g_free(valid); return ""; }
    existing = g_tree_lookup(c->group_tree, valid);
    if (existing) {
        g_free(valid);
        c->last_group = existing;
        return existing;
    }
    g_ptr_array_add(c->groups, valid);
    g_tree_insert(c->group_tree, valid, valid);
    c->last_group = valid;
    return valid;
}
static void catalog_add_group(NewsCatalog *c, const char *group) {
    (void)catalog_intern_group(c, group);
}
static gboolean file_has_headers(const NewsFile *f) {
    return !f->ambiguous && f->total > 0 && f->parts_count == f->total;
}
static char *file_state(const NewsFile *f) {
    if (f->status) return g_strdup(f->status);
    if (f->ambiguous) return g_strdup("Ambiguous posting — conflicting parts");
    return g_strdup_printf("%s: %u/%u part headers%s",
        file_has_headers(f) ? "Headers complete" : "Incomplete",
        f->parts_count, f->total,
        file_has_headers(f) ? " — body unverified" : "");
}

/* Zero-allocation case-insensitive substring check for 2M+ record scanning. */
static gboolean ascii_icontains_len(const char *haystack, const char *needle, gsize nlen) {
    if (!nlen) return TRUE;
    if (!haystack || !*haystack) return FALSE;
    char first = g_ascii_tolower(needle[0]);
    for (const char *p = haystack; *p; ++p) {
        if (g_ascii_tolower(*p) == first) {
            gsize i = 1;
            while (i < nlen && p[i] && g_ascii_tolower(p[i]) == g_ascii_tolower(needle[i])) i++;
            if (i == nlen) return TRUE;
        }
    }
    return FALSE;
}

static gboolean ascii_icontains(const char *haystack, const char *needle) {
    return needle ? ascii_icontains_len(haystack, needle, strlen(needle)) : TRUE;
}

/* Fast, zero-allocation linear scanner for [part/total] or (part/total) markers.
 * Replaces compiling a PCRE2 GRegex on every single row across 2M+ articles. */
static void parse_last_part_marker(const char *subject, guint *out_part, guint *out_total, int *out_begin, int *out_end) {
    *out_part = 1;
    *out_total = 1;
    *out_begin = -1;
    *out_end = -1;
    if (!subject) return;
    for (const char *p = subject; *p; ++p) {
        if (*p == '(' || *p == '[') {
            const char *start = p;
            const char *q = p + 1;
            if (!g_ascii_isdigit(*q)) continue;
            guint64 part = 0;
            while (g_ascii_isdigit(*q)) {
                part = part * 10 + (guint64)(*q - '0');
                if (part > 10000000ULL) break;
                q++;
            }
            if (*q != '/' || part == 0 || part > 10000000ULL) continue;
            q++;
            if (!g_ascii_isdigit(*q)) continue;
            guint64 total = 0;
            while (g_ascii_isdigit(*q)) {
                total = total * 10 + (guint64)(*q - '0');
                if (total > 10000000ULL) break;
                q++;
            }
            if ((*q == ')' || *q == ']') && total > 0 && part <= total && total <= 10000000ULL) {
                *out_part = (guint)part;
                *out_total = (guint)total;
                *out_begin = (int)(start - subject);
                *out_end = (int)((q + 1) - subject);
            }
        }
    }
}

static void catalog_add(NewsCatalog *c, const char *number, const char *subject,
                        const char *author, const char *date, const char *group, const char *message,
                        const char *refs) {
    NewsArticle *a = g_new0(NewsArticle, 1);
    a->number = g_ascii_strtoll(number ? number : "0", NULL, 10);
    const char *raw_subj = subject ? subject : "";
    const char *raw_auth = author ? author : "";
    const char *raw_date = date ? date : "";
    a->subject = g_utf8_validate(raw_subj, -1, NULL) ? g_strdup(raw_subj) : g_utf8_make_valid(raw_subj, -1);
    a->author = g_utf8_validate(raw_auth, -1, NULL) ? g_strdup(raw_auth) : g_utf8_make_valid(raw_auth, -1);
    a->date = g_utf8_validate(raw_date, -1, NULL) ? g_strdup(raw_date) : g_utf8_make_valid(raw_date, -1);
    a->group = catalog_intern_group(c, group ? group : "");
    a->message = g_strdup(message ? message : "");
    g_ptr_array_add(c->articles, a);
    a->yenc = ascii_icontains(a->subject, "yenc");
    if (!a->yenc) { a->refs = g_strdup(refs ? refs : ""); return; }
    int begin = -1, end = -1;
    parse_last_part_marker(a->subject, &a->part, &a->total, &begin, &end);
    char stack_base[1024];
    char *heap_base = NULL;
    char *base = stack_base;
    gsize subj_len = strlen(a->subject);
    if (subj_len < sizeof(stack_base)) {
        if (begin >= 0) {
            memcpy(stack_base, a->subject, (gsize)begin);
            strcpy(stack_base + begin, a->subject + end);
        } else {
            memcpy(stack_base, a->subject, subj_len + 1);
        }
    } else {
        if (begin >= 0) {
            char *prefix = g_strndup(a->subject, (gsize)begin);
            heap_base = g_strconcat(prefix, a->subject + end, NULL);
            g_free(prefix);
        } else heap_base = g_strdup(a->subject);
        base = heap_base;
    }
    g_strstrip(base);
    char stack_key[1536];
    char *heap_key = NULL;
    char *key = stack_key;
    int klen = g_snprintf(stack_key, sizeof(stack_key), "%zu:%s%zu:%s%u:%s",
                          strlen(a->group), a->group, strlen(a->author), a->author, a->total, base);
    if (klen < 0 || (gsize)klen >= sizeof(stack_key)) {
        heap_key = g_strdup_printf("%zu:%s%zu:%s%u:%s", strlen(a->group), a->group,
                                   strlen(a->author), a->author, a->total, base);
        key = heap_key;
    }
    NewsFile *f = g_tree_lookup(c->index, key);
    if (!f) {
        f = g_new0(NewsFile, 1); f->key = g_strdup(key); f->title = g_strdup(base);
        f->group = a->group; f->author = g_strdup(a->author); f->total = a->total;
        f->parts = g_tree_new_full(number_compare, NULL, NULL, NULL);
        g_tree_insert(c->index, f->key, f); g_ptr_array_add(c->files, f);
    }
    a->file = f;
    NewsArticle *previous = g_tree_lookup(f->parts, GUINT_TO_POINTER(a->part));
    if (previous) {
        if (!*a->message || strcmp(previous->message, a->message)) f->ambiguous = TRUE;
    } else {
        g_tree_insert(f->parts, GUINT_TO_POINTER(a->part), a);
        f->parts_count++;
    }
    g_free(heap_base); g_free(heap_key);
}

/* Universal multi-token search across all file and article metadata fields. */
static gboolean catalog_file_matches(const NewsFile *f, const char *query) {
    if (!query || !*query) return TRUE;
    char parts_buf[64];
    g_snprintf(parts_buf, sizeof(parts_buf), "%u/%u", f->parts_count, f->total);
    gboolean is_complete = file_has_headers(f);
    const char *state_kw = f->status ? f->status :
        (f->ambiguous ? "ambiguous conflicting" :
         (is_complete ? "headers complete ready" : "missing-parts partial"));
    const char *queue_kw = f->queued ? "queued checked selected" : "not-in-queue unchecked";
    const char *p = query;
    while (*p) {
        while (*p && g_ascii_isspace(*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !g_ascii_isspace(*p)) p++;
        gsize tlen = (gsize)(p - start);
        gboolean want_incomplete = (tlen == 10 && g_ascii_strncasecmp(start, "incomplete", 10) == 0);
        gboolean want_unqueued = (tlen == 8 && g_ascii_strncasecmp(start, "unqueued", 8) == 0);
        if (want_incomplete) {
            if (is_complete) return FALSE;
            continue;
        }
        if (want_unqueued) {
            if (f->queued) return FALSE;
            continue;
        }
        if (!ascii_icontains_len(f->title, start, tlen) &&
            !ascii_icontains_len(f->author, start, tlen) &&
            !ascii_icontains_len(f->group, start, tlen) &&
            !ascii_icontains_len(parts_buf, start, tlen) &&
            !ascii_icontains_len(state_kw, start, tlen) &&
            !ascii_icontains_len(queue_kw, start, tlen) &&
            !ascii_icontains_len("yenc file", start, tlen) &&
            !(f->saved_path && ascii_icontains_len(f->saved_path, start, tlen))) {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean catalog_article_matches(const NewsArticle *a, const char *query) {
    if (!query || !*query) return TRUE;
    char num_buf[64];
    g_snprintf(num_buf, sizeof(num_buf), "#%" G_GINT64_FORMAT " %u/%u", a->number, a->part, a->total);
    const char *type_kw = a->yenc ? "yenc file binary" : "article text";
    gboolean is_complete = a->file ? file_has_headers(a->file) : TRUE;
    const char *state_kw = a->file ? (a->file->status ? a->file->status :
        (a->file->ambiguous ? "ambiguous" : (is_complete ? "headers complete ready" : "missing-parts partial"))) : "";
    gboolean is_queued = a->queued || (a->file && a->file->queued);
    const char *queue_kw = is_queued ? "queued checked selected" : "not-in-queue unchecked";
    const char *p = query;
    while (*p) {
        while (*p && g_ascii_isspace(*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !g_ascii_isspace(*p)) p++;
        gsize tlen = (gsize)(p - start);
        gboolean want_incomplete = (tlen == 10 && g_ascii_strncasecmp(start, "incomplete", 10) == 0);
        gboolean want_unqueued = (tlen == 8 && g_ascii_strncasecmp(start, "unqueued", 8) == 0);
        if (want_incomplete) {
            if (!a->file || is_complete) return FALSE;
            continue;
        }
        if (want_unqueued) {
            if (is_queued) return FALSE;
            continue;
        }
        if (!ascii_icontains_len(a->subject, start, tlen) &&
            !ascii_icontains_len(a->author, start, tlen) &&
            !ascii_icontains_len(a->group, start, tlen) &&
            !ascii_icontains_len(a->date, start, tlen) &&
            !ascii_icontains_len(a->message, start, tlen) &&
            !ascii_icontains_len(num_buf, start, tlen) &&
            !ascii_icontains_len(type_kw, start, tlen) &&
            !ascii_icontains_len(state_kw, start, tlen) &&
            !ascii_icontains_len(queue_kw, start, tlen)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Default connection string for a standard local installation of each backend. */
static const char *catalog_default_connection(const char *type) {
    if (!type || !strcmp(type, "sqlite")) return "nntp2sql.db";
    if (!strcmp(type, "mysql") || !strcmp(type, "mariadb")) return "127.0.0.1,nntp2sql,root,,3306";
    if (!strcmp(type, "postgres") || !strcmp(type, "pgsql")) return "127.0.0.1,nntp2sql,postgres,,5432";
    return "nntp2sql.db";
}

static const char *catalog_default_placeholder(const char *type) {
    if (!type || !strcmp(type, "sqlite"))
        return "nntp2sql.db (SQLite file path)";
    if (!strcmp(type, "mysql") || !strcmp(type, "mariadb"))
        return "127.0.0.1,nntp2sql,root,,3306 (host,database,user,password,port)";
    if (!strcmp(type, "postgres") || !strcmp(type, "pgsql"))
        return "127.0.0.1,nntp2sql,postgres,,5432 (host,database,user,password,port)";
    return "Database path or connection string";
}

static void catalog_apply_setting(NewsCatalog *c, const char *k, const char *v) {
    if (!c || !k || !v) return;
    if (!strcmp(k, "host") && *v) {
        g_free(c->default_host);
        c->default_host = g_strdup(v);
    } else if (!strcmp(k, "port") && atoi(v) >= 1 && atoi(v) <= 65535) {
        c->default_port = (guint)atoi(v);
    } else if (!strcmp(k, "ssl") || !strcmp(k, "starttls")) {
        if (!strcmp(k, "ssl")) c->stored_ssl = atoi(v) ? 1 : 0; else c->stored_starttls = atoi(v) ? 1 : 0;
        /* TLS wins, then STARTTLS; both stored as 0 means an explicit plaintext connection. */
        c->default_security = c->stored_ssl > 0 ? 0 : c->stored_starttls > 0 ? 1 : 2;
        c->has_default_security = TRUE;
    } else if (!strcmp(k, "user")) {
        g_free(c->default_user);
        c->default_user = g_strdup(v);
    } else if (!strcmp(k, "pass")) {
        g_free(c->default_pass);
        c->default_pass = g_strdup(v);
    } else if (!strcmp(k, "threads") && atoi(v) >= 1 && atoi(v) <= 64) {
        c->default_workers = (guint)atoi(v);
    } else if (!strcmp(k, "group") && *v) {
        g_free(c->default_group);
        c->default_group = g_strdup(v);
        catalog_add_group(c, v);
    }
}

typedef struct {
    char *host;
    char *database;
    char *user;
    char *password;
    guint port;
    gboolean is_default_db;
    gboolean is_default_user;
    gboolean is_default_host;
} CatalogConnParams;

static void catalog_conn_params_clear(CatalogConnParams *p) {
    if (!p) return;
    g_free(p->host);
    g_free(p->database);
    g_free(p->user);
    g_free(p->password);
    memset(p, 0, sizeof(*p));
}

/* Parses MySQL/MariaDB or PostgreSQL connection strings in any of:
 *   1. "host,database,user,password[,port]" (default viewer format)
 *   2. "database_name" (uses default local host, user, password, port)
 *   3. "mysql://user:pass@host:port/dbname" or "postgresql://..."
 *   4. "host=127.0.0.1 dbname=news0 user=root password=... port=3306"
 *   5. "" (empty -> uses default local installation parameters) */
static void catalog_parse_server_connection(const char *type, const char *connection, CatalogConnParams *out) {
    memset(out, 0, sizeof(*out));
    gboolean is_pg = type && (!strcmp(type, "postgres") || !strcmp(type, "pgsql"));
    const char *def_user = is_pg ? "postgres" : "root";
    guint def_port = is_pg ? 5432 : 3306;

    out->host = g_strdup("127.0.0.1");
    out->database = g_strdup("nntp2sql");
    out->user = g_strdup(def_user);
    out->password = g_strdup("");
    out->port = def_port;
    out->is_default_db = TRUE;
    out->is_default_user = TRUE;
    out->is_default_host = TRUE;

    if (!connection) return;
    char *trimmed = g_strdup(connection);
    g_strstrip(trimmed);
    if (!*trimmed) {
        g_free(trimmed);
        return;
    }

    const char *uri_sep = strstr(trimmed, "://");
    if (uri_sep) {
        const char *rest = uri_sep + 3;
        const char *slash = strchr(rest, '/');
        char *authority = slash ? g_strndup(rest, (gsize)(slash - rest)) : g_strdup(rest);
        if (slash && slash[1]) {
            char *db_part = g_strdup(slash + 1);
            char *q = strchr(db_part, '?');
            if (q) *q = '\0';
            if (*db_part) {
                g_free(out->database);
                out->database = g_strdup(db_part);
                out->is_default_db = (strcmp(out->database, "nntp2sql") == 0);
            }
            g_free(db_part);
        }
        char *at = strchr(authority, '@');
        char *hostport = authority;
        if (at) {
            *at = '\0';
            hostport = at + 1;
            char *colon = strchr(authority, ':');
            if (colon) {
                *colon = '\0';
                g_free(out->password);
                out->password = g_strdup(colon + 1);
            }
            if (*authority) {
                g_free(out->user);
                out->user = g_strdup(authority);
                out->is_default_user = (strcmp(out->user, def_user) == 0 && !*out->password);
            }
        }
        char *pcolon = strchr(hostport, ':');
        if (pcolon) {
            *pcolon = '\0';
            int pv = atoi(pcolon + 1);
            if (pv >= 1 && pv <= 65535) out->port = (guint)pv;
        }
        if (*hostport) {
            g_free(out->host);
            out->host = g_strdup(hostport);
            out->is_default_host = (!strcmp(out->host, "127.0.0.1") || !strcmp(out->host, "localhost"));
        }
        g_free(authority);
        g_free(trimmed);
        return;
    }

    if (strchr(trimmed, ',') != NULL) {
        char **v = g_strsplit(trimmed, ",", 5);
        guint n = g_strv_length(v);
        if (n >= 1 && v[0]) {
            g_strstrip(v[0]);
            if (*v[0]) {
                g_free(out->host);
                out->host = g_strdup(v[0]);
                out->is_default_host = (!strcmp(out->host, "127.0.0.1") || !strcmp(out->host, "localhost"));
            }
        }
        if (n >= 2 && v[1]) {
            g_strstrip(v[1]);
            if (*v[1]) {
                g_free(out->database);
                out->database = g_strdup(v[1]);
                out->is_default_db = (strcmp(out->database, "nntp2sql") == 0);
            } else {
                out->is_default_db = TRUE;
            }
        }
        if (n >= 3 && v[2]) {
            g_strstrip(v[2]);
            if (*v[2]) {
                g_free(out->user);
                out->user = g_strdup(v[2]);
            }
        }
        if (n >= 4 && v[3]) {
            g_free(out->password);
            out->password = g_strdup(v[3]);
        }
        if (n >= 5 && v[4]) {
            g_strstrip(v[4]);
            int pv = atoi(v[4]);
            if (pv >= 1 && pv <= 65535) out->port = (guint)pv;
        }
        out->is_default_user = (!strcmp(out->user, def_user) && (!out->password || !*out->password));
        g_strfreev(v);
        g_free(trimmed);
        return;
    }

    if (strchr(trimmed, '=') != NULL) {
        char **tokens = g_strsplit_set(trimmed, " ;", -1);
        for (guint i = 0; tokens && tokens[i]; ++i) {
            char *eq = strchr(tokens[i], '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = tokens[i], *val = eq + 1;
            g_strstrip(k); g_strstrip(val);
            if (!g_ascii_strcasecmp(k, "host") && *val) {
                g_free(out->host);
                out->host = g_strdup(val);
                out->is_default_host = (!strcmp(out->host, "127.0.0.1") || !strcmp(out->host, "localhost"));
            } else if ((!g_ascii_strcasecmp(k, "dbname") || !g_ascii_strcasecmp(k, "database") || !g_ascii_strcasecmp(k, "db")) && *val) {
                g_free(out->database);
                out->database = g_strdup(val);
                out->is_default_db = (strcmp(out->database, "nntp2sql") == 0);
            } else if ((!g_ascii_strcasecmp(k, "user") || !g_ascii_strcasecmp(k, "username")) && *val) {
                g_free(out->user);
                out->user = g_strdup(val);
            } else if (!g_ascii_strcasecmp(k, "password") || !g_ascii_strcasecmp(k, "pass")) {
                g_free(out->password);
                out->password = g_strdup(val);
            } else if (!g_ascii_strcasecmp(k, "port") && atoi(val) >= 1 && atoi(val) <= 65535) {
                out->port = (guint)atoi(val);
            }
        }
        out->is_default_user = (!strcmp(out->user, def_user) && (!out->password || !*out->password));
        g_strfreev(tokens);
        g_free(trimmed);
        return;
    }

    /* Plain database name (e.g. "news0") -> use default local host, user, password, port */
    g_free(out->database);
    out->database = g_strdup(trimmed);
    out->is_default_db = (strcmp(out->database, "nntp2sql") == 0);
    g_free(trimmed);
}

static char *catalog_format_connection_for_db(const char *type, const char *current_connection, const char *db_name) {
    if (!type || !strcmp(type, "sqlite")) {
        return g_strdup((db_name && *db_name) ? db_name : "nntp2sql.db");
    }
    CatalogConnParams p;
    catalog_parse_server_connection(type, current_connection, &p);
    const char *target_db = (db_name && *db_name) ? db_name : p.database;
    char *formatted = g_strdup_printf("%s,%s,%s,%s,%u", p.host, target_db, p.user, p.password, p.port);
    catalog_conn_params_clear(&p);
    return formatted;
}

/* Verifies whether a local file is a SQLite database with the nntp2sql schema
 * (articles table with artnum/group_name/subject, groups table with article_count/first/last,
 * or settings table with schema_version). */
static gboolean catalog_sqlite_matches_schema(const char *path) {
    if (!path || !*path) return FALSE;
    if (g_str_has_suffix(path, "-wal") || g_str_has_suffix(path, "-shm") || g_str_has_suffix(path, "-journal"))
        return FALSE;
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) return FALSE;

    FILE *fp = fopen(path, "rb");
    if (!fp) return FALSE;
    char magic[16] = {0};
    size_t nread = fread(magic, 1, sizeof(magic), fp);
    fclose(fp);
    if (nread < 16 || memcmp(magic, "SQLite format 3\0", 16) != 0) return FALSE;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return FALSE;
    }
    sqlite3_busy_timeout(db, 1500);
    /* Real nntp2sql schema: articles(artnum, subject, group_name, ...) plus groups(name, article_count, ...).
     * Generic tables such as settings(key, value) must not count, or unrelated databases would match. */
    const char *schema_sql =
        "SELECT "
        "  (SELECT COUNT(*) FROM pragma_table_info('articles') WHERE LOWER(name) IN ('artnum', 'group_name', 'subject')) = 3 "
        "  AND (SELECT COUNT(*) FROM pragma_table_info('groups') WHERE LOWER(name) IN ('name', 'article_count')) = 2";
    sqlite3_stmt *stmt = NULL;
    int matches = 0;
    if (sqlite3_prepare_v2(db, schema_sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            matches = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return matches == 1;
}

static void catalog_scan_sqlite_dir(const char *dir_path, gboolean prefer_basename,
                                    GHashTable *seen_canon, GPtrArray *out) {
    if (!dir_path || !*dir_path || !g_file_test(dir_path, G_FILE_TEST_IS_DIR)) return;
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) return;
    const char *entry = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (entry[0] == '.') continue;
        char *full = g_build_filename(dir_path, entry, NULL);
        if (catalog_sqlite_matches_schema(full)) {
            char *canon = g_canonicalize_filename(full, NULL);
            if (!g_hash_table_contains(seen_canon, canon)) {
                g_hash_table_add(seen_canon, g_strdup(canon));
                g_ptr_array_add(out, prefer_basename ? g_strdup(entry) : g_strdup(full));
            }
            g_free(canon);
        }
        g_free(full);
    }
    g_dir_close(dir);
}

/* Connects to MySQL/MariaDB server with automatic local fallback (127.0.0.1 <-> localhost,
 * root <-> current OS user) when default connection settings are used. */
static MYSQL *catalog_mysql_connect(CatalogConnParams *p, const char *override_db, GError **error) {
    const char *target_db = override_db;
    const char *hosts[3] = { p->host, NULL, NULL };
    int nhosts = 1;
    if (p->is_default_host) {
        hosts[nhosts++] = strcmp(p->host, "127.0.0.1") == 0 ? "localhost" : "127.0.0.1";
    }
    const char *os_user = g_get_user_name();
    const char *users[3] = { p->user, NULL, NULL };
    int nusers = 1;
    if (p->is_default_user && os_user && *os_user && strcmp(os_user, p->user) != 0) {
        users[nusers++] = os_user;
    }

    MYSQL *last_db = NULL;
    for (int ui = 0; ui < nusers; ++ui) {
        for (int hi = 0; hi < nhosts; ++hi) {
            MYSQL *db = mysql_init(NULL);
            if (!db) continue;
            unsigned int timeout = 5;
            mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
            mysql_options(db, MYSQL_OPT_READ_TIMEOUT, &timeout);
            if (mysql_real_connect(db, hosts[hi], users[ui], p->password, target_db, p->port, NULL, 0)) {
                if (last_db) mysql_close(last_db);
                mysql_set_character_set(db, "utf8mb4");
                if (strcmp(p->host, hosts[hi]) != 0) {
                    g_free(p->host);
                    p->host = g_strdup(hosts[hi]);
                }
                if (strcmp(p->user, users[ui]) != 0) {
                    g_free(p->user);
                    p->user = g_strdup(users[ui]);
                }
                return db;
            }
            /* If server reached but specific database does not exist (1049 ER_BAD_DB_ERROR), stop host/user loop */
            if (target_db && mysql_errno(db) == 1049) {
                if (last_db) mysql_close(last_db);
                last_db = db;
                goto fail;
            }
            if (last_db) mysql_close(last_db);
            last_db = db;
        }
    }
fail:
    if (error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "MySQL: %s",
                    last_db ? mysql_error(last_db) : "cannot initialize MySQL client");
    }
    if (last_db) mysql_close(last_db);
    return NULL;
}

/* Dynamically loads libpq on macOS / Linux if HAVE_PQ was not enabled at compile time,
 * so PostgreSQL works out of the box in both Xcode and Autotools builds. */
typedef struct PGconn PGconn_t;
typedef struct PGresult PGresult_t;
typedef struct {
    void *handle;
    PGconn_t *(*connectdb)(const char *conninfo);
    int (*status)(const PGconn_t *conn);
    char *(*errorMessage)(const PGconn_t *conn);
    PGresult_t *(*exec)(PGconn_t *conn, const char *query);
    int (*resultStatus)(const PGresult_t *res);
    char *(*resultErrorMessage)(const PGresult_t *res);
    int (*ntuples)(const PGresult_t *res);
    char *(*getvalue)(const PGresult_t *res, int tup_num, int field_num);
    void (*clear)(PGresult_t *res);
    void (*finish)(PGconn_t *conn);
    gboolean ready;
} PgApi;

static gboolean catalog_load_pg_api(PgApi *api, GError **error) {
    memset(api, 0, sizeof(*api));
#ifdef HAVE_PQ
    api->connectdb = (PGconn_t *(*)(const char *))PQconnectdb;
    api->status = (int (*)(const PGconn_t *))PQstatus;
    api->errorMessage = (char *(*)(const PGconn_t *))PQerrorMessage;
    api->exec = (PGresult_t *(*)(PGconn_t *, const char *))PQexec;
    api->resultStatus = (int (*)(const PGresult_t *))PQresultStatus;
    api->resultErrorMessage = (char *(*)(const PGresult_t *))PQresultErrorMessage;
    api->ntuples = (int (*)(const PGresult_t *))PQntuples;
    api->getvalue = (char *(*)(const PGresult_t *, int, int))PQgetvalue;
    api->clear = (void (*)(PGresult_t *))PQclear;
    api->finish = (void (*)(PGconn_t *))PQfinish;
    api->ready = TRUE;
    return TRUE;
#else
    static const char *candidates[] = {
        "libpq.dylib",
        "/opt/homebrew/lib/libpq.dylib",
        "/opt/homebrew/opt/libpq/lib/libpq.dylib",
        "/usr/local/lib/libpq.dylib",
        "/usr/local/opt/libpq/lib/libpq.dylib",
        "/Applications/Postgres.app/Contents/Versions/latest/lib/libpq.dylib",
        "libpq.so.5",
        "libpq.so",
        NULL
    };
    void *h = NULL;
    for (int i = 0; candidates[i]; ++i) {
        h = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL);
        if (h) break;
    }
    if (!h) {
        if (error) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "PostgreSQL client library (libpq) not found. Install libpq/postgresql (e.g. 'brew install libpq').");
        }
        return FALSE;
    }
    api->handle = h;
    api->connectdb = (PGconn_t *(*)(const char *))dlsym(h, "PQconnectdb");
    api->status = (int (*)(const PGconn_t *))dlsym(h, "PQstatus");
    api->errorMessage = (char *(*)(const PGconn_t *))dlsym(h, "PQerrorMessage");
    api->exec = (PGresult_t *(*)(PGconn_t *, const char *))dlsym(h, "PQexec");
    api->resultStatus = (int (*)(const PGresult_t *))dlsym(h, "PQresultStatus");
    api->resultErrorMessage = (char *(*)(const PGresult_t *))dlsym(h, "PQresultErrorMessage");
    api->ntuples = (int (*)(const PGresult_t *))dlsym(h, "PQntuples");
    api->getvalue = (char *(*)(const PGresult_t *, int, int))dlsym(h, "PQgetvalue");
    api->clear = (void (*)(PGresult_t *))dlsym(h, "PQclear");
    api->finish = (void (*)(PGconn_t *))dlsym(h, "PQfinish");
    if (!api->connectdb || !api->status || !api->errorMessage || !api->exec ||
        !api->resultStatus || !api->resultErrorMessage || !api->ntuples ||
        !api->getvalue || !api->clear || !api->finish) {
        if (error) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "PostgreSQL: incomplete libpq symbol table");
        }
        return FALSE;
    }
    api->ready = TRUE;
    return TRUE;
#endif
}

/* libpq conninfo values are single-quoted; backslash and quote must be escaped. */
static char *catalog_pg_quote(const char *v) {
    GString *out = g_string_new("'");
    for (const char *c = v ? v : ""; *c; ++c) {
        if (*c == '\\' || *c == '\'') g_string_append_c(out, '\\');
        g_string_append_c(out, *c);
    }
    g_string_append_c(out, '\'');
    return g_string_free(out, FALSE);
}

static PGconn_t *catalog_pg_connect(PgApi *api, CatalogConnParams *p, const char *target_db, GError **error) {
    const char *os_user = g_get_user_name();
    const char *users[3] = { p->user, NULL, NULL };
    int nusers = 1;
    if (p->is_default_user && os_user && *os_user && strcmp(os_user, p->user) != 0) {
        users[nusers++] = os_user;
    }
    PGconn_t *last_conn = NULL;
    for (int ui = 0; ui < nusers; ++ui) {
        char *qh = catalog_pg_quote(p->host), *qd = catalog_pg_quote(target_db ? target_db : p->database);
        char *qu = catalog_pg_quote(users[ui]), *qp = catalog_pg_quote(p->password);
        char *conninfo = g_strdup_printf("host=%s port=%u dbname=%s user=%s password=%s connect_timeout=5",
                                         qh, p->port, qd, qu, qp);
        g_free(qh); g_free(qd); g_free(qu); g_free(qp);
        PGconn_t *conn = api->connectdb(conninfo);
        g_free(conninfo);
        if (conn && api->status(conn) == 0 /* CONNECTION_OK */) {
            if (last_conn) api->finish(last_conn);
            if (strcmp(p->user, users[ui]) != 0) {
                g_free(p->user);
                p->user = g_strdup(users[ui]);
            }
            return conn;
        }
        if (last_conn) api->finish(last_conn);
        last_conn = conn;
    }
    if (error) {
        const char *msg = last_conn ? api->errorMessage(last_conn) : "cannot connect to PostgreSQL";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "PostgreSQL: %s", msg);
    }
    if (last_conn) api->finish(last_conn);
    return NULL;
}

/* Discovers all nntp2sql databases matching the nntp2sql table/column schema
 * (e.g. news0, news1, nntp2sql) across SQLite, MySQL/MariaDB, and PostgreSQL. */
static GPtrArray *catalog_discover_databases(const char *type, const char *connection, GError **error) {
    GPtrArray *dbs = g_ptr_array_new_with_free_func(g_free);
    if (!type || !strcmp(type, "sqlite")) {
        GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        if (connection && *connection) {
            char *trimmed = g_strdup(connection);
            g_strstrip(trimmed);
            if (g_file_test(trimmed, G_FILE_TEST_IS_DIR)) {
                catalog_scan_sqlite_dir(trimmed, FALSE, seen, dbs);
            } else {
                if (catalog_sqlite_matches_schema(trimmed)) {
                    char *canon = g_canonicalize_filename(trimmed, NULL);
                    g_hash_table_add(seen, canon);
                    g_ptr_array_add(dbs, g_strdup(trimmed));
                }
                char *parent = g_path_get_dirname(trimmed);
                if (parent && *parent) {
                    gboolean is_dot = (strcmp(parent, ".") == 0);
                    catalog_scan_sqlite_dir(parent, is_dot, seen, dbs);
                }
                g_free(parent);
            }
            g_free(trimmed);
        }
        catalog_scan_sqlite_dir(".", TRUE, seen, dbs);
        catalog_scan_sqlite_dir("nntp2sql/nntp2sql/nntp2sql", FALSE, seen, dbs);
        g_hash_table_destroy(seen);
        g_ptr_array_sort(dbs, group_ptr_compare);
        return dbs;
    }

    if (!strcmp(type, "mysql") || !strcmp(type, "mariadb")) {
        CatalogConnParams p;
        catalog_parse_server_connection(type, connection, &p);
        MYSQL *db = catalog_mysql_connect(&p, NULL, error);
        if (db) {
            const char *schema_query =
                "SELECT a.table_schema FROM information_schema.columns a "
                "JOIN information_schema.columns g ON g.table_schema = a.table_schema "
                "WHERE a.table_schema NOT IN ('information_schema', 'mysql', 'performance_schema', 'sys') "
                "  AND LOWER(a.table_name) = 'articles' AND LOWER(a.column_name) IN ('artnum', 'group_name', 'subject') "
                "  AND LOWER(g.table_name) = 'groups' AND LOWER(g.column_name) IN ('name', 'article_count') "
                "GROUP BY a.table_schema "
                "HAVING COUNT(DISTINCT a.column_name) = 3 AND COUNT(DISTINCT g.column_name) = 2 "
                "ORDER BY a.table_schema";
            if (mysql_query(db, schema_query) == 0) {
                MYSQL_RES *res = mysql_store_result(db);
                if (res) {
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(res))) {
                        if (row[0] && *row[0]) g_ptr_array_add(dbs, g_strdup(row[0]));
                    }
                    mysql_free_result(res);
                }
            }
            mysql_close(db);
        }
        mysql_thread_end();
        catalog_conn_params_clear(&p);
        g_ptr_array_sort(dbs, group_ptr_compare);
        return dbs;
    }

    if (!strcmp(type, "postgres") || !strcmp(type, "pgsql")) {
        PgApi api;
        if (!catalog_load_pg_api(&api, error)) return dbs;
        CatalogConnParams p;
        catalog_parse_server_connection(type, connection, &p);
        GError *local_err = NULL;
        PGconn_t *conn = catalog_pg_connect(&api, &p, "postgres", &local_err);
        if (!conn) {
            g_clear_error(&local_err);
            conn = catalog_pg_connect(&api, &p, "template1", &local_err);
        }
        if (!conn) {
            g_clear_error(&local_err);
            conn = catalog_pg_connect(&api, &p, p.database, error);
        } else {
            g_clear_error(&local_err);
        }
        if (conn) {
            GPtrArray *candidates = g_ptr_array_new_with_free_func(g_free);
            PGresult_t *res = api.exec(conn, "SELECT datname FROM pg_database WHERE datistemplate = false ORDER BY datname");
            if (res && api.resultStatus(res) == 2 /* PGRES_TUPLES_OK */) {
                int n = api.ntuples(res);
                for (int i = 0; i < n; ++i) {
                    const char *dname = api.getvalue(res, i, 0);
                    if (dname && *dname) g_ptr_array_add(candidates, g_strdup(dname));
                }
            }
            if (res) api.clear(res);
            api.finish(conn);

            const char *pg_schema_check =
                "SELECT 1 FROM ("
                "  SELECT COUNT(DISTINCT column_name) FILTER (WHERE LOWER(table_name) = 'articles' AND LOWER(column_name) IN ('artnum', 'group_name', 'subject')) AS a, "
                "         COUNT(DISTINCT column_name) FILTER (WHERE LOWER(table_name) = 'groups' AND LOWER(column_name) IN ('name', 'article_count')) AS g "
                "  FROM information_schema.columns WHERE table_schema = 'public'"
                ") t WHERE a = 3 AND g = 2";
            for (guint i = 0; i < candidates->len; ++i) {
                const char *cand = g_ptr_array_index(candidates, i);
                PGconn_t *dconn = catalog_pg_connect(&api, &p, cand, NULL);
                if (!dconn) continue;
                PGresult_t *sres = api.exec(dconn, pg_schema_check);
                if (sres && api.resultStatus(sres) == 2 && api.ntuples(sres) > 0) {
                    g_ptr_array_add(dbs, g_strdup(cand));
                }
                if (sres) api.clear(sres);
                api.finish(dconn);
            }
            g_ptr_array_unref(candidates);
        }
        catalog_conn_params_clear(&p);
        g_ptr_array_sort(dbs, group_ptr_compare);
        return dbs;
    }

    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported database backend");
    return dbs;
}

/* Options live in a table named after the database (news0 -> news0_settings; a SQLite file data/news.db ->
 * news_settings), the same rule nntp2sql uses. Older databases keep them in a table called "settings", which is
 * read when the per-database table is missing or empty. Only letters, digits and '_' are kept. */
static char *catalog_settings_table(const char *db_name, gboolean is_file) {
    const char *base = db_name ? db_name : "";
    if (is_file) { const char *slash = strrchr(base, '/'); if (slash) base = slash + 1; }
    GString *stem = g_string_new(NULL);
    for (const char *p = base; *p && stem->len < 63; ++p) {
        if (is_file && *p == '.' && p > base && strchr(p + 1, '.') == NULL) break; /* file extension */
        g_string_append_c(stem, (g_ascii_isalnum(*p) || *p == '_') ? *p : '_');
    }
    if (!stem->len) g_string_assign(stem, "nntp2sql");
    g_string_append(stem, "_settings");
    return g_string_free(stem, FALSE);
}

static guint catalog_sqlite_settings(sqlite3 *db, const char *table, NewsCatalog *c) {
    char *sql = g_strdup_printf("SELECT key, value FROM \"%s\"", table);
    sqlite3_stmt *st = NULL; guint n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) { catalog_apply_setting(c, (const char *)sqlite3_column_text(st, 0), (const char *)sqlite3_column_text(st, 1)); n++; }
        sqlite3_finalize(st);
    }
    g_free(sql);
    return n;
}
static guint catalog_mysql_settings(MYSQL *db, const char *table, NewsCatalog *c) {
    char *sql = g_strdup_printf("SELECT `key`, `value` FROM `%s`", table);
    MYSQL_RES *res = NULL; guint n = 0;
    if (!mysql_query(db, sql) && (res = mysql_store_result(db))) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) { catalog_apply_setting(c, row[0], row[1]); n++; }
        mysql_free_result(res);
    }
    g_free(sql);
    return n;
}
static guint catalog_pg_settings(PgApi *api, PGconn_t *db, const char *table, NewsCatalog *c) {
    char *sql = g_strdup_printf("SELECT key, value FROM \"%s\"", table);
    PGresult_t *res = api->exec(db, sql); guint n = 0;
    if (res && api->resultStatus(res) == 2 /* PGRES_TUPLES_OK */)
        for (int i = 0; i < api->ntuples(res); ++i) { catalog_apply_setting(c, api->getvalue(res, i, 0), api->getvalue(res, i, 1)); n++; }
    if (res) api->clear(res);
    g_free(sql);
    return n;
}

/* Loads settings and articles. 'only_group' limits the articles to one group; 'groups_only' skips the
 * articles entirely and only collects the names of the groups that have some (fast, for the group picker). */
static NewsCatalog *catalog_load_ex(const char *type, const char *connection,
                                    GCancellable *cancel, GError **error,
                                    const char *only_group, gboolean groups_only) {
    /* Group names are only ever compared as quoted text; quotes are doubled (and backslashes refused for the
     * servers that treat them specially), so a group name can never alter the statement. */
    gboolean group_ok = !only_group || !strpbrk(only_group, "\\");
    char *pg_group = NULL;
    if (only_group && group_ok) {
        GString *q = g_string_new(NULL);
        for (const char *ch = only_group; *ch; ++ch) { if (*ch == '\'') g_string_append_c(q, '\''); g_string_append_c(q, *ch); }
        pg_group = g_string_free(q, FALSE);
    }
    char *query = groups_only ? g_strdup("SELECT DISTINCT group_name FROM articles ORDER BY group_name")
        : only_group ? g_strdup_printf("SELECT artnum,subject,author,date,group_name,message_id,refs FROM articles WHERE group_name = '%s' ORDER BY artnum", group_ok ? pg_group : "")
        : g_strdup("SELECT artnum,subject,author,date,group_name,message_id,refs FROM articles ORDER BY group_name,artnum");
    char *mysql_query_str = NULL; /* built once the connection exists, to escape the group name */
    const char *eff_conn = (connection && *connection) ? connection : catalog_default_connection(type);
    NewsCatalog *c = catalog_new();

    if (!strcmp(type, "sqlite")) {
        char *resolved_path = g_strdup(eff_conn);
        g_strstrip(resolved_path);
        if (g_str_has_prefix(resolved_path, "sqlite://")) {
            char * clean = g_strdup(resolved_path + 9);
            g_free(resolved_path);
            resolved_path = clean;
        }
        /* If default "nntp2sql.db" was used and does not exist yet, auto-select the first
         * discovered nntp2sql SQLite database in the directory (e.g. "news0.db"). */
        if ((!connection || !*connection || !strcmp(resolved_path, "nntp2sql.db")) &&
            !g_file_test(resolved_path, G_FILE_TEST_IS_REGULAR)) {
            GPtrArray *found = catalog_discover_databases("sqlite", resolved_path, NULL);
            if (found && found->len > 0) {
                g_free(resolved_path);
                resolved_path = g_strdup(g_ptr_array_index(found, 0));
            }
            if (found) g_ptr_array_unref(found);
        }
        c->resolved_database = g_strdup(resolved_path);
        c->resolved_connection = g_strdup(resolved_path);

        sqlite3 *db = NULL; sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_open_v2(resolved_path, &db, SQLITE_OPEN_READONLY, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_busy_timeout(db, 5000);
            sqlite3_exec(db, "PRAGMA temp_store=MEMORY; PRAGMA cache_size=-65536; PRAGMA mmap_size=268435456;", NULL, NULL, NULL);
            char *stable = catalog_settings_table(resolved_path, TRUE);
            if (!catalog_sqlite_settings(db, stable, c)) catalog_sqlite_settings(db, "settings", c); /* older databases */
            g_free(stable);
            /* The Group list is built from groups that have articles (see catalog_add), not from the
             * groups table, which after `nntp2sql --list-groups` holds every group on the server. */
            rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
        }
        if (rc == SQLITE_OK) {
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                if (g_cancellable_set_error_if_cancelled(cancel, error)) break;
                if (groups_only) { catalog_add_group(c, (const char *)sqlite3_column_text(stmt, 0)); continue; }
                const char *v[7]; for (int i = 0; i < 7; ++i) v[i] = (const char *)sqlite3_column_text(stmt, i);
                catalog_add(c, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
            }
        }
        if (rc != SQLITE_DONE && (!error || !*error))
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "SQLite (%s): %s",
                        resolved_path, db ? sqlite3_errmsg(db) : "cannot open database");
        sqlite3_finalize(stmt); if (db) sqlite3_close(db);
        g_free(resolved_path);
    } else if (!strcmp(type, "mysql") || !strcmp(type, "mariadb")) {
        CatalogConnParams p;
        catalog_parse_server_connection(type, eff_conn, &p);
        GError *conn_err = NULL;
        MYSQL *db = catalog_mysql_connect(&p, p.database, &conn_err);
        /* If the default database name ("nntp2sql") does not exist on the MySQL server,
         * automatically detect any nntp2sql-schema database (such as "news0") and open it! */
        if (!db && p.is_default_db) {
            GPtrArray *found = catalog_discover_databases("mysql", eff_conn, NULL);
            if (found && found->len > 0) {
                g_clear_error(&conn_err);
                g_free(p.database);
                p.database = g_strdup(g_ptr_array_index(found, 0));
                db = catalog_mysql_connect(&p, p.database, &conn_err);
            }
            if (found) g_ptr_array_unref(found);
        }
        if (!db) {
            if (conn_err) g_propagate_error(error, conn_err);
            else g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "MySQL: connection failed");
        } else {
            c->resolved_database = g_strdup(p.database);
            c->resolved_connection = g_strdup_printf("%s,%s,%s,%s,%u", p.host, p.database, p.user, p.password, p.port);
            MYSQL_RES *res = NULL;
            char *stable = catalog_settings_table(p.database, FALSE);
            if (!catalog_mysql_settings(db, stable, c)) catalog_mysql_settings(db, "settings", c); /* older databases */
            g_free(stable);
            if (groups_only) mysql_query_str = g_strdup("SELECT DISTINCT `group_name` FROM `articles` ORDER BY `group_name`");
            else if (only_group) {
                char *esc = g_malloc(strlen(only_group) * 2 + 1);
                mysql_real_escape_string(db, esc, only_group, (unsigned long)strlen(only_group));
                mysql_query_str = g_strdup_printf("SELECT `artnum`,`subject`,`author`,`date`,`group_name`,`message_id`,`refs` FROM `articles` WHERE `group_name` = '%s' ORDER BY `artnum`", esc);
                g_free(esc);
            } else mysql_query_str = g_strdup("SELECT `artnum`,`subject`,`author`,`date`,`group_name`,`message_id`,`refs` FROM `articles` ORDER BY `group_name`,`artnum`");
            if (mysql_query(db, mysql_query_str) || !(res = mysql_use_result(db))) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "MySQL (%s): %s", p.database, mysql_error(db));
            }
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res))) {
                    if (g_cancellable_set_error_if_cancelled(cancel, error)) break;
                    if (groups_only) { catalog_add_group(c, row[0]); continue; }
                    catalog_add(c, row[0], row[1], row[2], row[3], row[4], row[5], row[6]);
                }
                if ((!error || !*error) && mysql_errno(db))
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "MySQL (%s): %s", p.database, mysql_error(db));
                mysql_free_result(res);
            }
            mysql_close(db);
        }
        mysql_thread_end();
        catalog_conn_params_clear(&p);
    } else if (!strcmp(type, "postgres") || !strcmp(type, "pgsql")) {
        PgApi api;
        if (catalog_load_pg_api(&api, error)) {
            CatalogConnParams p;
            catalog_parse_server_connection(type, eff_conn, &p);
            GError *conn_err = NULL;
            PGconn_t *db = catalog_pg_connect(&api, &p, p.database, &conn_err);
            if (!db && p.is_default_db) {
                GPtrArray *found = catalog_discover_databases("postgres", eff_conn, NULL);
                if (found && found->len > 0) {
                    g_clear_error(&conn_err);
                    g_free(p.database);
                    p.database = g_strdup(g_ptr_array_index(found, 0));
                    db = catalog_pg_connect(&api, &p, p.database, &conn_err);
                }
                if (found) g_ptr_array_unref(found);
            }
            if (!db) {
                if (conn_err) g_propagate_error(error, conn_err);
                else g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "PostgreSQL: connection failed");
            } else {
                c->resolved_database = g_strdup(p.database);
                c->resolved_connection = g_strdup_printf("%s,%s,%s,%s,%u", p.host, p.database, p.user, p.password, p.port);
                char *stable = catalog_settings_table(p.database, FALSE);
                if (!catalog_pg_settings(&api, db, stable, c)) catalog_pg_settings(&api, db, "settings", c); /* older databases */
                g_free(stable);

                PGresult_t *res = api.exec(db, query);
                if (!res || api.resultStatus(res) != 2) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "PostgreSQL (%s): %s",
                                p.database, res ? api.resultErrorMessage(res) : api.errorMessage(db));
                } else {
                    int n = api.ntuples(res);
                    for (int i = 0; i < n; ++i) {
                        if (g_cancellable_set_error_if_cancelled(cancel, error)) break;
                        if (groups_only) { catalog_add_group(c, api.getvalue(res, i, 0)); continue; }
                        catalog_add(c, api.getvalue(res, i, 0), api.getvalue(res, i, 1), api.getvalue(res, i, 2),
                                    api.getvalue(res, i, 3), api.getvalue(res, i, 4), api.getvalue(res, i, 5),
                                    api.getvalue(res, i, 6));
                    }
                }
                if (res) api.clear(res);
                api.finish(db);
            }
            catalog_conn_params_clear(&p);
        }
    } else g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported database backend");
    g_free(query); g_free(mysql_query_str); g_free(pg_group);
    if (error && *error) { catalog_free(c); return NULL; }
    g_ptr_array_sort(c->groups, group_ptr_compare);
    return c;
}

/* Loads every article of every group (the original behaviour). */
static NewsCatalog *catalog_load(const char *type, const char *connection,
                                 GCancellable *cancel, GError **error) {
    return catalog_load_ex(type, connection, cancel, error, NULL, FALSE);
}
#endif

