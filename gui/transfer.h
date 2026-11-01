/* Worker pool + linked FIFO; binary trees index part numbers and byte ranges.
 * All worker-owned files are private staging files until validation succeeds. */
#ifndef VIEWER_TRANSFER_H
#define VIEWER_TRANSFER_H
#include "catalog.h"
#include "yenc.h"
#include <glib/gstdio.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct {
    char *host, *user, *password, *directory;
    guint port, workers, retries, timeout;
    guint security; /* 0: TLS, 1: STARTTLS, 2: explicit plaintext */
} TransferSettings;
typedef struct {
    GMutex mutex;
    guint done, total;
    guint files_done, files_total;
    guint64 bytes;
    const char *current_title;
} TransferProgress;
typedef struct {
    GIOStream *stream;
    GBufferedInputStream *input;
    GCancellable *cancel;
} NewsConnection;
typedef struct {
    NewsArticle *article;
    char *path;
    YencDecoder decoder;
    GError *error;
} PartJob;
typedef struct {
    GQueue jobs; /* doubly linked list, protected by mutex */
    GMutex mutex;
    const TransferSettings *settings;
    TransferProgress *progress;
    GCancellable *cancel;
    const char *temporary;
} TransferPool;

static void settings_clear(TransferSettings *s) {
    g_free(s->host); g_free(s->user); g_free(s->password); g_free(s->directory);
}
static void news_close(NewsConnection *n) {
    g_clear_object(&n->input);
    if (n->stream) { g_io_stream_close(n->stream, NULL, NULL); g_clear_object(&n->stream); }
}
/* Bounded, binary-safe line reader; yEnc data must never pass through strlen. */
static GByteArray *news_line(NewsConnection *n, GError **error) {
    GByteArray *line = g_byte_array_new();
    for (;;) {
        int ch = g_buffered_input_stream_read_byte(n->input, n->cancel, error);
        if (ch < 0) {
            if (!error || !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "NNTP connection ended before response completed");
            g_byte_array_unref(line); return NULL;
        }
        if (ch == '\n') {
            if (!line->len || line->data[line->len-1] != '\r') {
                yenc_error(error, "NNTP line lacks CRLF"); g_byte_array_unref(line); return NULL;
            }
            g_byte_array_set_size(line, line->len-1); return line;
        }
        guint8 byte = (guint8)ch; g_byte_array_append(line, &byte, 1);
        if (line->len > 65536) { yenc_error(error, "NNTP line exceeds 64 KiB"); g_byte_array_unref(line); return NULL; }
    }
}
static int news_response(NewsConnection *n, GError **error) {
    GByteArray *line = news_line(n, error);
    if (!line) return -1;
    int code = -1;
    if (line->len >= 3 && g_ascii_isdigit(line->data[0]) && g_ascii_isdigit(line->data[1]) && g_ascii_isdigit(line->data[2]))
        code = (line->data[0]-'0')*100 + (line->data[1]-'0')*10 + line->data[2]-'0';
    else yenc_error(error, "Invalid NNTP status");
    g_byte_array_unref(line); return code;
}
static int news_command(NewsConnection *n, const char *command, GError **error) {
    char *wire = g_strconcat(command, "\r\n", NULL);
    gboolean ok = g_output_stream_write_all(g_io_stream_get_output_stream(n->stream), wire, strlen(wire), NULL, n->cancel, error);
    g_free(wire);
    return ok ? news_response(n, error) : -1;
}
static gboolean no_newline(const char *s) {
    return s && !strchr(s, '\r') && !strchr(s, '\n');
}
static gboolean news_connect(NewsConnection *n, const TransferSettings *s, GCancellable *cancel, GError **error) {
    n->cancel = cancel;
    if (!s->host || !*s->host || !no_newline(s->user) || !no_newline(s->password))
        return yenc_error(error, "Invalid NNTP host or credentials");
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, s->timeout ? s->timeout : 20);
    g_socket_client_set_tls(client, s->security == 0);
    GSocketConnection *socket = g_socket_client_connect_to_host(client, s->host, (guint16)s->port, cancel, error);
    g_object_unref(client);
    if (!socket) return FALSE;
    n->stream = G_IO_STREAM(socket);
    n->input = G_BUFFERED_INPUT_STREAM(g_buffered_input_stream_new(g_io_stream_get_input_stream(n->stream)));
    int code = news_response(n, error);
    if (code != 200 && code != 201) goto rejected;
    if (s->security == 1) {
        code = news_command(n, "STARTTLS", error);
        if (code != 382) goto rejected;
        GSocketConnectable *identity = g_network_address_new(s->host, (guint16)s->port);
        GIOStream *tls = g_tls_client_connection_new(n->stream, identity, error);
        g_object_unref(identity);
        if (!tls) goto rejected;
        g_clear_object(&n->input); g_object_unref(n->stream); n->stream = tls;
        if (!g_tls_connection_handshake(G_TLS_CONNECTION(tls), cancel, error)) goto rejected;
        n->input = G_BUFFERED_INPUT_STREAM(g_buffered_input_stream_new(g_io_stream_get_input_stream(n->stream)));
    }
    if (*s->user) {
        char *command = g_strconcat("AUTHINFO USER ", s->user, NULL);
        code = news_command(n, command, error); g_free(command);
        if (code == 381) {
            command = g_strconcat("AUTHINFO PASS ", s->password, NULL);
            code = news_command(n, command, error); g_free(command);
        }
        if (code != 281) goto rejected;
    }
    return TRUE;
rejected:
    if (!error || !*error) g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "NNTP connection/authentication rejected (%d)", code);
    news_close(n); return FALSE;
}
static gboolean news_body(NewsConnection *n, NewsArticle *a, GError **error) {
    char *command = NULL;
    if (*a->message) {
        gsize length = strlen(a->message);
        if (length < 3 || length > 998 || a->message[0] != '<' || a->message[length-1] != '>' ||
            strpbrk(a->message, "\r\n \t")) return yenc_error(error, "Invalid Message-ID in database");
        command = g_strconcat("BODY ", a->message, NULL);
    } else {
        if (!*a->group || a->number < 1 || strspn(a->group, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-") != strlen(a->group))
            return yenc_error(error, "No valid Message-ID or group/article number");
        command = g_strconcat("GROUP ", a->group, NULL);
        int code = news_command(n, command, error); g_free(command);
        if (code != 211) {
            if (!error || !*error) g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "NNTP group unavailable (%d)", code);
            return FALSE;
        }
        command = g_strdup_printf("BODY %" G_GINT64_FORMAT, a->number);
    }
    int code = news_command(n, command, error); g_free(command);
    if (code == 222) return TRUE;
    if (!error || !*error) g_set_error(error, G_IO_ERROR, code == 430 || code == 423 ? G_IO_ERROR_NOT_FOUND : G_IO_ERROR_FAILED, "Article body unavailable (%d)", code);
    return FALSE;
}
static gboolean fetch_part(NewsConnection *n, PartJob *job, GError **error) {
    if (!news_body(n, job->article, error)) return FALSE;
    for (;;) {
        GByteArray *line = news_line(n, error);
        if (!line) return FALSE;
        if (line->len == 1 && line->data[0] == '.') {
            g_byte_array_unref(line);
            return job->decoder.ended ? TRUE : yenc_error(error, "Incomplete or non-yEnc article body");
        }
        const guint8 *bytes = line->data; gsize length = line->len;
        if (length && bytes[0] == '.') { bytes++; length--; }
        gboolean ok = yenc_line(&job->decoder, bytes, length, job->article->part, job->article->total, error);
        g_byte_array_unref(line);
        if (!ok) return FALSE;
    }
}
static gpointer part_worker(gpointer data) {
    TransferPool *pool = data;
    NewsConnection connection = {0};
    for (;;) {
        g_mutex_lock(&pool->mutex);
        PartJob *job = g_queue_pop_head(&pool->jobs);
        g_mutex_unlock(&pool->mutex);
        if (!job) break;
        for (guint attempt = 0; attempt <= pool->settings->retries; ++attempt) {
            g_clear_error(&job->error);
            if (g_cancellable_set_error_if_cancelled(pool->cancel, &job->error)) break;
            g_free(job->decoder.name); memset(&job->decoder, 0, sizeof(job->decoder));
            if (!connection.stream && !news_connect(&connection, pool->settings, pool->cancel, &job->error)) continue;
            job->decoder.output = g_fopen(job->path, "wb");
            if (!job->decoder.output) {
                g_set_error(&job->error, G_IO_ERROR, G_IO_ERROR_FAILED, "Opening part file: %s", g_strerror(errno)); break;
            }
            gboolean ok = fetch_part(&connection, job, &job->error);
            if (fclose(job->decoder.output) && ok) {
                g_set_error(&job->error, G_IO_ERROR, G_IO_ERROR_FAILED, "Flushing part file: %s", g_strerror(errno)); ok = FALSE;
            }
            job->decoder.output = NULL;
            if (ok) break;
            news_close(&connection);
            if (g_error_matches(job->error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
                g_error_matches(job->error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) break;
        }
        g_mutex_lock(&pool->progress->mutex);
        pool->progress->done++;
        if (!job->error) pool->progress->bytes += job->decoder.written;
        g_mutex_unlock(&pool->progress->mutex);
    }
    news_close(&connection); return NULL;
}
static gint offset_compare(gconstpointer a, gconstpointer b, gpointer unused) {
    (void)unused; guint64 x = *(const guint64 *)a, y = *(const guint64 *)b;
    return (x > y) - (x < y);
}
static void part_job_free(gpointer value) {
    PartJob *job = value;
    if (job->path) g_unlink(job->path);
    g_free(job->path); g_free(job->decoder.name); g_clear_error(&job->error); g_free(job);
}
typedef struct {
    FILE *output;
    guint64 next;
    guint32 crc;
    GCancellable *cancel;
    GError **error;
    gboolean ok;
} Assembly;
static gboolean assemble_part(gpointer key, gpointer value, gpointer data) {
    (void)key;
    PartJob *job = value; Assembly *a = data;
    if (job->decoder.begin != a->next) { a->ok = yenc_error(a->error, "Missing or overlapping yEnc byte ranges"); return TRUE; }
    FILE *input = g_fopen(job->path, "rb");
    if (!input) { g_set_error(a->error, G_IO_ERROR, G_IO_ERROR_FAILED, "Opening decoded part: %s", g_strerror(errno)); a->ok = FALSE; return TRUE; }
    guint8 buffer[65536]; size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), input))) {
        if (g_cancellable_set_error_if_cancelled(a->cancel, a->error)) { a->ok = FALSE; break; }
        if (fwrite(buffer, 1, count, a->output) != count) {
            g_set_error(a->error, G_IO_ERROR, G_IO_ERROR_FAILED, "Writing output: %s", g_strerror(errno)); a->ok = FALSE; break;
        }
        a->crc = (guint32)crc32(a->crc, buffer, (uInt)count);
    }
    if (ferror(input) && a->ok) { g_set_error_literal(a->error, G_IO_ERROR, G_IO_ERROR_FAILED, "Reading decoded part failed"); a->ok = FALSE; }
    fclose(input);
    a->next = job->decoder.end + 1;
    return !a->ok;
}
/* Synchronous coordinator, called from a GTask, never the GTK thread. */
static char *download_file(NewsFile *file, const TransferSettings *settings,
                           TransferProgress *progress, GCancellable *cancel, GError **error) {
    if (!file_has_headers(file)) { yenc_error(error, "Cannot download: missing or ambiguous part headers"); return NULL; }
    if (!settings->workers || settings->workers > 64 || !settings->port || settings->port > 65535 ||
        settings->retries > 10 || settings->security > 2) { yenc_error(error, "Invalid download settings"); return NULL; }
    if (g_mkdir_with_parents(settings->directory, 0700) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Download folder: %s", g_strerror(errno)); return NULL;
    }
    char *temporary = g_build_filename(settings->directory, ".nntp2sql-XXXXXX", NULL);
    if (!g_mkdtemp(temporary)) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Staging folder: %s", g_strerror(errno)); g_free(temporary); return NULL; }
    TransferPool pool = {0};
    g_mutex_init(&pool.mutex); g_queue_init(&pool.jobs);
    pool.settings = settings; pool.progress = progress; pool.cancel = cancel; pool.temporary = temporary;
    GPtrArray *jobs = g_ptr_array_new_with_free_func(part_job_free);
    for (guint i = 1; i <= file->total; ++i) {
        PartJob *job = g_new0(PartJob, 1);
        job->article = g_tree_lookup(file->parts, GUINT_TO_POINTER(i));
        char *leaf = g_strdup_printf("%u.part", i); job->path = g_build_filename(temporary, leaf, NULL); g_free(leaf);
        g_ptr_array_add(jobs, job); g_queue_push_tail(&pool.jobs, job);
    }
    g_mutex_lock(&progress->mutex); progress->done = 0; progress->total = file->total; progress->current_title = file->title; g_mutex_unlock(&progress->mutex);
    GThread *threads[64]; guint started = 0;
    for (guint i = 0; i < MIN(settings->workers, file->total); ++i) {
        GError *thread_error = NULL;
        GThread *thread = g_thread_try_new("yenc-part", part_worker, &pool, &thread_error);
        if (thread) threads[started++] = thread;
        g_clear_error(&thread_error);
    }
    if (!started) part_worker(&pool);
    for (guint i = 0; i < started; ++i) g_thread_join(threads[i]);
    GTree *ranges = g_tree_new_full(offset_compare, NULL, NULL, NULL);
    gboolean ok = !g_cancellable_set_error_if_cancelled(cancel, error);
    PartJob *first = g_ptr_array_index(jobs, 0);
    gboolean has_full_crc = FALSE; guint32 full_crc = 0;
    for (guint i = 0; ok && i < jobs->len; ++i) {
        PartJob *job = g_ptr_array_index(jobs, i);
        if (job->error) { g_propagate_error(error, g_error_copy(job->error)); ok = FALSE; break; }
        if (!job->decoder.ended || job->decoder.size != first->decoder.size || g_strcmp0(job->decoder.name, first->decoder.name) ||
            g_tree_lookup(ranges, &job->decoder.begin)) { ok = yenc_error(error, "Conflicting yEnc filenames, sizes, or ranges"); break; }
        if (job->decoder.has_full_crc) {
            if (has_full_crc && full_crc != job->decoder.full_crc) { ok = yenc_error(error, "Conflicting full-file checksums"); break; }
            has_full_crc = TRUE; full_crc = job->decoder.full_crc;
        }
        g_tree_insert(ranges, &job->decoder.begin, job);
    }
    char *assembled = g_build_filename(temporary, "assembled", NULL), *result = NULL;
    if (ok) {
        FILE *output = g_fopen(assembled, "wb");
        if (!output) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Creating output: %s", g_strerror(errno)); ok = FALSE; }
        else {
            Assembly a = {output, 1, (guint32)crc32(0, NULL, 0), cancel, error, TRUE};
            g_tree_foreach(ranges, assemble_part, &a);
            ok = a.ok;
            if (ok && (a.next != first->decoder.size + 1 || (has_full_crc && a.crc != full_crc)))
                ok = yenc_error(error, "Final size or full-file CRC32 verification failed");
            if (ok && (fflush(output) || fsync(fileno(output)))) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Flushing output: %s", g_strerror(errno)); ok = FALSE; }
            if (fclose(output) && ok) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Closing output failed"); ok = FALSE; }
        }
    }
    if (ok && !g_cancellable_set_error_if_cancelled(cancel, error)) {
        char *destination = g_build_filename(settings->directory, first->decoder.name, NULL);
        /* Hard-link publication is atomic and refuses to replace an existing file.
         * Staging is on the same filesystem as the selected destination. */
        if (link(assembled, destination) == 0) result = destination;
        else { g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Saving file (existing files are not overwritten): %s", g_strerror(errno)); g_free(destination); }
    }
    g_unlink(assembled); g_free(assembled); g_tree_destroy(ranges);
    g_ptr_array_unref(jobs); g_queue_clear(&pool.jobs); g_mutex_clear(&pool.mutex);
    g_rmdir(temporary); g_free(temporary);
    return result;
}

/* Batch/queue coordinator capable of processing upwards of 2,000,000 queued files. */
static char *download_queued_files(GPtrArray *files, const TransferSettings *settings,
                                   TransferProgress *progress, GCancellable *cancel, GError **error) {
    if (!files || !files->len) {
        yenc_error(error, "No files in download queue");
        return NULL;
    }
    g_mutex_lock(&progress->mutex);
    progress->files_done = 0;
    progress->files_total = files->len;
    g_mutex_unlock(&progress->mutex);
    guint ok_count = 0;
    GError *first_error = NULL;
    char *last_saved = NULL;
    for (guint i = 0; i < files->len; ++i) {
        if (g_cancellable_set_error_if_cancelled(cancel, &first_error)) break;
        NewsFile *f = g_ptr_array_index(files, i);
        gint64 started = g_get_monotonic_time();
        GError *file_error = NULL;
        char *saved = download_file(f, settings, progress, cancel, &file_error);
        f->elapsed = (g_get_monotonic_time() - started) / 1000000.0;
        g_free(f->status);
        if (saved) {
            f->status = g_strdup("Complete — decoded sizes and CRCs verified");
            g_free(f->saved_path);
            f->saved_path = g_strdup(saved);
            f->queued = FALSE;
            g_free(last_saved);
            last_saved = saved;
            ok_count++;
        } else {
            f->status = g_strdup_printf("Incomplete — %s", file_error ? file_error->message : "Download failed");
            if (!first_error && file_error) first_error = file_error;
            else g_clear_error(&file_error);
        }
        g_mutex_lock(&progress->mutex);
        progress->files_done = i + 1;
        g_mutex_unlock(&progress->mutex);
        if (g_cancellable_is_cancelled(cancel)) break;
    }
    if (files->len == 1) {
        if (ok_count == 1) {
            g_clear_error(&first_error);
            return last_saved;
        }
        g_free(last_saved);
        if (first_error) g_propagate_error(error, first_error);
        else yenc_error(error, "Download failed");
        return NULL;
    }
    g_free(last_saved);
    if (ok_count > 0) {
        g_clear_error(&first_error);
        if (ok_count == files->len)
            return g_strdup_printf("%u/%u queued files downloaded to %s", ok_count, files->len, settings->directory);
        return g_strdup_printf("%u/%u queued files downloaded (%u failed) in %s",
                               ok_count, files->len, files->len - ok_count, settings->directory);
    }
    if (first_error) g_propagate_error(error, first_error);
    else yenc_error(error, "All queued file downloads failed");
    return NULL;
}
#endif
