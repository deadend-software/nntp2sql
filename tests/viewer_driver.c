/* Headless integration harness using exactly the viewer's catalog and downloader. */
#include "../gui/transfer.h"

typedef struct { GCancellable *cancel; guint delay; } CancelRequest;
static gpointer cancel_later(gpointer data) {
    CancelRequest *r = data;
    g_usleep((gulong)r->delay * 1000);
    g_cancellable_cancel(r->cancel);
    return NULL;
}
int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "defaults")) {
        g_print("sqlite=%s\n", catalog_default_connection("sqlite"));
        g_print("mysql=%s\n", catalog_default_connection("mysql"));
        g_print("postgres=%s\n", catalog_default_connection("postgres"));
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "discover")) {
        GError *err = NULL;
        const char *conn = argc >= 4 ? argv[3] : catalog_default_connection(argv[2]);
        GPtrArray *dbs = catalog_discover_databases(argv[2], conn, &err);
        if (err && dbs->len == 0) {
            g_printerr("%s\n", err->message);
            g_clear_error(&err);
            g_ptr_array_unref(dbs);
            return 1;
        }
        g_clear_error(&err);
        for (guint i = 0; i < dbs->len; ++i)
            g_print("%s\n", (const char *)g_ptr_array_index(dbs, i));
        g_ptr_array_unref(dbs);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "parse-conn")) {
        CatalogConnParams p;
        catalog_parse_server_connection(argv[2], argv[3], &p);
        g_print("host=%s db=%s user=%s pass=%s port=%u def_db=%d def_user=%d def_host=%d\n",
                p.host, p.database, p.user, p.password, p.port,
                p.is_default_db ? 1 : 0, p.is_default_user ? 1 : 0, p.is_default_host ? 1 : 0);
        catalog_conn_params_clear(&p);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "format-conn")) {
        char *out = catalog_format_connection_for_db(argv[2], argv[3], argv[4]);
        g_print("%s\n", out);
        g_free(out);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "inspect-db")) {
        GError *err = NULL;
        GCancellable *cancel = g_cancellable_new();
        const char *conn = argc >= 4 ? argv[3] : "";
        NewsCatalog *c = catalog_load(argv[2], conn, cancel, &err);
        if (!c) {
            g_printerr("%s\n", err ? err->message : "load failed");
            g_clear_error(&err);
            g_object_unref(cancel);
            return 1;
        }
        g_print("db=%s conn=%s host=%s port=%u workers=%u groups=%u articles=%u files=%u\n",
                c->resolved_database ? c->resolved_database : "",
                c->resolved_connection ? c->resolved_connection : "",
                c->default_host ? c->default_host : "",
                c->default_port, c->default_workers,
                c->groups->len, c->articles->len, c->files->len);
        catalog_free(c);
        g_object_unref(cancel);
        return 0;
    }
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "scale-check")) {
        guint total = (guint)strtoul(argv[2], NULL, 10);
        NewsCatalog *c = catalog_new();
        for (guint i = 1; i <= total; ++i) {
            char num[32], subj[128], msg[64];
            g_snprintf(num, sizeof(num), "%u", i);
            g_snprintf(subj, sizeof(subj), "Release_%u \"payload_%u.bin\" yEnc (1/1)", i, i);
            g_snprintf(msg, sizeof(msg), "<part-%u@test>", i);
            catalog_add(c, num, subj, i % 2 == 0 ? "alice@example.com" : "bob@example.com",
                        "2026-04-20", i % 3 == 0 ? "alt.binaries.boneless" : "alt.binaries.test", msg);
        }
        GPtrArray *queue = g_ptr_array_sized_new(c->files->len);
        for (guint i = 0; i < c->files->len; ++i) {
            NewsFile *f = g_ptr_array_index(c->files, i);
            f->queued = TRUE;
            g_ptr_array_add(queue, f);
        }
        guint hits = 0;
        for (guint i = 0; i < c->files->len; ++i) {
            NewsFile *f = g_ptr_array_index(c->files, i);
            if (catalog_file_matches(f, "payload_2000000.bin queued complete")) hits++;
        }
        g_print("SCALE_OK %u %u %u %u\n", c->articles->len, c->files->len, queue->len, hits);
        g_ptr_array_unref(queue);
        catalog_free(c);
        return 0;
    }
    GError *error = NULL;
    GCancellable *cancel = g_cancellable_new();
    NewsCatalog *c = catalog_load("sqlite", argv[2], cancel, &error);
    if (!c) { g_printerr("%s\n", error->message); g_clear_error(&error); g_object_unref(cancel); return 1; }
    if (!strcmp(argv[1], "inspect")) {
        g_print("%u %u\n", c->articles->len, c->files->len);
        for (guint i = 0; i < c->files->len; ++i) {
            NewsFile *f = g_ptr_array_index(c->files, i);
            g_print("%u %d %d\n", f->total, g_tree_nnodes(f->parts), file_has_headers(f));
        }
        catalog_free(c); g_object_unref(cancel); return 0;
    }
    if (!strcmp(argv[1], "groups")) {
        for (guint i = 0; i < c->groups->len; ++i)
            g_print("%s\n", (const char *)g_ptr_array_index(c->groups, i));
        catalog_free(c); g_object_unref(cancel); return 0;
    }
    if (!strcmp(argv[1], "search-files") && argc >= 4) {
        for (guint i = 0; i < c->files->len; ++i) {
            NewsFile *f = g_ptr_array_index(c->files, i);
            if (argc >= 5 && !strcmp(argv[4], "queue-even") && (i % 2 == 0)) f->queued = TRUE;
            if (catalog_file_matches(f, argv[3])) g_print("%s\n", f->title);
        }
        catalog_free(c); g_object_unref(cancel); return 0;
    }
    if (!strcmp(argv[1], "search-articles") && argc >= 4) {
        for (guint i = 0; i < c->articles->len; ++i) {
            NewsArticle *a = g_ptr_array_index(c->articles, i);
            if (catalog_article_matches(a, argv[3])) g_print("%" G_GINT64_FORMAT "\n", a->number);
        }
        catalog_free(c); g_object_unref(cancel); return 0;
    }
    if (!strcmp(argv[1], "queue-download") && argc >= 8) {
        TransferSettings s = {0};
        s.directory = g_strdup(argv[3]); s.host = g_strdup(argv[4]); s.port = (guint)atoi(argv[5]);
        s.security = (guint)atoi(argv[6]); s.workers = (guint)atoi(argv[7]); s.retries = 1; s.timeout = 2;
        s.user = g_strdup(g_getenv("TEST_NNTP_USER") ? g_getenv("TEST_NNTP_USER") : "");
        s.password = g_strdup(g_getenv("TEST_NNTP_PASS") ? g_getenv("TEST_NNTP_PASS") : "");
        GPtrArray *queue = g_ptr_array_new();
        for (guint i = 0; i < c->files->len; ++i) {
            NewsFile *f = g_ptr_array_index(c->files, i);
            if (file_has_headers(f)) {
                f->queued = TRUE;
                g_ptr_array_add(queue, f);
            }
        }
        TransferProgress progress = {0}; g_mutex_init(&progress.mutex);
        char *summary = download_queued_files(queue, &s, &progress, cancel, &error);
        int result = summary ? 0 : 1;
        if (summary) g_print("QUEUE_OK %u/%u %s\n", progress.files_done, progress.files_total, summary);
        else g_printerr("%s\n", error ? error->message : "unknown failure");
        g_free(summary); g_ptr_array_unref(queue); g_clear_error(&error); g_mutex_clear(&progress.mutex);
        settings_clear(&s); catalog_free(c); g_object_unref(cancel);
        return result;
    }
    if (argc < 8 || c->files->len != 1) { catalog_free(c); g_object_unref(cancel); return 2; }
    TransferSettings s = {0};
    s.directory = g_strdup(argv[3]); s.host = g_strdup(argv[4]); s.port = (guint)atoi(argv[5]);
    s.security = (guint)atoi(argv[6]); s.workers = (guint)atoi(argv[7]); s.retries = 1; s.timeout = 2;
    s.user = g_strdup(g_getenv("TEST_NNTP_USER") ? g_getenv("TEST_NNTP_USER") : "");
    s.password = g_strdup(g_getenv("TEST_NNTP_PASS") ? g_getenv("TEST_NNTP_PASS") : "");
    TransferProgress progress = {0}; g_mutex_init(&progress.mutex);
    CancelRequest request = {cancel, argc > 8 ? (guint)atoi(argv[8]) : 0};
    GThread *thread = request.delay ? g_thread_new("cancel", cancel_later, &request) : NULL;
    char *path = download_file(g_ptr_array_index(c->files, 0), &s, &progress, cancel, &error);
    if (thread) g_thread_join(thread);
    int result = path ? 0 : 1;
    if (path) g_print("SAVED %s\n", path);
    else g_printerr("%s\n", error ? error->message : "unknown failure");
    g_free(path); g_clear_error(&error); g_mutex_clear(&progress.mutex);
    settings_clear(&s); catalog_free(c); g_object_unref(cancel);
    return result;
}
