/* Streaming yEnc decoder: never publish unverified or partially decoded data. */
#ifndef VIEWER_YENC_H
#define VIEWER_YENC_H
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    gboolean begun, ranged, ended, multipart, has_full_crc;
    guint part, total;
    guint64 size, begin, end, written;
    guint32 crc, full_crc;
    char *name;
    FILE *output;
} YencDecoder;

static gboolean yenc_error(GError **error, const char *message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}
/* Match complete attribute names, not e.g. crc32 inside pcrc32 or name=. */
static const char *yenc_attribute(const char *line, const char *key) {
    gsize n = strlen(key);
    const char *p = line;
    while ((p = strchr(p, ' '))) {
        ++p;
        if (!strncmp(p, "name=", 5)) return !strcmp(key, "name") ? p + 5 : NULL;
        if (!strncmp(p, key, n) && p[n] == '=') return p + n + 1;
    }
    return NULL;
}
static gboolean yenc_number(const char *line, const char *key, guint64 *out) {
    const char *v = yenc_attribute(line, key);
    if (!v || !g_ascii_isdigit(*v)) return FALSE;
    errno = 0; char *end;
    guint64 value = g_ascii_strtoull(v, &end, 10);
    if (errno || (*end && *end != ' ')) return FALSE;
    *out = value; return TRUE;
}
static gboolean yenc_crc(const char *line, const char *key, guint32 *out) {
    const char *v = yenc_attribute(line, key);
    if (!v || strlen(v) < 8) return FALSE;
    for (int i = 0; i < 8; ++i) if (!g_ascii_isxdigit(v[i])) return FALSE;
    if (v[8] && v[8] != ' ') return FALSE;
    *out = (guint32)g_ascii_strtoull(v, NULL, 16); return TRUE;
}
static gboolean safe_filename(const char *name) {
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..") ||
        !g_utf8_validate(name, -1, NULL) || strlen(name) > 240) return FALSE;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\' || *p == ':') return FALSE;
    return TRUE;
}
static gboolean yenc_line(YencDecoder *d, const guint8 *bytes, gsize length,
                          guint expected_part, guint expected_total, GError **error) {
    char *line = g_strndup((const char *)bytes, length);
    gboolean ok = TRUE;
    if (length >= 8 && !memcmp(bytes, "=ybegin ", 8)) {
        if (d->begun || memchr(bytes, 0, length)) { g_free(line); return yenc_error(error, "Multiple or invalid yEnc blocks"); }
        guint64 size, part = 1, total = expected_total, width;
        const char *name = yenc_attribute(line, "name");
        d->multipart = yenc_attribute(line, "part") != NULL;
        ok = yenc_number(line, "size", &size) && size <= G_GUINT64_CONSTANT(1099511627776) &&
             yenc_number(line, "line", &width) && width > 0 && width <= 65536 &&
             safe_filename(name);
        if (d->multipart) ok = ok && yenc_number(line, "part", &part);
        if (yenc_attribute(line, "total")) ok = ok && yenc_number(line, "total", &total);
        ok = ok && part == expected_part && total == expected_total &&
             (d->multipart || expected_total == 1);
        if (ok) {
            d->begun = TRUE; d->size = size; d->part = (guint)part; d->total = (guint)total;
            d->name = g_strdup(name); d->begin = 1; d->end = size; d->crc = (guint32)crc32(0, NULL, 0);
        } else yenc_error(error, "Invalid yEnc header, filename, size, or part number");
    } else if (length >= 7 && !memcmp(bytes, "=ypart ", 7)) {
        ok = d->begun && d->multipart && !d->ranged && !d->written && !d->ended &&
             yenc_number(line, "begin", &d->begin) && yenc_number(line, "end", &d->end) &&
             d->begin >= 1 && d->end >= d->begin && d->end <= d->size;
        if (ok) d->ranged = TRUE;
        else yenc_error(error, "Invalid yEnc byte range");
    } else if (length >= 6 && !memcmp(bytes, "=yend ", 6)) {
        guint64 size = 0, part = d->part;
        guint32 checksum = 0;
        ok = d->begun && !d->ended && (!d->multipart || d->ranged) &&
             yenc_number(line, "size", &size) && size == d->written &&
             d->written == (d->size == 0 ? 0 : d->end - d->begin + 1);
        if (yenc_attribute(line, "part")) ok = ok && yenc_number(line, "part", &part) && part == d->part;
        ok = ok && yenc_crc(line, d->multipart ? "pcrc32" : "crc32", &checksum) && checksum == d->crc;
        d->has_full_crc = yenc_crc(line, "crc32", &d->full_crc);
        if (ok) d->ended = TRUE;
        else yenc_error(error, "yEnc size or CRC32 verification failed (checksum required)");
    } else if (d->begun && !d->ended) {
        if (d->multipart && !d->ranged) ok = yenc_error(error, "Missing yEnc part range");
        else {
            guint8 *decoded = g_malloc(length + 1); gsize used = 0;
            for (gsize i = 0; i < length; ++i) {
                guint8 ch = bytes[i];
                if (ch == '=') {
                    if (++i == length) { ok = yenc_error(error, "Truncated yEnc escape"); break; }
                    ch = (guint8)(bytes[i] - 64);
                }
                decoded[used++] = (guint8)(ch - 42);
            }
            guint64 available = d->size == 0 ? 0 : d->end - d->begin + 1;
            if (ok && (d->written > available || used > available - d->written))
                ok = yenc_error(error, "Decoded data exceeds declared size");
            if (ok && fwrite(decoded, 1, used, d->output) != used) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Writing part: %s", g_strerror(errno)); ok = FALSE;
            }
            if (ok) { d->crc = (guint32)crc32(d->crc, decoded, (uInt)used); d->written += used; }
            g_free(decoded);
        }
    }
    g_free(line); return ok;
}
#endif
